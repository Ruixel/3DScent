// =============================================================================
// ndsfunc.c — 3DS rendering backend
// =============================================================================
// Renders the game's 320x200 palette-indexed back_buffer to the top screen by:
//   1. Converting palette indices to RGBA8 in a linear staging buffer
//   2. Software-tiling the staging buffer into a PICA200 tiled texture
//   3. Drawing a textured quad covering the top screen
//
// Palette index 0 is treated as transparent (alpha blended against clear color).
//
// Debug render modes are available via RENDER_MODE — leave on BITBLT for
// normal gameplay. See "Debug modes" section at the bottom of this file.
// =============================================================================

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "3ds.h"
#include <citro3d.h>

#include "fix.h"
#include "gr.h"
#include "key.h"
#include "mouse.h"
#include "3d.h"
#include "game.h"
#include "grdef.h"
#include "error.h"
#include "mem.h"
#include "ndsfunc.h"
#include "softkey.h"
#include "vshader_shbin.h"
#include "vshader_wire_shbin.h"

// -----------------------------------------------------------------------------
// Render mode selection
// -----------------------------------------------------------------------------
#define RENDER_MODE_BITBLT       0  // Normal gameplay: palette -> RGBA -> quad
#define RENDER_MODE_TRIANGLE     1  // Debug: colored triangle (requires color shader)
#define RENDER_MODE_SOLID        2  // Debug: solid-red quad
#define RENDER_MODE_CHECKERBOARD 3  // Debug: 32x32 red/white checker
#define RENDER_MODE_RAW_WRITE    4  // Debug: bypass tiler, write tex->data directly

#define RENDER_MODE  RENDER_MODE_BITBLT

// -----------------------------------------------------------------------------
// Lighting constants (used by game code, not rendering)
// -----------------------------------------------------------------------------
#define NUM_LIGHTING_LEVELS 32
#define MAX_LIGHTING_VALUE  ((NUM_LIGHTING_LEVELS-1)*F1_0/NUM_LIGHTING_LEVELS)
int Lighting_on;
int Max_perspective_depth, Max_linear_depth, Current_seg_depth;

// -----------------------------------------------------------------------------
// Framebuffer state shared with game code
// -----------------------------------------------------------------------------
ALIGN(4) u8 back_buffer[400 * 240];
u16         ds_palette[256];
int         palette_updated;
bool        doSleep;

// -----------------------------------------------------------------------------
// GPU state
// -----------------------------------------------------------------------------
static C3D_RenderTarget* target;
static DVLB_s*           vshader_dvlb;
static shaderProgram_s   program;
static int               uLoc_projection;
static C3D_Mtx           projection;
static void*             vbo_data;

// Top screen is 240x400 physical, rendered as 400x240 landscape via OrthoTilt.
#define SCREEN_W  400
#define SCREEN_H  240

// -----------------------------------------------------------------------------
// Game screen dimensions and the on-screen quad region.
// Hoisted here (above the render-mode switch) because the wireframe overlay
// needs these values regardless of mode.
// -----------------------------------------------------------------------------
#define TEX_W     512   // PICA200 requires power-of-two textures
#define TEX_H     256
#define GAME_W    320   // Game's native back_buffer width
#define GAME_H    200   // Game's native back_buffer height
#define UPLOAD_H  256   // Rows we actually upload (must cover the UV sample range)

#define QUAD_X_MARGIN  40.0f
#define QUAD_Y_MARGIN  20.0f
#define QUAD_X0        QUAD_X_MARGIN
#define QUAD_X1        (SCREEN_W - QUAD_X_MARGIN)
#define QUAD_Y0        QUAD_Y_MARGIN
#define QUAD_Y1        (SCREEN_H - QUAD_Y_MARGIN)
#define QUAD_TEX_U     ((float)GAME_W / (float)TEX_W)
#define QUAD_TEX_V     (56.0f / (float)TEX_H)

