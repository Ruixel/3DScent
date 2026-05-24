// =============================================================================
// ndsfunc.c — 3DS rendering backend
// =============================================================================
// Two layers composited each frame:
//   1. Bitblt: Descent's 320x200 palette-indexed back_buffer is converted
//      to RGBA, software-tiled into a PICA200 texture, and drawn as a quad
//      (HUD, menus, anything Descent renders to its software framebuffer).
//      Palette index 255 is transparent.
//   2. World: level geometry rendered natively as textured triangles.
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
#include "piggy.h"
#include "vshader_shbin.h"
#include "vshader_world_shbin.h"

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
u8          back_buffer[400 * 240];
u16         ds_palette[256];
int         palette_updated;
bool        doSleep;

// -----------------------------------------------------------------------------
// Screen / quad geometry
// -----------------------------------------------------------------------------
// Top screen is 240x400 physical, rendered as 400x240 landscape via OrthoTilt.
#define SCREEN_W  400
#define SCREEN_H  240

#define TEX_W     512   // PICA200 requires power-of-two textures
#define TEX_H     256
#define GAME_W    320   // Game's native back_buffer width
#define GAME_H    200
#define UPLOAD_H  256   // Rows uploaded; must cover the UV sample range

#define QUAD_X_MARGIN  40.0f
#define QUAD_Y_MARGIN  20.0f
#define QUAD_X0        QUAD_X_MARGIN
#define QUAD_X1        (SCREEN_W - QUAD_X_MARGIN)
#define QUAD_Y0        QUAD_Y_MARGIN
#define QUAD_Y1        (SCREEN_H - QUAD_Y_MARGIN)
#define QUAD_TEX_U     ((float)GAME_W / (float)TEX_W)
#define QUAD_TEX_V     (56.0f / (float)TEX_H)

#define CLEAR_COLOR  0x000000FF

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// -----------------------------------------------------------------------------
// GPU state (bitblt)
// -----------------------------------------------------------------------------
static C3D_RenderTarget* target_left;
static C3D_RenderTarget* target_right;
static DVLB_s*           vshader_dvlb;
static shaderProgram_s   program;
static int               uLoc_projection;
static C3D_Mtx           projection;
static void*             vbo_data;

g3_draw_tmap_func_t g3_draw_tmap_func = (g3_draw_tmap_func_t)g3_draw_tmap_tex;

bool gpu_inited = false;

// =============================================================================
// Debug timing
// =============================================================================
// Define DEBUG_TIMING to enable. Each TIMED_BLOCK accumulates into a u64
// counter; PRINT_TIMING dumps and resets at frame end.
// =============================================================================
//#define DEBUG_TIMING
//#define DEBUG_TIMING_DETAILED

#ifdef DEBUG_TIMING_DETAILED
  #define DEBUG_TIMING  // auto-enable coarse timing
#endif

#ifdef DEBUG_TIMING
  #define TIMER_DECL(name)   static u64 name = 0
  #define TIMER_START(t)     u64 t = svcGetSystemTick()
  #define TIMER_ADD(acc, t)  (acc) += svcGetSystemTick() - (t)
  #define TIMER_RESET(t)     (t) = svcGetSystemTick()
  #define TIMER_MS(t)        ((t) / (double)CPU_TICKS_PER_MSEC)
#else
  #define TIMER_DECL(name)
  #define TIMER_START(t)
  #define TIMER_ADD(acc, t)
  #define TIMER_RESET(t)
  #define TIMER_MS(t)        0.0
#endif

#ifdef DEBUG_TIMING_DETAILED
  #define TIMER_START_D(t)    TIMER_START(t)
  #define TIMER_ADD_D(acc, t) TIMER_ADD(acc, t)
  #define TIMER_RESET_D(t)    TIMER_RESET(t)
#else
  #define TIMER_START_D(t)
  #define TIMER_ADD_D(acc, t)
  #define TIMER_RESET_D(t)
#endif

// =============================================================================
// gpu_tex — preloaded GPU texture pool
// =============================================================================
// One PICA200 texture per Descent bitmap, allocated at level load in
// RGBA5551 format (half the size of RGBA8). A bitmap's index in
// GameBitmaps[] is also its slot in gpu_tex_pool[].
//
// init_nds_textures() is called by piggy_bitmap_page_out_all() after each
// level load, so the pool is rebuilt whenever the game flushes its bitmap
// cache. Slots beyond GPU_TEX_PRELOADED_MAX are reserved for dynamic
// uploads (texmerge composites, etc.).
// =============================================================================

#define GPU_TEX_PRELOADED_MAX  MAX_BITMAP_FILES
#define GPU_TEX_DYNAMIC_MAX    1024
#define GPU_TEX_MAX           (GPU_TEX_PRELOADED_MAX + GPU_TEX_DYNAMIC_MAX)

typedef struct {
    C3D_Tex* tex;        // NULL if not loaded
    float    u_scale;    // bm_w / pot_w (for non-POT bitmaps)
    float    v_scale;    // bm_h / pot_h
    bool     owned;      // true if malloc'd
} gpu_tex_entry_t;

static gpu_tex_entry_t gpu_tex_pool[GPU_TEX_MAX];
static int gpu_tex_loaded_count;
static int gpu_tex_total_bytes;

// Per-frame counters
static int poly_count = 0;
static int frame_preloaded_hits = 0;
static int frame_hash_hits = 0;
static int frame_dynamic_uploads = 0;
static int frame_composite_uploads = 0;
static int frame_composite_reuses = 0;
static int frame_lookup_failures = 0;

static u8 swizzle_lut[64];
static bool swizzle_lut_init = false;

static void init_swizzle_lut(void) {
    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            int z = (px & 1)        | ((py & 1) << 1) |
                    ((px & 2) << 1) | ((py & 2) << 2) |
                    ((px & 4) << 2) | ((py & 4) << 3);
            swizzle_lut[py * 8 + px] = z;
        }
    }
    swizzle_lut_init = true;
}

// -----------------------------------------------------------------------------
// Texmerge composite tracking.
//
// Descent's texmerge.c keeps a small LRU cache (~10 slots) of composite
// bitmaps. When it needs an 11th, an existing slot is REUSED — same
// grs_bitmap* and bm_data, but freshly computed pixels. If we cached the
// GPU texture once and trusted bm->key forever, we'd display stale
// textures whenever a slot was reused.
//
// texmerge writes a hash (orient<<30 | top<<16 | bottom) into bm->key
// each time it (re)builds a slot. We cache that hash alongside the GPU
// slot and re-upload when it changes.
//
// TEXMERGE_TRACKED_MAX must be >= MAX_NUM_CACHE_BITMAPS in texmerge.c.
// -----------------------------------------------------------------------------
#define TEXMERGE_TRACKED_MAX  256

typedef struct {
    grs_bitmap* bm;
    int         composite_key;
    int         slot;
} texmerge_track_t;

static texmerge_track_t texmerge_track[TEXMERGE_TRACKED_MAX];

// Orphaned slots awaiting deferred free.
//
// When a texmerge bitmap's content changes mid-frame, we can't free the
// old GPU texture immediately: earlier batches in world_batches[] still
// reference it and won't render until C3D_FrameEnd. Freeing now would
// cause use-after-free. Instead we defer the free to the next frame's
// world_frame_begin, after gspWaitForP3D has confirmed GPU completion.
#define ORPHAN_SLOTS_MAX  128
static int orphan_slots[ORPHAN_SLOTS_MAX];
static int orphan_slot_count = 0;

// A composite key has high bits set (orient<<30); plain bitmap_index
// values are small positive ints.
static inline bool is_composite_key(int key)
{
    return (key < 0) || (key >= GPU_TEX_PRELOADED_MAX);
}

// -----------------------------------------------------------------------------
// bm_data pointer -> bitmap index reverse lookup.
//
// Some grs_bitmap structs passed to g3_draw_tmap_tex aren't the originals
// from GameBitmaps[] — they're copies (Textures[] entries, animated wall
// snapshots, etc.) that share bm_data with the original but have an
// uninitialized `key`. We build a hash from bm_data -> original index at
// preload time so we can recover the slot.
//
// Open-addressed linear probing, sized 2x the bitmap count.
// -----------------------------------------------------------------------------
#define BM_HASH_SIZE  (MAX_BITMAP_FILES * 2)

typedef struct {
    void* data;
    int   index;
} bm_hash_entry_t;

static bm_hash_entry_t bm_hash[BM_HASH_SIZE];

static inline u32 bm_hash_func(void* p)
{
    u32 h = (u32)(uintptr_t)p;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    return h % BM_HASH_SIZE;
}

static void bm_hash_clear(void)
{
    memset(bm_hash, 0, sizeof(bm_hash));
}

static void bm_hash_insert(void* data, int index)
{
    if (!data) return;
    u32 slot = bm_hash_func(data);
    for (int probe = 0; probe < BM_HASH_SIZE; probe++) {
        u32 i = (slot + probe) % BM_HASH_SIZE;
        if (bm_hash[i].data == NULL) {
            bm_hash[i].data  = data;
            bm_hash[i].index = index;
            return;
        }
        if (bm_hash[i].data == data) {
            // First-inserted wins when multiple bitmaps share a data buffer.
            return;
        }
    }
}

static int bm_hash_lookup(void* data)
{
    if (!data) return -1;
    u32 slot = bm_hash_func(data);
    for (int probe = 0; probe < BM_HASH_SIZE; probe++) {
        u32 i = (slot + probe) % BM_HASH_SIZE;
        if (bm_hash[i].data == NULL) return -1;
        if (bm_hash[i].data == data) return bm_hash[i].index;
    }
    return -1;
}

static inline int next_pot(int n)
{
    int p = 8;
    while (p < n) p <<= 1;
    return p;
}

// Descent palette is 6-bit per channel; RGBA5551 is 5-bit. Drop one bit.
static inline u16 palette_to_rgba5551(int idx, ubyte* palette, bool transparent_zero)
{
    if (transparent_zero && idx == 255) {
        return 0;  // index 255 = transparent
    }
    u8 r = palette[idx * 3 + 0] >> 1;
    u8 g = palette[idx * 3 + 1] >> 1;
    u8 b = palette[idx * 3 + 2] >> 1;
    return (r << 11) | (g << 6) | (b << 1) | 1;
}

// Software Morton-code tiler for RGBA5551 textures.
static void tex_upload_5551(C3D_Tex* tex, const u16* src_linear,
                            int src_w, int src_h, int tex_w)
{
    if (!swizzle_lut_init) init_swizzle_lut();

    u16* dst = (u16*)tex->data;
    int tiles_per_row = tex_w / 8;

    memset(dst, 0, tex_w * tex->height * sizeof(u16));

    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int z = swizzle_lut[(y & 7) * 8 + (x & 7)];
            int tile_idx = (y / 8) * tiles_per_row + (x / 8);
            dst[tile_idx * 64 + z] = src_linear[y * src_w + x];
        }
    }
    C3D_TexFlush(tex);
}

// Decompress an RLE-compressed bitmap into a flat indexed buffer.
// Reuses the existing software blitter via a fake destination bitmap.
static u8* decompress_rle(grs_bitmap* bmp)
{
    extern void gr_bm_ubitblt_rle(int w, int h, int dx, int dy,
                                  int sx, int sy, grs_bitmap* src, grs_bitmap* dest);
    int w = bmp->bm_w, h = bmp->bm_h;
    u8* out = malloc(w * h);
    if (!out) return NULL;

    grs_bitmap dest;
    memset(&dest, 0, sizeof(dest));
    dest.bm_w = w;
    dest.bm_h = h;
    dest.bm_data = out;
    dest.bm_rowsize = w;
    dest.bm_type = 0;

    gr_bm_ubitblt_rle(w, h, 0, 0, 0, 0, bmp, &dest);
    return out;
}

static bool gpu_tex_upload_to_slot(grs_bitmap* bmp, int slot)
{
    if (bmp->bm_w <= 0 || bmp->bm_h <= 0) return false;
    if (!bmp->bm_data) return false;
    if (slot < 0 || slot >= GPU_TEX_MAX) return false;
    if (gpu_tex_pool[slot].tex) return false;

    u8* indexed_data;
    bool we_allocated = false;
    if (bmp->bm_flags & BM_FLAG_RLE) {
        indexed_data = decompress_rle(bmp);
        if (!indexed_data) return false;
        we_allocated = true;
    } else {
        indexed_data = bmp->bm_data;
    }

    int pot_w = next_pot(bmp->bm_w);
    int pot_h = next_pot(bmp->bm_h);
    bool has_transparency = (bmp->bm_flags & BM_FLAG_TRANSPARENT) != 0;

    u16* rgba = malloc(bmp->bm_w * bmp->bm_h * sizeof(u16));
    if (!rgba) {
        if (we_allocated) free(indexed_data);
        return false;
    }
    for (int i = 0; i < bmp->bm_w * bmp->bm_h; i++) {
        rgba[i] = palette_to_rgba5551(indexed_data[i], gr_palette, has_transparency);
    }

    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    if (!tex || !C3D_TexInit(tex, pot_w, pot_h, GPU_RGBA5551)) {
        if (tex) free(tex);
        free(rgba);
        if (we_allocated) free(indexed_data);
        return false;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);

    tex_upload_5551(tex, rgba, bmp->bm_w, bmp->bm_h, pot_w);

    free(rgba);
    if (we_allocated) free(indexed_data);

    gpu_tex_pool[slot].tex     = tex;
    gpu_tex_pool[slot].u_scale = (float)bmp->bm_w / (float)pot_w;
    gpu_tex_pool[slot].v_scale = (float)bmp->bm_h / (float)pot_h;
    gpu_tex_pool[slot].owned   = true;
    gpu_tex_total_bytes += pot_w * pot_h * sizeof(u16);
    gpu_tex_loaded_count++;

    bmp->key = slot;
    bm_hash_insert(bmp->bm_data, slot);
    return true;
}

static bool gpu_tex_upload_one(int idx)
{
    return gpu_tex_upload_to_slot(&GameBitmaps[idx], idx);
}

// Find a free slot in the dynamic range and upload there. Used at draw
// time for texmerge composites and other bitmaps not in GameBitmaps[].
static int gpu_tex_upload_dynamic(grs_bitmap* bmp)
{
    if (!bmp || !bmp->bm_data) return -1;

    static int next_slot_hint = GPU_TEX_PRELOADED_MAX;
    for (int probe = 0; probe < GPU_TEX_DYNAMIC_MAX; probe++) {
        int slot = GPU_TEX_PRELOADED_MAX +
                   ((next_slot_hint - GPU_TEX_PRELOADED_MAX + probe) % GPU_TEX_DYNAMIC_MAX);
        if (!gpu_tex_pool[slot].tex) {
            if (gpu_tex_upload_to_slot(bmp, slot)) {
                next_slot_hint = slot + 1;
                return slot;
            }
            return -1;
        }
    }
    return -1;
}

static void gpu_tex_free_slot(int slot)
{
    if (slot < 0 || slot >= GPU_TEX_MAX) return;
    if (!gpu_tex_pool[slot].tex) return;
    if (gpu_tex_pool[slot].owned) {
        C3D_TexDelete(gpu_tex_pool[slot].tex);
        free(gpu_tex_pool[slot].tex);
        gpu_tex_pool[slot].tex = NULL;
        gpu_tex_pool[slot].owned = false;
        gpu_tex_loaded_count--;
    }
}