// Clear to black for gameplay. Swap to 0x68B0D8FF if you want a visible
// background behind transparent (palette-index-0) pixels during testing.
#define CLEAR_COLOR  0x000000FF

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// =============================================================================
// Stubs for game-side hooks that this backend doesn't need on 3DS.
// Kept as no-ops so the rest of the game links cleanly.
// =============================================================================
ITCM_CODE void sync_palette()        {}
ITCM_CODE void irq_Vblank()          {}
ITCM_CODE void irq_arm9_fifo()       {}
void ds_start_frame()                {}
void ds_end_frame()                  {}
void nds_set_render_size(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void init_nds_textures()             {}
void nds_init()                      {}

// Legacy NDS-era bitmap/texture cache, unused on 3DS. Left as stubs so the
// game's draw calls still link — rendering of actual geometry is TBD.
typedef struct {
    int         last_frame_used;
    int         key;
    u32         format;
    grs_bitmap* bm;
} gltexture_t;

int find_bitmap_in_vram(int key)                 { (void)key; return -1; }
ITCM_CODE void bind_texture(grs_bitmap* bmp)     { (void)bmp; }

g3_draw_tmap_func_t g3_draw_tmap_func = (g3_draw_tmap_func_t)g3_draw_tmap_tex;

// =============================================================================
// Wireframe overlay
// =============================================================================
// Separate shader, VBO, and projection from the bitblt quad. Called by
// g3_draw_poly to overlay polygon outlines on top of the back_buffer.
//
// Coordinate assumption: g3_draw_poly receives points already projected to
// the game's 320x200 screen space by the software renderer. We take x/y as
// pixel coords (with fixed-point precision to strip) and map them via an
// orthographic projection onto the same region of the 3DS top screen where
// the bitblt quad lives, so wireframes align with the underlying pixels.
// =============================================================================

// Descent gives us camera-space vertices in 16.16 fixed-point (F1_0 = 65536
// = 1 "unit"). We project them on the CPU here, so the GPU-side projection
// can stay as a simple screen-space ortho.
//
// Descent axis convention (confirmed by inspection):
//   +X = right   +Y = up   +Z = forward (into the screen)
// This is a left-handed system. Standard graphics conventions are
// right-handed with -Z forward, so we need to flip Y (screen y grows down)
// and handle Z as the depth-into-scene.

// Vertical field of view in degrees
#define WIRE_FOV_DEG      60.0f

// Near/far clip planes in Descent units (F1_0 = 65536 = 1 unit).
// Anything with z < WIRE_NEAR gets culled.
#define WIRE_NEAR         0.1f
#define WIRE_FAR          10000.0f

// Line thickness in screen pixels (constant regardless of distance)
#define WIRE_THICKNESS    1.0f

// Max verts per frame — 6 verts per edge (thin quad as 2 tris) * edges
#define WIRE_MAX_VERTS    (4096 * 6)

typedef struct { float x, y, z; } wire_vertex;

static shaderProgram_s  wire_program;
static DVLB_s*          wire_dvlb;
static int              wire_uLoc_projection;
static int              wire_uLoc_color;
static C3D_Mtx          wire_projection;
static wire_vertex*     wire_vbo;       // linear-alloc'd
static int              wire_vbo_count; // reset each frame

// Precomputed projection scale factors — set once in wire_init
static float wire_focal;         // distance in game-units from camera to projection plane
static float wire_aspect_inv;    // 1/aspect (GAME_H/GAME_W)
static float wire_half_w;        // QUAD region half-width (screen pixels)
static float wire_half_h;        // QUAD region half-height
static float wire_center_x;      // screen center of the quad region
static float wire_center_y;

static void wire_init(void)
{
    wire_dvlb = DVLB_ParseFile((u32*)vshader_wire_shbin, vshader_wire_shbin_size);
    shaderProgramInit(&wire_program);
    shaderProgramSetVsh(&wire_program, &wire_dvlb->DVLE[0]);

    wire_uLoc_projection = shaderInstanceGetUniformLocation(wire_program.vertexShader, "projection");
    wire_uLoc_color      = shaderInstanceGetUniformLocation(wire_program.vertexShader, "wireColor");

    // GPU-side projection stays ortho in 3DS screen space. We do perspective
    // projection on the CPU and emit screen coords directly.
    Mtx_OrthoTilt(&wire_projection, 0.0f, SCREEN_W, SCREEN_H, 0.0f,
                  -1.0f, 1.0f, true);

    // Precompute focal length and viewport mapping.
    // Standard perspective: screen_x = focal * (x / z), screen_y = focal * (y / z)
    // where focal is chosen so that the edge of the frustum at z=1 lands at
    // the edge of the screen. For FOV given as vertical angle:
    //   tan(fov/2) = (screen_half_height) / focal   =>   focal = 1 / tan(fov/2)
    float fov_rad = WIRE_FOV_DEG * (float)M_PI / 180.0f;
    wire_focal       = 1.0f / tanf(fov_rad * 0.5f);
    wire_aspect_inv  = (float)GAME_H / (float)GAME_W;

    wire_half_w   = (QUAD_X1 - QUAD_X0) * 0.5f;
    wire_half_h   = (QUAD_Y1 - QUAD_Y0) * 0.5f;
    wire_center_x = (QUAD_X0 + QUAD_X1) * 0.5f;
    wire_center_y = (QUAD_Y0 + QUAD_Y1) * 0.5f;

    wire_vbo = linearAlloc(sizeof(wire_vertex) * WIRE_MAX_VERTS);
    wire_vbo_count = 0;
}

// Project a camera-space point (16.16 fixed) to 3DS screen space.
// Returns true if the point is in front of the camera, false if culled.
//
// Descent camera space: +X right, +Y up, +Z forward (left-handed).
// We flip Y so screen-y grows downward (standard screen convention).
static inline bool wire_project(fix cx, fix cy, fix cz,
                                float* out_sx, float* out_sy)
{
    // Convert 16.16 fixed -> float units (F1_0 = 65536 = 1.0 unit)
    float x = (float)cx * (1.0f / 65536.0f);
    float y = (float)cy * (1.0f / 65536.0f);
    float z = (float)cz * (1.0f / 65536.0f);

    if (z < WIRE_NEAR || z > WIRE_FAR) return false;

    // Perspective divide, scale by focal length and viewport.
    // y is negated because Descent has +Y up, screen has +Y down.
    float ndc_x =  (x / z) * wire_focal * wire_aspect_inv;
    float ndc_y = -(y / z) * wire_focal;

    *out_sx = wire_center_x + ndc_x * wire_half_w;
    *out_sy = wire_center_y + ndc_y * wire_half_h;
    return true;
}

// Emit a thickness-WIRE_THICKNESS line as two triangles (a thin quad).
// Done on the CPU because PICA200 doesn't have a first-class line primitive
// in citro3d (you'd need geometry shaders or pre-built line geometry).
static void wire_emit_line(float x0, float y0, float x1, float y1)
{
    if (wire_vbo_count + 6 > WIRE_MAX_VERTS) return;

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;

    // Perpendicular unit vector, scaled by half-thickness
    float px = -dy / len * (WIRE_THICKNESS * 0.5f);
    float py =  dx / len * (WIRE_THICKNESS * 0.5f);

    wire_vertex* v = &wire_vbo[wire_vbo_count];
    // Tri 1
    v[0].x = x0 + px; v[0].y = y0 + py; v[0].z = 0.0f;
    v[1].x = x0 - px; v[1].y = y0 - py; v[1].z = 0.0f;
    v[2].x = x1 + px; v[2].y = y1 + py; v[2].z = 0.0f;
    // Tri 2
    v[3].x = x1 + px; v[3].y = y1 + py; v[3].z = 0.0f;
    v[4].x = x0 - px; v[4].y = y0 - py; v[4].z = 0.0f;
    v[5].x = x1 - px; v[5].y = y1 - py; v[5].z = 0.0f;

    wire_vbo_count += 6;
}

// Reset the per-frame wireframe buffer — call at the start of each frame,
// before any g3_draw_poly calls.
static void wire_frame_begin(void)
{
    wire_vbo_count = 0;
}

// Draw all buffered wireframe lines. Called after the bitblt quad so the
// lines overlay on top of the back_buffer.
static void wire_frame_end(void)
{
    if (wire_vbo_count == 0) return;

    // Bind wireframe shader
    C3D_BindProgram(&wire_program);

    // Set up vertex attributes for wireframe (position only)
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, wire_vbo, sizeof(wire_vertex), 1, 0x0);

    // Uniforms: projection + line color (white, fully opaque)
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, wire_uLoc_projection, &wire_projection);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, wire_uLoc_color,
                  1.0f, 1.0f, 1.0f, 1.0f);

    // Lines should source their color from the vertex shader (primary color),
    // not the bound texture. Swap the texenv temporarily.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    C3D_DrawArrays(GPU_TRIANGLES, 0, wire_vbo_count);
}