static void gpu_tex_free_all(void)
{
    for (int i = 0; i < GPU_TEX_MAX; i++) {
        if (gpu_tex_pool[i].tex && gpu_tex_pool[i].owned) {
            C3D_TexDelete(gpu_tex_pool[i].tex);
            free(gpu_tex_pool[i].tex);
            gpu_tex_pool[i].tex = NULL;
            gpu_tex_pool[i].owned = false;
        }
        // Borrowed slots (e.g. white_tex_slot) survive the wipe.
    }
    bm_hash_clear();
    memset(texmerge_track, 0, sizeof(texmerge_track));
    gpu_tex_loaded_count = 0;
    gpu_tex_total_bytes = 0;
}

static texmerge_track_t* texmerge_track_get(grs_bitmap* bm)
{
    for (int i = 0; i < TEXMERGE_TRACKED_MAX; i++) {
        if (texmerge_track[i].bm == bm) return &texmerge_track[i];
    }
    for (int i = 0; i < TEXMERGE_TRACKED_MAX; i++) {
        if (texmerge_track[i].bm == NULL) {
            texmerge_track[i].bm = bm;
            texmerge_track[i].composite_key = 0;
            texmerge_track[i].slot = -1;
            return &texmerge_track[i];
        }
    }
    // Full — evict slot 0. With TEXMERGE_TRACKED_MAX >> texmerge's cache
    // size this should never happen.
    if (texmerge_track[0].slot >= 0) {
        gpu_tex_free_slot(texmerge_track[0].slot);
    }
    texmerge_track[0].bm = bm;
    texmerge_track[0].composite_key = 0;
    texmerge_track[0].slot = -1;
    return &texmerge_track[0];
}

// page_out_all() frees all bm_data before init_nds_textures runs, so we
// need to page each bitmap back in from disk before we can upload it.
static void gpu_tex_preload_all(void)
{
    extern int Num_bitmap_files;

    for (int i = 0; i < Num_bitmap_files; i++) {
        bitmap_index bi;
        bi.index = i;

        if (GameBitmaps[i].bm_flags & BM_FLAG_PAGED_OUT) {
            piggy_bitmap_page_in(bi);
        }
        gpu_tex_upload_one(i);
    }

    printf("gpu_tex: loaded %d/%d bitmaps, %d KB\n",
           gpu_tex_loaded_count, Num_bitmap_files,
           gpu_tex_total_bytes / 1024);
    printf("VRAM free: %u KB, linear free: %u KB\n",
           vramSpaceFree() / 1024, linearSpaceFree() / 1024);
}

// =============================================================================
// World renderer
// =============================================================================
// Descent's original 4:3 CRT used non-square pixels with ~90° horizontal FOV.
// On the 3DS we render to a roughly-4:3 quad of square pixels, so we set
// horizontal and vertical FOV independently rather than deriving one from
// the other via aspect ratio.
// =============================================================================
#define WORLD_FOV_H_DEG    56.0f
#define WORLD_FOV_V_DEG    60.0f
#define WORLD_NEAR         0.1f
#define WORLD_FAR          10000.0f

#define WORLD_MAX_VERTS       (4096 * 6)
#define WORLD_MAX_POLY_VERTS  16
#define WORLD_MAX_BATCHES     512

// pos (3) + texcoord (2) + light (1) + color RGBA (1) = 8 floats = 32 B
typedef struct { float x, y, z, u, v, light; u32 color; } world_vertex;

typedef struct {
    C3D_Tex* tex;
    int      vert_start;
    int      vert_count;
} world_batch_t;

static shaderProgram_s  world_program;
static DVLB_s*          world_dvlb;
static int              world_uLoc_projection;
static C3D_Mtx          world_projection;
static world_vertex*    world_vbo;
static int              world_vbo_count;

static world_batch_t world_batches[WORLD_MAX_BATCHES];
static int           world_batch_count;
static int           world_last_slot = -1;
static bool          world_frame_prepared = false;


static int  current_tex_slot = 0;  // set before any emit_vert calls
static u16* index_buf;          // linearAlloc'd, fed to DrawElements
static u16  vert_tex_slots[WORLD_MAX_VERTS];  // tex slot for each vert

static void world_init(void)
{
    world_dvlb = DVLB_ParseFile((u32*)vshader_world_shbin, vshader_world_shbin_size);
    shaderProgramInit(&world_program);
    shaderProgramSetVsh(&world_program, &world_dvlb->DVLE[0]);

    world_uLoc_projection = shaderInstanceGetUniformLocation(world_program.vertexShader, "projection");

    world_vbo = linearAlloc(sizeof(world_vertex) * WORLD_MAX_VERTS);
    world_vbo_count = 0;

    index_buf = linearAlloc(WORLD_MAX_VERTS * sizeof(u16));
}

// Called by piggy_bitmap_page_out_all() after every level load.
void init_nds_textures(void)
{
    if (!gpu_inited) return;
    
    // Discard any pending verts that reference the old texture pool
    world_vbo_count = 0;
    world_batch_count = 0;
    world_last_slot = -1;
    world_frame_prepared = false;
    orphan_slot_count = 0;  // these slots are about to be freed anyway
    
    gpu_tex_free_all();
    gpu_tex_preload_all();
}


static inline void world_emit_vert(float x, float y, float z,
                                   float u, float v, float light, u32 color)
{
    if (world_vbo_count >= WORLD_MAX_VERTS) return;
    vert_tex_slots[world_vbo_count] = (u16)current_tex_slot;
    world_vertex* p = &world_vbo[world_vbo_count++];
    p->x = x; p->y = y; p->z = z;
    p->u = u; p->v = v;
    p->light = light;
    p->color = color;
}

// Unchecked variant — caller must have already verified capacity.
// Used in the hot paths after a single top-of-function bounds check.
static inline void world_emit_vert_unchecked(float x, float y, float z,
                                             float u, float v, float light, u32 color)
{
    vert_tex_slots[world_vbo_count] = (u16)current_tex_slot;
    world_vertex* p = &world_vbo[world_vbo_count++];
    p->x = x; p->y = y; p->z = z;
    p->u = u; p->v = v;
    p->light = light;
    p->color = color;
}

// Build the perspective projection for one eye.
//
// We construct the matrix as remap * shear * persp_tilt rather than calling
// Mtx_PerspStereoTilt directly. The shear translates camera-space X by
// iod*(Z-screen_dist)/screen_dist so that:
//   - objects at Z=screen_dist sit at screen depth (zero parallax)
//   - nearer objects pop out, farther objects recede
// This is equivalent to PerspStereoTilt but keeps the stereo math pre-
// perspective, dodging ordering issues with the post-tilt viewport remap.
//
// The final remap matrix confines output to the QUAD_* region (so the
// world renders into the same on-screen area as the bitblt quad).
static const float stereo_screen_dist = 8.0f;

static void world_build_projection(float iod)
{
    float fov_v_rad = WORLD_FOV_V_DEG * (float)M_PI / 180.0f;
    float fov_h_rad = WORLD_FOV_H_DEG * (float)M_PI / 180.0f;
    float effective_aspect = tanf(fov_h_rad * 0.5f) / tanf(fov_v_rad * 0.5f);

    Mtx_PerspTilt(&world_projection, fov_v_rad, effective_aspect,
                  WORLD_NEAR, WORLD_FAR, true);

    if (iod != 0.0f) {
        C3D_Mtx shear;
        Mtx_Identity(&shear);
        shear.r[0].z = iod / stereo_screen_dist;
        shear.r[0].w = -iod;

        C3D_Mtx tmp;
        Mtx_Multiply(&tmp, &world_projection, &shear);
        world_projection = tmp;
    }

    float quad_cx_ndc = ((QUAD_X0 + QUAD_X1) * 0.5f - SCREEN_W * 0.5f) / (SCREEN_W * 0.5f);
    float quad_cy_ndc = ((QUAD_Y0 + QUAD_Y1) * 0.5f - SCREEN_H * 0.5f) / (SCREEN_H * 0.5f);
    float quad_sx     = (QUAD_X1 - QUAD_X0) / (float)SCREEN_W;
    float quad_sy     = (QUAD_Y1 - QUAD_Y0) / (float)SCREEN_H;

    C3D_Mtx remap;
    Mtx_Identity(&remap);
    remap.r[0].x = quad_sx;
    remap.r[1].y = quad_sy;
    remap.r[0].w = quad_cx_ndc;
    remap.r[1].w = quad_cy_ndc;

    C3D_Mtx tmp;
    Mtx_Multiply(&tmp, &remap, &world_projection);
    world_projection = tmp;
}