// -----------------------------------------------------------------------------
// Actual g3_draw_poly implementation — replaces the stub.
// Takes an n-vertex convex polygon and emits its edges as line segments.
// Expects camera-space coordinates in 16.16 fixed point.
// -----------------------------------------------------------------------------
#define WIRE_MAX_POLY_VERTS 16

ITCM_CODE void g3_draw_poly(int nv, vms_vector** pointlist)
{
    if (nv < 2) return;
    if (nv > WIRE_MAX_POLY_VERTS) nv = WIRE_MAX_POLY_VERTS;

    // Project all vertices to screen space up front. Track which ones ended
    // up in front of the camera so we can skip edges with a culled endpoint.
    float sx[WIRE_MAX_POLY_VERTS], sy[WIRE_MAX_POLY_VERTS];
    bool  visible[WIRE_MAX_POLY_VERTS];

    for (int i = 0; i < nv; i++) {
        visible[i] = wire_project(pointlist[i]->x, pointlist[i]->y,
                                  pointlist[i]->z, &sx[i], &sy[i]);
    }

    // Emit edges. Skip any edge with at least one endpoint behind the camera.
    // This is coarse (proper clipping would intersect the edge with the near
    // plane) but good enough for a debug wireframe.
    for (int i = 0; i < nv; i++) {
        int j = (i + 1) % nv;
        if (visible[i] && visible[j]) {
            wire_emit_line(sx[i], sy[i], sx[j], sy[j]);
        }
    }
}