TIMER_DECL(p3d_wait_time);
static void world_frame_begin(void)
{
    TIMER_START(p3d_t);
    gspWaitForP3D();
    TIMER_ADD(p3d_wait_time, p3d_t);

    world_vbo_count = 0;
    world_batch_count = 0;
    world_last_slot = -1;
    world_frame_prepared = false;   // <-- add this

    for (int i = 0; i < orphan_slot_count; i++) {
        gpu_tex_free_slot(orphan_slots[i]);
    }
    orphan_slot_count = 0;
}

static void world_setup_state(void)
{
    C3D_BindProgram(&world_program);

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);          // position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);          // texcoord
    AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 1);          // light
    AttrInfo_AddLoader(attrInfo, 3, GPU_UNSIGNED_BYTE, 4);  // color

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, world_vbo, sizeof(world_vertex), 4, 0x3210);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, world_uLoc_projection, &world_projection);

    // Modulate texture by interpolated vertex color (Gouraud lighting).
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

// Forward-declare citro3d internals (from citro3d's internal.h)
typedef struct C3D_Context_tag C3D_Context;
extern C3D_Context* C3Di_GetContext(void);
extern void C3Di_UpdateContext(void);
extern void C3Di_SetTex(int unit, C3D_Tex* tex);

static void world_frame_end(void)
{
    if (world_vbo_count == 0) return;

    assert(world_vbo_count % 3 == 0);
    assert(world_vbo_count <= WORLD_MAX_VERTS);

    world_setup_state();

    static int tex_counts[GPU_TEX_MAX];
    static u16 active_tex[WORLD_MAX_BATCHES];
    static int active_tex_count = 0;

    if (!world_frame_prepared) {
        int tex_offsets[GPU_TEX_MAX];
        int tex_cursor[GPU_TEX_MAX];
        static world_vertex sorted_vbo[WORLD_MAX_VERTS];

        for (int i = 0; i < active_tex_count; i++)
            tex_counts[active_tex[i]] = 0;
        active_tex_count = 0;

        const int tri_count = world_vbo_count / 3;

        // Pass 1: count triangles per texture
        for (int t = 0; t < tri_count; t++) {
            u16 slot = vert_tex_slots[t * 3];
            if (tex_counts[slot] == 0) {
                assert(active_tex_count < WORLD_MAX_BATCHES);
                active_tex[active_tex_count++] = slot;
            }
            tex_counts[slot] += 3;
        }

        // Pass 2: offsets
        int offset = 0;
        for (int i = 0; i < active_tex_count; i++) {
            u16 slot = active_tex[i];
            tex_offsets[slot] = offset;
            tex_cursor[slot]  = offset;
            offset += tex_counts[slot];
        }

        // Pass 3: scatter triangles
        for (int t = 0; t < tri_count; t++) {
            const int src = t * 3;
            u16 slot = vert_tex_slots[src];
            int dst = tex_cursor[slot];
            sorted_vbo[dst + 0] = world_vbo[src + 0];
            sorted_vbo[dst + 1] = world_vbo[src + 1];
            sorted_vbo[dst + 2] = world_vbo[src + 2];
            tex_cursor[slot] = dst + 3;
        }

        memcpy(world_vbo, sorted_vbo, world_vbo_count * sizeof(world_vertex));
        GSPGPU_FlushDataCache(world_vbo, world_vbo_count * sizeof(world_vertex));

        // Rebuild world_batches[] to reflect post-sort layout
        world_batch_count = 0;
        world_last_slot = -1;
        for (int i = 0; i < active_tex_count; i++) {
            u16 slot = active_tex[i];
            world_batch_t* batch = &world_batches[world_batch_count++];
            batch->tex        = gpu_tex_pool[slot].tex;
            batch->vert_start = tex_offsets[slot];
            batch->vert_count = tex_counts[slot];
        }

        world_frame_prepared = true;
    }

    // Draw — runs every eye
    // Per-frame draw setup (was emitted per-batch by C3D_DrawArrays)
    C3D_TexBind(0, world_batches[0].tex);
    C3Di_UpdateContext();  // full setup for first batch

    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_TRIANGLES);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, 0x80000000);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 1);

    // First batch
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, world_batches[0].vert_count);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, world_batches[0].vert_start);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);

    // Subsequent batches: only swap texture descriptor
    for (int i = 1; i < world_batch_count; i++) {
        world_batch_t* batch = &world_batches[i];
        C3Di_SetTex(0, batch->tex);
        GPUCMD_AddWrite(GPUREG_NUMVERTICES, batch->vert_count);
        GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, batch->vert_start);
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
        GPUCMD_AddWrite(GPUREG_DRAWARRAYS, 1);
        GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
        GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    }

    // Disable array drawing mode so subsequent draws (bitblt) start fresh
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 1, 0);}

// =============================================================================
// Flat-shaded polygons (OP_FLATPOLY)
// =============================================================================
// Used for laser bolts, weapon trails, particle effects. We render these
// as triangle fans textured with a shared 1x1 white texture, modulated by
// vertex color = palette[color_idx]. This routes through the normal world
// batching path so we get stereo + batching for free.
// =============================================================================
static C3D_Tex white_tex;
static bool    white_tex_ready = false;

extern ubyte gr_palette[];

static int white_tex_slot = -1;

static void white_tex_init(void)
{
    if (white_tex_ready) return;
    if (!C3D_TexInit(&white_tex, 8, 8, GPU_RGBA5551)) return;
    C3D_TexSetFilter(&white_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&white_tex, GPU_REPEAT, GPU_REPEAT);

    u16 src[64];
    for (int i = 0; i < 64; i++) src[i] = 0xFFFF;
    tex_upload_5551(&white_tex, src, 8, 8, 8);

    // Claim a dynamic slot so it plays nicely with the bucket sort
    white_tex_slot = GPU_TEX_PRELOADED_MAX;  // first dynamic slot
    gpu_tex_pool[white_tex_slot].tex     = &white_tex;
    gpu_tex_pool[white_tex_slot].u_scale = 1.0f;
    gpu_tex_pool[white_tex_slot].v_scale = 1.0f;
    gpu_tex_pool[white_tex_slot].owned   = false;

    white_tex_ready = true;
}

ITCM_CODE void g3_draw_poly_flat_color(int nv, vms_vector** pointlist, int color_idx)
{
    if (nv < 3) return;
    if (!white_tex_ready) white_tex_init();
    if (!white_tex_ready) return;
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    poly_count++;

    float r = (float)gr_palette[color_idx * 3 + 0] * (1.0f / 63.0f);
    float g = (float)gr_palette[color_idx * 3 + 1] * (1.0f / 63.0f);
    float b = (float)gr_palette[color_idx * 3 + 2] * (1.0f / 63.0f);
    u32 color = 0xFF000000 | ((u32)(b * 255) << 16) | ((u32)(g * 255) << 8) | (u32)(r * 255);

    int needed = (nv - 2) * 3;
    if (world_vbo_count + needed > WORLD_MAX_VERTS) return;

    world_batch_t* batch;
    if (world_last_slot == white_tex_slot && world_batch_count > 0) {
        batch = &world_batches[world_batch_count - 1];
    } else {
        if (world_batch_count >= WORLD_MAX_BATCHES) return;
        batch = &world_batches[world_batch_count++];
        batch->tex = &white_tex;
        batch->vert_start = world_vbo_count;
        batch->vert_count = 0;
        world_last_slot = white_tex_slot;
    }

    current_tex_slot = white_tex_slot;

    const float inv_fix = 1.0f / 65536.0f;

    const float v0x = (float)pointlist[0]->x * inv_fix;
    const float v0y = (float)pointlist[0]->y * inv_fix;
    const float v0z = (float)pointlist[0]->z * inv_fix;

    float prev_x = (float)pointlist[1]->x * inv_fix;
    float prev_y = (float)pointlist[1]->y * inv_fix;
    float prev_z = (float)pointlist[1]->z * inv_fix;

    for (int i = 1; i < nv - 1; i++) {
        const int j = i + 1;
        float nx = (float)pointlist[j]->x * inv_fix;
        float ny = (float)pointlist[j]->y * inv_fix;
        float nz = (float)pointlist[j]->z * inv_fix;

        world_emit_vert_unchecked(v0x,    v0y,    v0z,    0.5f, 0.5f, 1.0f, color);
        world_emit_vert_unchecked(prev_x, prev_y, prev_z, 0.5f, 0.5f, 1.0f, color);
        world_emit_vert_unchecked(nx,     ny,     nz,     0.5f, 0.5f, 1.0f, color);

        prev_x = nx; prev_y = ny; prev_z = nz;
    }
    batch->vert_count += needed;
}

// Legacy entry point: defaults to light gray. The current canvas color
// from gr_setcolor isn't plumbed through yet.
ITCM_CODE void g3_draw_poly(int nv, vms_vector** pointlist)
{
    g3_draw_poly_flat_color(nv, pointlist, 15);
}

// =============================================================================
// Missing-bitmap diagnostics
// =============================================================================
// When DEBUG_MISSING_TEXTURES is 1, log the first occurrence of each
// unique grs_bitmap* we reject. Useful when walls/sprites silently fail
// to render.
// =============================================================================
#define DEBUG_MISSING_TEXTURES 0

#if DEBUG_MISSING_TEXTURES
#define MISSING_LOG_MAX 64
static grs_bitmap* missing_logged[MISSING_LOG_MAX];
static int missing_logged_count = 0;

static void log_missing_bitmap(grs_bitmap* bm, const char* reason)
{
    for (int i = 0; i < missing_logged_count; i++) {
        if (missing_logged[i] == bm) return;
    }
    if (missing_logged_count >= MISSING_LOG_MAX) return;
    missing_logged[missing_logged_count++] = bm;

    printf("MISSING [%s]: bm=%p key=%d w=%d h=%d flags=0x%x data=%p\n",
           reason, (void*)bm, bm ? bm->key : -999,
           bm ? bm->bm_w : 0, bm ? bm->bm_h : 0,
           bm ? bm->bm_flags : 0, bm ? (void*)bm->bm_data : NULL);
}
#else
#define log_missing_bitmap(bm, reason) ((void)0)
#endif

// =============================================================================
// Textured polygons (g3_draw_tmap_tex)
// =============================================================================
// We don't have a real fade-table lookup on PICA200, so per-vertex lighting
// becomes a brightness multiplier sampled into the fragment color. The
// final pixel is texture * vertex_color via GPU_MODULATE.
// =============================================================================

TIMER_DECL(tmap_ticks);
TIMER_DECL(t_lookup);
TIMER_DECL(t_convert);
TIMER_DECL(t_emit);

ITCM_CODE void g3_draw_tmap_tex(int nv, vms_vector** pointlist,
                                g3s_uvl* uvl_list, grs_bitmap* bm)
{
    if (nv < 3 || !bm) return;
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    TIMER_START(t_total);
    TIMER_START_D(t_phase);

    poly_count++;

    // Resolve bitmap -> GPU texture slot. Four cases, in priority order:
    //   1. Composite key (texmerge): track per-bitmap, re-upload on content change.
    //   2. Valid preloaded key.
    //   3. bm_data hash hit (a copy of a preloaded bitmap).
    //   4. Brand-new bitmap: upload to a dynamic slot.
    int idx;
    if (is_composite_key(bm->key)) {
        texmerge_track_t* tt = texmerge_track_get(bm);
        if (tt->slot < 0 || tt->composite_key != bm->key) {
            // First sight, or content changed. Defer freeing the old slot
            // until next frame (see orphan_slots).
            if (tt->slot >= 0 && orphan_slot_count < ORPHAN_SLOTS_MAX) {
                orphan_slots[orphan_slot_count++] = tt->slot;
            }
            tt->slot = gpu_tex_upload_dynamic(bm);
            if (tt->slot < 0) {
                frame_lookup_failures++;
                log_missing_bitmap(bm, "texmerge-upload-failed");
                return;
            }
            tt->composite_key = bm->key;
            // upload_to_slot rewrote bm->key — restore the composite key
            // so the next content change is detectable.
            bm->key = tt->composite_key;
            frame_composite_uploads++;
        } else {
            frame_composite_reuses++;
        }
        idx = tt->slot;
    } else {
        idx = bm->key;
        if (idx < 0 || idx >= GPU_TEX_MAX || !gpu_tex_pool[idx].tex) {
            idx = bm_hash_lookup(bm->bm_data);
            if (idx < 0 || !gpu_tex_pool[idx].tex) {
                idx = gpu_tex_upload_dynamic(bm);
                if (idx < 0) {
                    frame_lookup_failures++;
                    log_missing_bitmap(bm, "upload-failed");
                    return;
                }
                frame_dynamic_uploads++;
            } else {
                bm->key = idx;  // cache for next frame
                frame_hash_hits++;
            }
        } else {
            frame_preloaded_hits++;
        }
    }
    gpu_tex_entry_t* gt = &gpu_tex_pool[idx];
    if (!gt->tex) {
        log_missing_bitmap(bm, "no-gpu-tex");
        return;
    }

    TIMER_ADD_D(t_lookup, t_phase);
    TIMER_RESET_D(t_phase);

    int needed = (nv - 2) * 3;
    if (world_vbo_count + needed > WORLD_MAX_VERTS) return;

    world_batch_t* batch;
    if (world_last_slot == idx && world_batch_count > 0) {
        batch = &world_batches[world_batch_count - 1];
    } else {
        if (world_batch_count >= WORLD_MAX_BATCHES) return;
        batch = &world_batches[world_batch_count++];
        batch->tex = gt->tex;
        batch->vert_start = world_vbo_count;
        batch->vert_count = 0;
        world_last_slot = idx;
    }

    current_tex_slot = idx;

    const float inv_fix = 1.0f / 65536.0f;
    const float u_scale = gt->u_scale;
    const float v_scale = gt->v_scale;

    // Vertex 0 is shared across every triangle of the fan — compute once.
    const float v0x = (float)pointlist[0]->x * inv_fix;
    const float v0y = (float)pointlist[0]->y * inv_fix;
    const float v0z = (float)pointlist[0]->z * inv_fix;
    const float v0u = (float)uvl_list[0].u * inv_fix * u_scale;
    const float v0v = v_scale - (float)uvl_list[0].v * inv_fix * v_scale;
    float v0l = (float)uvl_list[0].l * inv_fix;
    if (v0l < 0.0f) v0l = 0.0f;
    else if (v0l > 1.0f) v0l = 1.0f;

    // Vertex i+1 of one triangle is vertex i of the next — cache across iterations.
    float prev_x = (float)pointlist[1]->x * inv_fix;
    float prev_y = (float)pointlist[1]->y * inv_fix;
    float prev_z = (float)pointlist[1]->z * inv_fix;
    float prev_u = (float)uvl_list[1].u * inv_fix * u_scale;
    float prev_v = v_scale - (float)uvl_list[1].v * inv_fix * v_scale;
    float prev_l = (float)uvl_list[1].l * inv_fix;
    if (prev_l < 0.0f) prev_l = 0.0f;
    else if (prev_l > 1.0f) prev_l = 1.0f;

    for (int i = 1; i < nv - 1; i++) {
        const int j = i + 1;
        float nx = (float)pointlist[j]->x * inv_fix;
        float ny = (float)pointlist[j]->y * inv_fix;
        float nz = (float)pointlist[j]->z * inv_fix;
        float nu = (float)uvl_list[j].u * inv_fix * u_scale;
        float nv_ = v_scale - (float)uvl_list[j].v * inv_fix * v_scale;
        float nl = (float)uvl_list[j].l * inv_fix;
        if (nl < 0.0f) nl = 0.0f;
        else if (nl > 1.0f) nl = 1.0f;

        world_emit_vert_unchecked(v0x,    v0y,    v0z,    v0u,    v0v,    v0l,    0xFFFFFFFF);
        world_emit_vert_unchecked(prev_x, prev_y, prev_z, prev_u, prev_v, prev_l, 0xFFFFFFFF);
        world_emit_vert_unchecked(nx,     ny,     nz,     nu,     nv_,    nl,     0xFFFFFFFF);

        prev_x = nx; prev_y = ny; prev_z = nz;
        prev_u = nu; prev_v = nv_; prev_l = nl;
    }
    batch->vert_count += needed;

    TIMER_ADD_D(t_emit, t_phase);
    TIMER_ADD(tmap_ticks, t_total);
}