ITCM_CODE void g3_draw_tmap_flat(int nv, vms_vector** pointlist, g3s_uvl* uvl_list, grs_bitmap* bm) {
    (void)uvl_list; (void)bm;
    g3_draw_poly(nv, pointlist);
}
ITCM_CODE void g3_draw_tmap_tex(int nv, vms_vector** pointlist, g3s_uvl* uvl_list, grs_bitmap* bm) {
    (void)uvl_list; (void)bm;
    g3_draw_poly(nv, pointlist);
}
ITCM_CODE void g3_draw_bitmap(vms_vector* pos, fix width, fix height, grs_bitmap* bm) {
    (void)pos; (void)width; (void)height; (void)bm;
}

// =============================================================================
// TRIANGLE MODE — simplest possible GPU sanity test
// =============================================================================
#if RENDER_MODE == RENDER_MODE_TRIANGLE

// NOTE: requires the position+color vertex shader (vshader_triangle.v.pica).

typedef struct { float x, y, z; float r, g, b, a; } tri_vertex;

static const tri_vertex triangle_verts[] = {
    { 200.0f,  40.0f, 0.5f,  1.0f, 0.0f, 0.0f, 1.0f },
    {  60.0f, 200.0f, 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
    { 340.0f, 200.0f, 0.5f,  0.0f, 0.0f, 1.0f, 1.0f },
};
#define triangle_vert_count 3

void sceneInit(void)
{
    vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);

    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 4);

    Mtx_OrthoTilt(&projection, 0.0f, SCREEN_W, SCREEN_H, 0.0f, 0.0f, 1.0f, true);

    vbo_data = linearAlloc(sizeof(triangle_verts));
    memcpy(vbo_data, triangle_verts, sizeof(triangle_verts));

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, vbo_data, sizeof(tri_vertex), 2, 0x10);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

void bitblt_to_screen(void)
{
    hidScanInput();
    keyboard_handler();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_DrawArrays(GPU_TRIANGLES, 0, triangle_vert_count);
    C3D_FrameEnd(0);
}