// Flat-shaded textured: in the software renderer this draws distant walls
// without UV mapping for perf. On a GPU there's no win, so route to the
// normal textured path. Tradeoff: cloaked enemies render textured rather
// than as shimmer; revisit if we want the original effect.
ITCM_CODE void g3_draw_tmap_flat(int nv, vms_vector** pointlist,
                                 g3s_uvl* uvl_list, grs_bitmap* bm)
{
    g3_draw_tmap_tex(nv, pointlist, uvl_list, bm);
}

// =============================================================================
// Billboard sprites (g3_draw_bitmap)
// =============================================================================
// 2D bitmaps at a world position, always facing the camera. We transform
// to camera space and emit a quad aligned with the camera XY axes.
// =============================================================================
#define MAX_LIGHT 0x10000

extern vms_vector View_position;
extern vms_matrix View_matrix;

static inline void world_to_camera(vms_vector* world_pt,
                                   float* out_x, float* out_y, float* out_z)
{
    vms_vector tempv, out;
    vm_vec_sub(&tempv, world_pt, &View_position);
    vm_vec_rotate(&out, &tempv, &View_matrix);
    *out_x = (float)out.x * (1.0f / 65536.0f);
    *out_y = (float)out.y * (1.0f / 65536.0f);
    *out_z = (float)out.z * (1.0f / 65536.0f);
}

// Emit a 4-vertex quad with explicit UVs (no V-flip — sprites use a
// different UV convention than walls).
//
// NOTE on u_scale/v_scale: applying them here causes sprites to crop. The
// GPU appears to already sample [0, 1] over the bitmap region rather than
// the POT region, but walls (via tmap_tex) do need the scaling. Something
// is inconsistent in how non-POT samplers are configured — not understood
// yet. For now sprites pass UVs through unscaled.
static void emit_sprite_quad(grs_bitmap* bm,
                             float x0, float y0, float z0, float u0, float v0,
                             float x1, float y1, float z1, float u1, float v1,
                             float x2, float y2, float z2, float u2, float v2,
                             float x3, float y3, float z3, float u3, float v3,
                             fix light)
{
    if (!bm) return;

    int idx;
    if (is_composite_key(bm->key)) {
        texmerge_track_t* tt = texmerge_track_get(bm);
        if (tt->slot < 0 || tt->composite_key != bm->key) {
            if (tt->slot >= 0 && orphan_slot_count < ORPHAN_SLOTS_MAX) {
                orphan_slots[orphan_slot_count++] = tt->slot;
            }
            tt->slot = gpu_tex_upload_dynamic(bm);
            if (tt->slot < 0) return;
            tt->composite_key = bm->key;
            bm->key = tt->composite_key;
        }
        idx = tt->slot;
    } else {
        idx = bm->key;
        if (idx < 0 || idx >= GPU_TEX_MAX || !gpu_tex_pool[idx].tex) {
            idx = bm_hash_lookup(bm->bm_data);
            if (idx < 0 || !gpu_tex_pool[idx].tex) {
                idx = gpu_tex_upload_dynamic(bm);
                if (idx < 0) return;
            } else {
                bm->key = idx;
            }
        }
    }
    gpu_tex_entry_t* gt = &gpu_tex_pool[idx];
    if (!gt->tex) return;

    float brightness = (float)light * (1.0f / 65536.0f);
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;

    // Near-plane clipping for a 4-vertex polygon.
    float in_cx[4] = {x0, x1, x2, x3};
    float in_cy[4] = {y0, y1, y2, y3};
    float in_cz[4] = {z0, z1, z2, z3};
    float in_u [4] = {u0, u1, u2, u3};
    float in_v [4] = {v0, v1, v2, v3};
    float in_l [4] = {brightness, brightness, brightness, brightness};

    float cx[6], cy[6], cz[6], cu[6], cv[6], cl[6];
    int out_nv = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        bool curr_in = in_cz[i] >= WORLD_NEAR;
        bool next_in = in_cz[j] >= WORLD_NEAR;
        if (curr_in) {
            cx[out_nv] = in_cx[i]; cy[out_nv] = in_cy[i]; cz[out_nv] = in_cz[i];
            cu[out_nv] = in_u[i];  cv[out_nv] = in_v[i];  cl[out_nv] = in_l[i];
            out_nv++;
        }
        if (curr_in != next_in) {
            float t = (WORLD_NEAR - in_cz[i]) / (in_cz[j] - in_cz[i]);
            cx[out_nv] = in_cx[i] + t * (in_cx[j] - in_cx[i]);
            cy[out_nv] = in_cy[i] + t * (in_cy[j] - in_cy[i]);
            cz[out_nv] = WORLD_NEAR;
            cu[out_nv] = in_u[i]  + t * (in_u[j]  - in_u[i]);
            cv[out_nv] = in_v[i]  + t * (in_v[j]  - in_v[i]);
            cl[out_nv] = in_l[i]  + t * (in_l[j]  - in_l[i]);
            out_nv++;
        }
    }
    if (out_nv < 3) return;

    int needed = (out_nv - 2) * 3;
    if (world_vbo_count + needed > WORLD_MAX_VERTS) return;

    world_batch_t* batch;
    if (world_last_slot == idx && world_batch_count > 0) {
        batch = &world_batches[world_batch_count - 1];
    } else {
        if (world_batch_count >= WORLD_MAX_BATCHES) return;
        batch = &world_batches[world_batch_count++];
        batch->tex = gt->tex;
        batch->vert_start = world_vbo_count;
        batch->vert_count = 0;
        world_last_slot = idx;
    }

    current_tex_slot = idx;
    for (int i = 1; i < out_nv - 1; i++) {
        world_emit_vert_unchecked(cx[0],   cy[0],   cz[0],   cu[0],   cv[0],   cl[0],   0xFFFFFFFF);
        world_emit_vert_unchecked(cx[i],   cy[i],   cz[i],   cu[i],   cv[i],   cl[i],   0xFFFFFFFF);
        world_emit_vert_unchecked(cx[i+1], cy[i+1], cz[i+1], cu[i+1], cv[i+1], cl[i+1], 0xFFFFFFFF);
    }
    batch->vert_count += needed;
}