// =============================================================================
// TEXTURED MODES (BITBLT, SOLID, CHECKERBOARD, RAW_WRITE)
// All share the same setup; they only differ in how tex_buf is populated.
// =============================================================================
#else

// NOTE: requires the position+texcoord vertex shader (vshader.v.pica).
// Make sure it has an explicit `.entry vmain` and uses `.in` (not `.alias`)
// for v0/v1 — PICA200 hardware is strict about these, emulators are not.

// -----------------------------------------------------------------------------
// Quad covering most of the top screen with the game's aspect ratio
// -----------------------------------------------------------------------------
typedef struct { float x, y, z; float u, v; } tex_vertex;

static const tex_vertex quad_list[] = {
    // Triangle 1 — V coords flipped so the texture renders right-side up
    { QUAD_X0, QUAD_Y1, 0.5f, 0.0f,       QUAD_TEX_V },
    { QUAD_X1, QUAD_Y1, 0.5f, QUAD_TEX_U, QUAD_TEX_V },
    { QUAD_X1, QUAD_Y0, 0.5f, QUAD_TEX_U, 1.0f       },
    // Triangle 2
    { QUAD_X0, QUAD_Y1, 0.5f, 0.0f,       QUAD_TEX_V },
    { QUAD_X1, QUAD_Y0, 0.5f, QUAD_TEX_U, 1.0f       },
    { QUAD_X0, QUAD_Y0, 0.5f, 0.0f,       1.0f       },
};
#define quad_list_count 6

static C3D_Tex back_tex;
static void*   tex_buf;

// -----------------------------------------------------------------------------
// Software Morton-code tiler
// PICA200 textures use 8x8 tiles in Z-order (Morton interleave), with the
// tiles themselves laid out row-major across the texture.
// -----------------------------------------------------------------------------
static void tex_upload_software(C3D_Tex* tex,
                                const u32* src_linear, int src_stride,
                                int src_w, int src_h, int tex_w)
{
    u32* dst = (u32*)tex->data;
    int tiles_per_row = tex_w / 8;

    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int px = x & 7, py = y & 7;
            // Interleave bits of px and py to get Z-order index within tile
            int z = (px & 1)        | ((py & 1) << 1) |
                    ((px & 2) << 1) | ((py & 2) << 2) |
                    ((px & 4) << 2) | ((py & 4) << 3);
            int tile_idx = (y / 8) * tiles_per_row + (x / 8);
            dst[tile_idx * 64 + z] = src_linear[y * src_stride + x];
        }
    }
    C3D_TexFlush(tex);
}