ITCM_CODE void g3_draw_bitmap(vms_vector* pos, fix width, fix height, grs_bitmap* bm)
{
    if (!bm) return;

    float cx, cy, cz;
    world_to_camera(pos, &cx, &cy, &cz);

    // Non-POT sprites render vertically compressed unless we compensate
    // by 1/v_scale. The cause is tangled up with the same non-POT sampler
    // inconsistency described on emit_sprite_quad — not pretty, but visually
    // correct against original Descent.
    int idx2 = (bm->key >= 0 && bm->key < GPU_TEX_MAX) ? bm->key : 0;
    gpu_tex_entry_t* gt2 = &gpu_tex_pool[idx2];
    float v_compensate = (gt2->tex && gt2->v_scale > 0.0f) ? (1.0f / gt2->v_scale) : 1.0f;

    // width/height from Descent are radii (half-extents), not full extents.
    float hw = (float)width  * (1.0f / 65536.0f);
    float hh = (float)height * (1.0f / 65536.0f) * v_compensate;

    // V is inverted: top of sprite needs V=1 to sample top of image.
    float u_lo = 0.0f, u_hi = 1.0f;
    float v_lo = 1.0f, v_hi = 0.0f;

    emit_sprite_quad(bm,
                     cx - hw, cy + hh, cz,  u_lo, v_lo,   // top-left
                     cx + hw, cy + hh, cz,  u_hi, v_lo,   // top-right
                     cx + hw, cy - hh, cz,  u_hi, v_hi,   // bottom-right
                     cx - hw, cy - hh, cz,  u_lo, v_hi,   // bottom-left
                     MAX_LIGHT);
}

// =============================================================================
// Palette flash overlay
// =============================================================================
// Descent uses PaletteRedAdd/GreenAdd/BlueAdd to flash the screen (red on
// damage, blue on shield pickup, etc). On DOS this was done by modifying
// the palette. Our world textures were uploaded with the original palette,
// so we draw a translucent colored overlay on top of the 3D world instead.
// Drawn after world_frame_end but before the HUD bitblt, so the HUD is
// unaffected (matches DOS behavior — HUD was rendered via its own palette).
// =============================================================================

typedef struct { float x, y, z; float u, v; } tex_vertex;

extern int PaletteRedAdd, PaletteGreenAdd, PaletteBlueAdd;

// Fullscreen quad in screen space covering just the world viewport.
// Uses the same vertex format as the bitblt quad so we can reuse its
// buffer / shader pipeline.
static const tex_vertex flash_quad_list[] = {
    { 0,        SCREEN_H, 0.5f, 0.0f, 0.0f },
    { SCREEN_W, SCREEN_H, 0.5f, 0.0f, 0.0f },
    { SCREEN_W, 0,        0.5f, 0.0f, 0.0f },
    { 0,        SCREEN_H, 0.5f, 0.0f, 0.0f },
    { SCREEN_W, 0,        0.5f, 0.0f, 0.0f },
    { 0,        0,        0.5f, 0.0f, 0.0f },
};
#define flash_quad_count 6

static void* flash_vbo_data = NULL;

static void flash_overlay_init(void)
{
    if (flash_vbo_data) return;
    flash_vbo_data = linearAlloc(sizeof(flash_quad_list));
    memcpy(flash_vbo_data, flash_quad_list, sizeof(flash_quad_list));
}

static void draw_palette_flash_overlay(void)
{
    int r = PaletteRedAdd, g = PaletteGreenAdd, b = PaletteBlueAdd;
    if (r == 0 && g == 0 && b == 0) return;
    if (!flash_vbo_data) flash_overlay_init();

    C3D_BindProgram(&program);

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, flash_vbo_data, sizeof(tex_vertex), 2, 0x10);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    bool all_negative = (r <= 0 && g <= 0 && b <= 0) && (r < 0 || g < 0 || b < 0);

    if (all_negative) {
      // Cloak effect
      const float min_factor = 0.15f;
      int dim = -r;
      if (-g > dim) dim = -g;
      if (-b > dim) dim = -b;
      if (dim > MAX_PALETTE_ADD) dim = MAX_PALETTE_ADD;
      float factor = 1.0f - (1.0f - min_factor) * ((float)dim / MAX_PALETTE_ADD);
      if (factor < 0.0f) factor = 0.0f;
      u8 c = (u8)(factor * 255);
      u32 color = 0xFF000000 | ((u32)c << 16) | ((u32)c << 8) | (u32)c;
      C3D_TexEnvColor(env, color);
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                     GPU_DST_COLOR, GPU_ZERO,
                     GPU_DST_COLOR, GPU_ZERO);
      C3D_DrawArrays(GPU_TRIANGLES, 0, flash_quad_count);
    } else {
      int eff_r = (r > 0) ? r : 0;
      int eff_g = (g > 0) ? g : 0;
      int eff_b = (b > 0) ? b : 0;
      
      if (r < 0) { eff_g += (-r) / 4; eff_b += (-r) / 4; }
      if (g < 0) { eff_r += (-g) / 4; eff_b += (-g) / 4; }
      if (b < 0) { eff_r += (-b) / 4; eff_g += (-b) / 4; }

      const float intensity = 0.5f;
      u8 cr = (u8)((eff_r * 255 * intensity) / MAX_PALETTE_ADD);
      u8 cg = (u8)((eff_g * 255 * intensity) / MAX_PALETTE_ADD);
      u8 cb = (u8)((eff_b * 255 * intensity) / MAX_PALETTE_ADD);
      if (cr > 255) cr = 255;
      if (cg > 255) cg = 255;
      if (cb > 255) cb = 255;
      
      u32 color = 0xFF000000 | ((u32)cb << 16) | ((u32)cg << 8) | (u32)cr;
      C3D_TexEnvColor(env, color);
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                     GPU_ONE, GPU_ONE,
                     GPU_ONE, GPU_ONE);
      C3D_DrawArrays(GPU_TRIANGLES, 0, flash_quad_count);
    }

    // Restore standard blend
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

// =============================================================================
// Bitblt path — palette framebuffer to screen
// =============================================================================
static const tex_vertex quad_list[] = {
    { QUAD_X0, QUAD_Y1, 0.5f, 0.0f,       QUAD_TEX_V },
    { QUAD_X1, QUAD_Y1, 0.5f, QUAD_TEX_U, QUAD_TEX_V },
    { QUAD_X1, QUAD_Y0, 0.5f, QUAD_TEX_U, 1.0f       },
    { QUAD_X0, QUAD_Y1, 0.5f, 0.0f,       QUAD_TEX_V },
    { QUAD_X1, QUAD_Y0, 0.5f, QUAD_TEX_U, 1.0f       },
    { QUAD_X0, QUAD_Y0, 0.5f, 0.0f,       1.0f       },
};
#define quad_list_count 6

static C3D_Tex back_tex;

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

    C3D_CullFace(GPU_CULL_NONE);

    // PICA200 uses reversed-Z (near=1, far=0), so "nearer wins" = GEQUAL.
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    // Discard alpha-0 pixels. Without this, transparent sprite edges would
    // write depth, punching holes through geometry behind them.
    C3D_AlphaTest(true, GPU_GREATER, 0);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// Draws one eye's contents (bitblt quad + world geometry). Caller must
// have set up the render target and built world_projection for this eye.
//
// world_frame_end is invoked per-eye but only issues draw commands;
// world_vbo and the batch list are populated once per game frame and
// reused across eyes. That's the win of doing stereo this way — vertex
// transform happens once on CPU, GPU draws twice.
static void draw_screen_contents(void)
{
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_FRONT_CCW);

    world_frame_end();

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    //C3D_AlphaTest(false, GPU_ALWAYS, 0);
    draw_palette_flash_overlay();
    //C3D_AlphaTest(true, GPU_GREATER, 0);

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

    // Bitblt is the 2D UI layer drawn first. Disable depth writes so it
    // doesn't occlude world geometry drawn after it.
    C3D_DrawArrays(GPU_TRIANGLES, 0, quad_list_count);
}

// -----------------------------------------------------------------------------
// Palette cache — repack from 6-bit RGB to 32-bit RGBA only when palette
// actually changed.
// -----------------------------------------------------------------------------
static u32  packed_palette[256];
static u8   last_palette[768];

static bool update_packed_palette(void)
{
    extern ubyte gr_current_pal[];
    if (memcmp(last_palette, gr_current_pal, 768) == 0) return false;
    memcpy(last_palette, gr_current_pal, 768);

    for (int i = 0; i < 256; i++) {
        u8 r = gr_current_pal[i*3+0] << 2;
        u8 g = gr_current_pal[i*3+1] << 2;
        u8 b = gr_current_pal[i*3+2] << 2;
        u8 a = (i == 0) ? 0x00 : 0xFF;
        packed_palette[i] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
    }

  return true;
}

TIMER_DECL(bitblt_ticks);
TIMER_DECL(flush_ticks);
TIMER_DECL(render_left_ticks);
TIMER_DECL(render_right_ticks);
static u8 back_buffer_prev[GAME_W * GAME_H];

extern bool exit_requested;
extern jmp_buf exit_jmp;

void bitblt_to_screen(void)
{
    if (!aptMainLoop() && !exit_requested) {
        exit_requested = true;
        longjmp(exit_jmp, 1);  // unwinds the stack back to setjmp
    }

    TIMER_START(t_bitblt);

    hidScanInput();
    keyboard_handler();

    bool palette_updated = update_packed_palette();
    if (!swizzle_lut_init) init_swizzle_lut();

    // -------- Bitblt: palette indices -> tiled RGBA texture --------
    u32* dst = (u32*)back_tex.data;
    int tiles_per_row = TEX_W / 8;

    for (int ty = 0; ty < GAME_H / 8; ty++) {
        for (int tx = 0; tx < GAME_W / 8; tx++) {
            __builtin_prefetch(back_buffer + (ty * 8) * GAME_W + (tx + 2) * 8, 0, 0);

            u32* tile_dst = dst + (ty * tiles_per_row + tx) * 64;
            const u8* tile_src = back_buffer + (ty * 8) * GAME_W + (tx * 8);
            const u8* tile_prev = back_buffer_prev + (ty * 8) * GAME_W + (tx * 8);

            // Check all 8 rows of this tile
            if (!palette_updated) {
              bool dirty = false;
              for (int py = 0; py < 8 && !dirty; py++)
                  dirty = memcmp(tile_src + py * GAME_W, tile_prev + py * GAME_W, 8) != 0;

              if (!dirty) continue;
            }

            for (int py = 0; py < 8; py++) {
                const u8* row_src = tile_src + py * GAME_W;
                const u8* lut_row = &swizzle_lut[py * 8];
                tile_dst[lut_row[0]] = packed_palette[row_src[0]];
                tile_dst[lut_row[1]] = packed_palette[row_src[1]];
                tile_dst[lut_row[2]] = packed_palette[row_src[2]];
                tile_dst[lut_row[3]] = packed_palette[row_src[3]];
                tile_dst[lut_row[4]] = packed_palette[row_src[4]];
                tile_dst[lut_row[5]] = packed_palette[row_src[5]];
                tile_dst[lut_row[6]] = packed_palette[row_src[6]];
                tile_dst[lut_row[7]] = packed_palette[row_src[7]];
            }
        }
    }
    memcpy(back_buffer_prev, back_buffer, GAME_W * GAME_H);
    C3D_TexFlush(&back_tex);

    TIMER_ADD(bitblt_ticks, t_bitblt);

    // -------- Stereo setup --------
    // 3D slider returns 0..1; /3 matches citro3d's "tame the effect" tweak.
    float slider = osGet3DSliderState();
    float iod = slider / 3.0f;
    bool stereo = (iod > 0.0f);

    // Flush world_vbo from CPU cache so GPU sees fresh data. Without this,
    // stereo can sample stale cache lines because the right-eye draws are
    // submitted later and timing differences expose the coherency hole.
  
    TIMER_START(t_flush);
    if (world_vbo_count > 0) {
        GSPGPU_FlushDataCache(world_vbo, sizeof(world_vertex) * world_vbo_count);
    }

    // Pass 0 instead of C3D_FRAME_SYNCDRAW so CPU can start the next frame
    // while GPU is still rendering this one. SYNCDRAW capped us at 30 FPS.
    // Race protection: world_frame_begin's gspWaitForP3D gates CPU writes
    // to world_vbo; texture uploads via C3D_TexFlush are synchronous.
    // Outstanding risk: back_tex is re-uploaded every frame, so a CPU
    // overwrite during GPU read would tear the bitblt. If observed,
    // double-buffer back_tex.
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    TIMER_ADD(flush_ticks, t_flush);

    // -------- Left eye --------
    TIMER_START(t_left);
    C3D_RenderTargetClear(target_left, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target_left);
    world_build_projection(stereo ? -iod : 0.0f);
    draw_screen_contents();
    TIMER_ADD(render_left_ticks, t_left);

    // -------- Right eye (only if slider engaged) --------
    TIMER_START(t_right);
    if (stereo) {
        C3D_RenderTargetClear(target_right, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target_right);
        world_build_projection(iod);
        draw_screen_contents();
    }
    TIMER_ADD(render_right_ticks, t_right);

    C3D_FrameEnd(0);

#ifdef DEBUG_TIMING
    static int debug_frame = 0;
    if (++debug_frame >= 10) {
        debug_frame = 0;
        printf("preload hits=%d, hash hits=%d, dynamic uploads=%d, composite uploads=%d, composite reuses=%d, lookup failures=%d\n",
               frame_preloaded_hits, frame_hash_hits, frame_dynamic_uploads,
               frame_composite_uploads, frame_composite_reuses, frame_lookup_failures);
        printf("bitblt %.2f ms, tmap %.2f ms (lookup %.2f, convert %.2f, emit %.2f)\n",
               TIMER_MS(bitblt_ticks),
               TIMER_MS(tmap_ticks), TIMER_MS(t_lookup), TIMER_MS(t_convert), TIMER_MS(t_emit));
        printf("flush %.2f ms, render left %.2f ms, render right %.2f ms\n",
               TIMER_MS(flush_ticks), TIMER_MS(render_left_ticks), TIMER_MS(render_right_ticks));
        printf("world_vbo_count=%d\n", world_vbo_count);
        printf("world_batch_count=%d\n", world_batch_count);
        printf("p3d waits=%.2f ms\n", TIMER_MS(p3d_wait_time));
    }
    bitblt_ticks = 0; render_left_ticks = 0; render_right_ticks = 0; flush_ticks = 0;
    tmap_ticks = 0; t_lookup = 0; t_convert = 0; t_emit = 0;
    p3d_wait_time = 0;
#endif

    poly_count = 0;
    frame_preloaded_hits = 0;
    frame_hash_hits = 0;
    frame_dynamic_uploads = 0;
    frame_composite_uploads = 0;
    frame_composite_reuses = 0;
    frame_lookup_failures = 0;

    world_frame_begin();
}

// =============================================================================
// GPU bring-up
// =============================================================================
void init_3ds_gpu(void)
{
    gfxSet3D(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    target_left = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W,
                                         GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target_left, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    target_right = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W,
                                          GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target_right, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

    sceneInit();
    world_init();

    gpu_inited = true;

    // If piggy already loaded bitmaps before init_3ds_gpu was called, do
    // the initial preload here. Later level changes go via init_nds_textures.
    extern int Num_bitmap_files;
    if (Num_bitmap_files > 0) {
        gpu_tex_preload_all();
    }
}