// -----------------------------------------------------------------------------
// Scene setup — shader, attributes, projection, texture, GPU state
// -----------------------------------------------------------------------------
void sceneInit(void)
{
    vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);

    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

    // v0 = position (xyz), v1 = texcoord (uv)
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

    Mtx_OrthoTilt(&projection, 0.0f, SCREEN_W, SCREEN_H, 0.0f, 0.0f, 1.0f, true);

    vbo_data = linearAlloc(sizeof(quad_list));
    memcpy(vbo_data, quad_list, sizeof(quad_list));

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, vbo_data, sizeof(tex_vertex), 2, 0x10);

    C3D_TexInit(&back_tex, TEX_W, TEX_H, GPU_RGBA8);
    C3D_TexSetFilter(&back_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&back_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    tex_buf = linearAlloc(TEX_W * TEX_H * 4);
    memset(tex_buf, 0, TEX_W * TEX_H * 4);

    // Explicit GPU state — emulators tolerate garbage defaults, hardware does not
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    // Standard alpha blending: src * src.a + dst * (1 - src.a).
    // Enables palette-index-0 transparency from the conversion loop.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);

    // Fragment stage: output the sampled texture color directly
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// -----------------------------------------------------------------------------
// Main present function — called every frame by the game
// -----------------------------------------------------------------------------
void bitblt_to_screen(void)
{
    hidScanInput();
    keyboard_handler();

    // --- Populate the staging buffer based on render mode ---
#if RENDER_MODE == RENDER_MODE_BITBLT
    // Convert palette-indexed back_buffer to RGBA8. Palette indices are 6-bit
    // (0..63), so shift left by 2 to get 8-bit values.
    // Palette index 0 is treated as transparent (alpha = 0).
    extern ubyte gr_current_pal[];
    for (int y = 0; y < UPLOAD_H; y++) {
        const u8* src = back_buffer + y * 320;
        u32*      dst = (u32*)tex_buf + y * TEX_W;
        for (int x = 0; x < 320; x++) {
            u8 idx = src[x];
            u8 r = gr_current_pal[idx * 3 + 0] << 2;
            u8 g = gr_current_pal[idx * 3 + 1] << 2;
            u8 b = gr_current_pal[idx * 3 + 2] << 2;
            u8 a = (idx == 0) ? 0x00 : 0xFF;
            dst[x] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
        }
    }
#elif RENDER_MODE == RENDER_MODE_SOLID
    u32* p = (u32*)tex_buf;
    for (int i = 0; i < TEX_W * TEX_H; i++) p[i] = 0xFF0000FF;
#elif RENDER_MODE == RENDER_MODE_CHECKERBOARD
    u32* p = (u32*)tex_buf;
    for (int y = 0; y < UPLOAD_H; y++) {
        for (int x = 0; x < GAME_W; x++) {
            int cell = ((x / 32) ^ (y / 32)) & 1;
            p[y * TEX_W + x] = cell ? 0xFF0000FF : 0xFFFFFFFF;
        }
    }
#elif RENDER_MODE == RENDER_MODE_RAW_WRITE
    // Bypasses the tiler entirely — writes directly to tex->data.
    u32* raw = (u32*)back_tex.data;
    for (int i = 0; i < TEX_W * TEX_H; i++) raw[i] = 0xFF0000FF;
    C3D_TexFlush(&back_tex);
#endif

    // --- Upload to GPU (unless we're testing raw writes) ---
#if RENDER_MODE != RENDER_MODE_RAW_WRITE
    tex_upload_software(&back_tex, (u32*)tex_buf, TEX_W, GAME_W, UPLOAD_H, TEX_W);
#endif

    // --- Render ---
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target);

    // Draw the bitblt quad (background)
    C3D_BindProgram(&program);
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, vbo_data, sizeof(tex_vertex), 2, 0x10);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_TexBind(0, &back_tex);
    C3D_DrawArrays(GPU_TRIANGLES, 0, quad_list_count);

    // Draw wireframes on top (uses its own shader, VBO, and texenv)
    wire_frame_end();

    C3D_FrameEnd(0);

    // Reset wireframe buffer for next frame's g3_draw_poly calls
    wire_frame_begin();
}

#endif // RENDER_MODE branch

// =============================================================================
// GPU bring-up — called once at startup
// =============================================================================
void init_3ds_gpu(void)
{
    gfxSet3D(false); // 2D mode — stereo without a right-eye target blanks the screen
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    target = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W,
                                    GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    sceneInit();
    wire_init();
}

// =============================================================================
// Debug modes reference
// =============================================================================
// To switch modes, change RENDER_MODE at the top of this file and (if switching
// between TRIANGLE and everything else) swap the vertex shader source.
//
//   BITBLT        — Normal gameplay rendering. Palette -> RGBA -> tiled texture -> quad.
//                   Uses position+texcoord shader.
//
//   TRIANGLE      — Minimal GPU test. Draws a colored triangle with no textures.
//                   Uses position+color shader (vshader_triangle.v.pica).
//                   If this fails, the pipeline (shader, render target, projection)
//                   is fundamentally broken.
//
//   SOLID         — Fills the staging buffer with solid red, then runs the full
//                   texture pipeline. If this works but BITBLT doesn't, the bug
//                   is in the palette conversion. If this fails but TRIANGLE
//                   works, the bug is in the texture upload path.
//
//   CHECKERBOARD  — 32x32 red/white checker. Same diagnostic purpose as SOLID
//                   but makes tiling bugs visually obvious (scrambled blocks).
//
//   RAW_WRITE     — Bypasses the software tiler entirely and writes directly
//                   into back_tex.data. If this works but SOLID doesn't, the
//                   tiler is broken. If both fail, something between tex->data
//                   and the GPU sampler is broken (state, binding, or memory).
// =============================================================================
