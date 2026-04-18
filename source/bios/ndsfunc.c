// =============================================================================
// ndsfunc.c — 3DS rendering backend
// =============================================================================
// Renders the game's 320x200 palette-indexed back_buffer to the top screen by:
//   1. Converting palette indices to RGBA8 in a linear staging buffer
//   2. Software-tiling the staging buffer into a PICA200 tiled texture
//   3. Drawing a textured quad covering the top screen
//
// Then on top of that, renders the world (level geometry) natively via
// either textured triangles or wireframe lines, controlled by WORLD_MODE.
//
// Palette index 0 in back_buffer is treated as transparent (alpha blended
// against clear color).
//
// Debug render modes for the bitblt path are available via RENDER_MODE.
// See "Debug modes" section near the bottom of this file.
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
#include "vshader_wire_shbin.h"
#include "vshader_world_shbin.h"

// -----------------------------------------------------------------------------
// Render mode selection (for the bitblt quad — separate from world rendering)
// -----------------------------------------------------------------------------
#define RENDER_MODE_BITBLT       0  // Normal gameplay: palette -> RGBA -> quad
#define RENDER_MODE_TRIANGLE     1  // Debug: colored triangle (requires color shader)
#define RENDER_MODE_SOLID        2  // Debug: solid-red quad
#define RENDER_MODE_CHECKERBOARD 3  // Debug: 32x32 red/white checker
#define RENDER_MODE_RAW_WRITE    4  // Debug: bypass tiler, write tex->data directly

#define RENDER_MODE  RENDER_MODE_BITBLT

// -----------------------------------------------------------------------------
// World rendering mode (for level geometry)
// -----------------------------------------------------------------------------
#define WORLD_MODE_TEXTURED   0  // Real textured triangles (Phase 1: no lighting)
#define WORLD_MODE_WIREFRAME  1  // Debug: wireframe outlines

#define WORLD_MODE  WORLD_MODE_TEXTURED

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
// GPU state (bitblt)
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
// Hoisted here (above the render-mode switch) because the world renderer
// needs these values to map 3D into the same screen area as the bitblt.
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
#define CLEAR_COLOR  0x68B0D8FF

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// =============================================================================
// Stubs for game-side hooks that this backend doesn't need on 3DS.
// =============================================================================
ITCM_CODE void sync_palette()        {}
ITCM_CODE void irq_Vblank()          {}
ITCM_CODE void irq_arm9_fifo()       {}
void ds_start_frame()                {}
void ds_end_frame()                  {}
void nds_set_render_size(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void nds_init()                      {}

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
// gpu_tex — preloaded GPU texture pool
// =============================================================================
// One PICA200 texture per Descent bitmap. Allocated at preload (RGBA5551
// format = 16 bits/texel, half the memory of RGBA8). Each bitmap's index
// into GameBitmaps[] is the same as its index into our gpu_tex_pool[].
//
// Hooked into init_nds_textures() which is called by piggy_bitmap_page_out_all()
// after every level load. So we rebuild the GPU texture pool whenever the
// game flushes its bitmap cache.
//
// Phase 1: no lighting, just raw textured polygons. Phase 2 will add the
// fade-table-based vertex coloring.
// =============================================================================

#define GPU_TEX_PRELOADED_MAX  MAX_BITMAP_FILES   // 1500-1800 from piggy.h
#define GPU_TEX_DYNAMIC_MAX    1024                // texmerge composites etc.
#define GPU_TEX_MAX           (GPU_TEX_PRELOADED_MAX + GPU_TEX_DYNAMIC_MAX)

typedef struct {
    C3D_Tex* tex;        // NULL if not loaded
    float    u_scale;    // bm_w / pot_w  (for non-POT bitmaps)
    float    v_scale;    // bm_h / pot_h
} gpu_tex_entry_t;

static gpu_tex_entry_t gpu_tex_pool[GPU_TEX_MAX];
static int gpu_tex_loaded_count;
static int gpu_tex_total_bytes;

// -----------------------------------------------------------------------------
// Texmerge composite tracking
//
// Descent's texmerge.c maintains a tiny LRU cache of 10 grs_bitmap* slots.
// When the game needs an 11th composite, one of the existing slots gets
// REUSED — same struct pointer, same bm_data buffer, but freshly computed
// pixels for a different (top, bottom, orientation) combination.
//
// If we just upload once on first sight and trust bm->key forever, we'll
// keep showing stale GPU textures while the game has overwritten the
// underlying data. Result: textures cycle wildly as the player moves.
//
// texmerge sets bmp->key to a composite hash (orient<<30 | top<<16 | bottom)
// every time it (re)builds a slot. That hash is unique per (top, bottom,
// orient) combination. So we cache the hash alongside the GPU slot, and
// re-upload whenever the hash changes for a given grs_bitmap*.
//
// MUST be >= MAX_NUM_CACHE_BITMAPS in texmerge.c. The original is 10.
// Bump this if you bump the texmerge cache.
#define TEXMERGE_TRACKED_MAX  256

typedef struct {
    grs_bitmap* bm;            // NULL = empty
    int         composite_key; // last-seen bm->key
    int         slot;          // gpu_tex_pool slot
} texmerge_track_t;

static texmerge_track_t texmerge_track[TEXMERGE_TRACKED_MAX];

// Orphaned slots from the current frame.
//
// When a texmerge bitmap's content changes mid-frame, we MUST NOT free the
// old GPU texture immediately, because earlier batches in world_batches[]
// still hold a C3D_Tex* pointing at it. Freeing now causes use-after-free
// when those batches finally render in C3D_FrameEnd.
//
// Instead we mark the slot as orphaned and free it at the START of the
// NEXT frame, after C3D_FrameBegin(SYNCDRAW) has waited for the previous
// frame's GPU work to complete.
//
// Sized generously — even with a huge texmerge cache, mid-frame evictions
// are rare. 128 is plenty.
#define ORPHAN_SLOTS_MAX  128
static int orphan_slots[ORPHAN_SLOTS_MAX];
static int orphan_slot_count = 0;

// A "composite-looking" key has high bits set (because of the orient<<30).
// Plain bitmap_index values are small positive ints (0..1800).
static inline bool is_composite_key(int key)
{
    // Either way out of bitmap_index range, or has top bits set
    return (key < 0) || (key >= GPU_TEX_PRELOADED_MAX);
}

// -----------------------------------------------------------------------------
// bm_data pointer -> bitmap index reverse lookup.
//
// Some grs_bitmap structs the game passes to g3_draw_tmap_tex are NOT the
// originals from GameBitmaps[] — they're copies (e.g. Textures[] entries,
// effect bitmaps, animated wall snapshots). Those copies share the same
// bm_data pointer as the original (because piggy_bitmap_page_in malloc'd
// the data once and everyone references it), but their `key` field is
// uninitialized garbage.
//
// We build a hash from bm_data pointer to original index at preload time,
// then look up via this hash when key is invalid.
//
// Open-addressed linear probing, sized 2x the bitmap count for low load
// factor (~50%) so lookups stay O(1) on average.
// -----------------------------------------------------------------------------
#define BM_HASH_SIZE  (MAX_BITMAP_FILES * 2)

typedef struct {
    void* data;   // bm_data pointer (NULL = empty slot)
    int   index; // bitmap_index this data belongs to
} bm_hash_entry_t;

static bm_hash_entry_t bm_hash[BM_HASH_SIZE];

static inline u32 bm_hash_func(void* p)
{
    // Mix the high bits down (most pointer entropy is in the middle bits)
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
            // Already present (multiple bitmaps sharing the same data buffer)
            // First-inserted wins; nothing to do.
            return;
        }
    }
    // Table full — should never happen at 50% load factor
}

// Returns -1 if not found.
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

// Round up to next power of two
static inline int next_pot(int n)
{
    int p = 8; // PICA200 minimum texture dimension
    while (p < n) p <<= 1;
    return p;
}

// Convert a 6-bit palette entry to RGBA5551.
// Descent palette is 0..63 per channel; RGBA5551 is 0..31 per channel.
// So we shift right by 1 to drop one bit.
static inline u16 palette_to_rgba5551(int idx, ubyte* palette, bool transparent_zero)
{
    if (transparent_zero && idx == 255) {
        // Color index 255 in Descent's palette is the "transparent" color
        return 0; // alpha = 0, transparent
    }
    u8 r = palette[idx * 3 + 0] >> 1;  // 6-bit -> 5-bit
    u8 g = palette[idx * 3 + 1] >> 1;
    u8 b = palette[idx * 3 + 2] >> 1;
    // RGBA5551 layout in PICA200 memory: R(5) G(5) B(5) A(1) packed little-endian
    // Bits: RRRRRGGGGGBBBBBA
    return (r << 11) | (g << 6) | (b << 1) | 1;
}

// Software Morton-code tiler for RGBA5551 (16-bit) textures.
// Same Z-order layout as our RGBA8 tiler but at u16 stride.
static void tex_upload_5551(C3D_Tex* tex, const u16* src_linear,
                            int src_w, int src_h, int tex_w)
{
    u16* dst = (u16*)tex->data;
    int tiles_per_row = tex_w / 8;

    // Zero out the entire texture first (in case src_w/h < tex_w/h)
    memset(dst, 0, tex_w * tex->height * sizeof(u16));

    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int px = x & 7, py = y & 7;
            int z = (px & 1)        | ((py & 1) << 1) |
                    ((px & 2) << 1) | ((py & 2) << 2) |
                    ((px & 4) << 2) | ((py & 4) << 3);
            int tile_idx = (y / 8) * tiles_per_row + (x / 8);
            dst[tile_idx * 64 + z] = src_linear[y * src_w + x];
        }
    }
    C3D_TexFlush(tex);
}

// Decompress an RLE-compressed bitmap into a flat indexed buffer.
// Descent's RLE format: rows are independently compressed. For now we use
// the existing software path (gr_bm_ubitblt_rle) by faking a destination
// grs_bitmap. Returns an allocated buffer the caller must free.
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
    dest.bm_type = 0;  // linear

    gr_bm_ubitblt_rle(w, h, 0, 0, 0, 0, bmp, &dest);
    return out;
}

// Core upload: take any bitmap and put it in the given pool slot.
// Returns true on success. Used by both preload and dynamic upload paths.
static bool gpu_tex_upload_to_slot(grs_bitmap* bmp, int slot)
{
    if (bmp->bm_w <= 0 || bmp->bm_h <= 0) return false;
    if (!bmp->bm_data) return false;
    if (slot < 0 || slot >= GPU_TEX_MAX) return false;
    if (gpu_tex_pool[slot].tex) return false;  // slot already used

    // Decompress if RLE
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

    // Convert indexed -> RGBA5551 in a linear buffer at full size (bm_w x bm_h).
    // The tiler will pad to POT during the tile step.
    u16* rgba = malloc(bmp->bm_w * bmp->bm_h * sizeof(u16));
    if (!rgba) {
        if (we_allocated) free(indexed_data);
        return false;
    }
    for (int i = 0; i < bmp->bm_w * bmp->bm_h; i++) {
        rgba[i] = palette_to_rgba5551(indexed_data[i], gr_palette, has_transparency);
    }

    // Allocate GPU texture
    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    if (!tex || !C3D_TexInit(tex, pot_w, pot_h, GPU_RGBA5551)) {
        if (tex) free(tex);
        free(rgba);
        if (we_allocated) free(indexed_data);
        return false;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);  // walls tile

    // Tile into GPU texture
    tex_upload_5551(tex, rgba, bmp->bm_w, bmp->bm_h, pot_w);

    free(rgba);
    if (we_allocated) free(indexed_data);

    // Store in pool
    gpu_tex_pool[slot].tex     = tex;
    gpu_tex_pool[slot].u_scale = (float)bmp->bm_w / (float)pot_w;
    gpu_tex_pool[slot].v_scale = (float)bmp->bm_h / (float)pot_h;
    gpu_tex_total_bytes += pot_w * pot_h * sizeof(u16);
    gpu_tex_loaded_count++;

    // Tag the bitmap with our slot so g3_draw_tmap_tex can find us.
    bmp->key = slot;

    // Also register in the data-pointer hash so future bitmaps that share
    // this bm_data buffer can be resolved via reverse lookup.
    bm_hash_insert(bmp->bm_data, slot);

    return true;
}

// Preload path: upload a bitmap from GameBitmaps[idx] into pool slot idx.
static bool gpu_tex_upload_one(int idx)
{
    return gpu_tex_upload_to_slot(&GameBitmaps[idx], idx);
}

// Dynamic upload path: find a free slot in the dynamic range and upload there.
// Returns the slot index on success, or -1 on failure (no free slots, or
// the bitmap is invalid). Used at draw time for texmerge composites and
// other bitmaps not in GameBitmaps[].
static int gpu_tex_upload_dynamic(grs_bitmap* bmp)
{
    if (!bmp || !bmp->bm_data) return -1;

    // Linear scan for a free slot in the dynamic range. Slow if dynamic
    // pool fills up but typically we'll find a slot in the first few
    // iterations since textures get added once at level load and stay.
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
    return -1;  // dynamic pool full
}

// Free a single slot in the pool. Used for re-uploading a texmerge
// composite when its content has changed.
static void gpu_tex_free_slot(int slot)
{
    if (slot < 0 || slot >= GPU_TEX_MAX) return;
    if (!gpu_tex_pool[slot].tex) return;
    C3D_TexDelete(gpu_tex_pool[slot].tex);
    free(gpu_tex_pool[slot].tex);
    gpu_tex_pool[slot].tex = NULL;
    gpu_tex_loaded_count--;
}

// Free the entire GPU texture pool. Called at the start of each rebuild.
static void gpu_tex_free_all(void)
{
    for (int i = 0; i < GPU_TEX_MAX; i++) {
        if (gpu_tex_pool[i].tex) {
            C3D_TexDelete(gpu_tex_pool[i].tex);
            free(gpu_tex_pool[i].tex);
            gpu_tex_pool[i].tex = NULL;
        }
    }
    bm_hash_clear();
    memset(texmerge_track, 0, sizeof(texmerge_track));
    gpu_tex_loaded_count = 0;
    gpu_tex_total_bytes = 0;
}

// Find or create a texmerge tracking entry for this bitmap.
// Returns a pointer to the entry. Never returns NULL (we evict if full).
static texmerge_track_t* texmerge_track_get(grs_bitmap* bm)
{
    // First pass: existing entry
    for (int i = 0; i < TEXMERGE_TRACKED_MAX; i++) {
        if (texmerge_track[i].bm == bm) return &texmerge_track[i];
    }
    // Second pass: empty slot
    for (int i = 0; i < TEXMERGE_TRACKED_MAX; i++) {
        if (texmerge_track[i].bm == NULL) {
            texmerge_track[i].bm = bm;
            texmerge_track[i].composite_key = 0;  // sentinel: forces re-upload
            texmerge_track[i].slot = -1;
            return &texmerge_track[i];
        }
    }
    // Full — evict slot 0 (texmerge has only 10 entries; 16 should always
    // be enough so we should never get here)
    if (texmerge_track[0].slot >= 0) {
        gpu_tex_free_slot(texmerge_track[0].slot);
    }
    texmerge_track[0].bm = bm;
    texmerge_track[0].composite_key = 0;
    texmerge_track[0].slot = -1;
    return &texmerge_track[0];
}

// Page in every bitmap that has a piggy file offset, then upload it to GPU.
// This is the "preload everything" step.
//
// page_out_all() just freed all bm_data and set BM_FLAG_PAGED_OUT, so we
// need to call piggy_bitmap_page_in() to reload from disk before upload.
static void gpu_tex_preload_all(void)
{
    extern int Num_bitmap_files;

    for (int i = 0; i < Num_bitmap_files; i++) {
        bitmap_index bi;
        bi.index = i;

        // PIGGY_PAGE_IN equivalent: force load from disk if paged out
        if (GameBitmaps[i].bm_flags & BM_FLAG_PAGED_OUT) {
            piggy_bitmap_page_in(bi);
        }

        gpu_tex_upload_one(i);
    }

    printf("gpu_tex: loaded %d/%d bitmaps, %d KB GPU memory\n",
           gpu_tex_loaded_count, Num_bitmap_files,
           gpu_tex_total_bytes / 1024);
}

// init_nds_textures is called by piggy_bitmap_page_out_all() after every
// level load. We use this hook to rebuild our GPU texture pool.
//
// IMPORTANT: page_out_all() runs BEFORE this, so all bm_data is freed at
// this point. gpu_tex_preload_all() will page everything back in.
void init_nds_textures(void)
{
    // Skip if GPU isn't initialized yet (called once before init_3ds_gpu)
    extern bool gpu_inited;
    if (!gpu_inited) return;

    gpu_tex_free_all();
    gpu_tex_preload_all();
}

bool gpu_inited = false;

// =============================================================================
// World renderer — shared between WIREFRAME and TEXTURED modes
// =============================================================================
// Both modes share: perspective projection, clipping, the per-frame VBO ring,
// init/draw lifecycle. They differ in vertex format and shader.
// =============================================================================

// Descent's original display was 320x200 stretched to a 4:3 CRT, giving
// taller-than-wide pixels. The game's "standard" FOV is roughly 90 degrees
// horizontal across 4:3, which works out to about 73.7 degrees vertical
// (since tan(73.7/2) = tan(90/2) * (3/4) when accounting for aspect).
//
// On the 3DS we render to a quad that's roughly 4:3, but pixels are square.
// To preserve Descent's feel — wide horizontally, less vertically — we
// override the horizontal FOV explicitly rather than computing it from
// vertical FOV * aspect.
//
// You can tune these to taste. Typical values:
//   - 90 horizontal / 75 vertical: Descent original
//   - 95 horizontal / 75 vertical: feels slightly wider, more modern
//   - 75 horizontal / 60 vertical: zoomed-in, "telephoto" look
#define WIRE_FOV_H_DEG    56.0f   // horizontal FOV
#define WIRE_FOV_V_DEG    60.0f   // vertical FOV

// Near/far clip planes in Descent units (F1_0 = 65536 = 1 unit)
#define WIRE_NEAR         0.1f
#define WIRE_FAR          10000.0f

// Line thickness in screen pixels (wireframe only)
#define WIRE_THICKNESS    1.0f

// Max verts per frame. 6 verts per edge for wireframe, ~6 verts per quad
// for textured (2 tris). Either way, 4096*6 is a safe upper bound.
#define WIRE_MAX_VERTS    (4096 * 6)

// Vertex format depends on mode
#if WORLD_MODE == WORLD_MODE_WIREFRAME
typedef struct { float x, y, z; } world_vertex;
#else
typedef struct { float x, y, z, u, v; } world_vertex;
#endif

static shaderProgram_s  world_program;
static DVLB_s*          world_dvlb;
static int              world_uLoc_projection;
static int              world_uLoc_color;     // wireframe only
static C3D_Mtx          world_projection;
static world_vertex*    world_vbo;
static int              world_vbo_count;

// In TEXTURED mode we accumulate verts into per-texture batches. We can't
// issue draw calls from inside g3_draw_tmap_tex because that runs outside
// the C3D_FrameBegin/End block, so all verts get collected and drawn in
// world_frame_end(). Each batch is a contiguous range in world_vbo plus
// the texture to bind.
#if WORLD_MODE == WORLD_MODE_TEXTURED
#define WORLD_MAX_BATCHES  256  // unique textures per frame
typedef struct {
    C3D_Tex* tex;
    int      vert_start;
    int      vert_count;
} world_batch_t;
static world_batch_t world_batches[WORLD_MAX_BATCHES];
static int world_batch_count;
static C3D_Tex* world_last_tex;  // for coalescing consecutive same-texture polys
#endif

// Precomputed projection scale factors
static float world_focal;
static float world_aspect_inv;
static float world_half_w;
static float world_half_h;
static float world_center_x;
static float world_center_y;

static void world_init(void)
{
#if WORLD_MODE == WORLD_MODE_WIREFRAME
    world_dvlb = DVLB_ParseFile((u32*)vshader_wire_shbin, vshader_wire_shbin_size);
#else
    world_dvlb = DVLB_ParseFile((u32*)vshader_world_shbin, vshader_world_shbin_size);
#endif
    shaderProgramInit(&world_program);
    shaderProgramSetVsh(&world_program, &world_dvlb->DVLE[0]);

    world_uLoc_projection = shaderInstanceGetUniformLocation(world_program.vertexShader, "projection");
#if WORLD_MODE == WORLD_MODE_WIREFRAME
    world_uLoc_color = shaderInstanceGetUniformLocation(world_program.vertexShader, "wireColor");
#endif

#if WORLD_MODE == WORLD_MODE_WIREFRAME
    // Wireframe still uses CPU-side projection + screen-space ortho on GPU.
    // The line-quad extrusion math operates in screen space, so this
    // approach stays as-is.
    Mtx_OrthoTilt(&world_projection, 0.0f, SCREEN_W, SCREEN_H, 0.0f,
                  -1.0f, 1.0f, true);
#else
    // TEXTURED mode: real perspective projection on the GPU. Camera-space
    // vertices come in, the GPU divides by w to project AND interpolates
    // texcoords perspective-correctly (which is the whole point of moving
    // projection to the GPU).
    //
    // We build the matrix to:
    //   1. Apply perspective projection with our FOV
    //   2. Flip Y so +Y up in Descent maps to screen +Y up (PICA200 NDC has +Y up)
    //   3. Flip Z direction (Descent +Z forward, PICA200 wants -Z forward)
    //   4. Apply the 3DS's 90-degree screen rotation (tilt)
    //   5. Map the NDC output to the QUAD region instead of full screen
    //
    // We use Mtx_PerspTilt for steps 1, 3, 4 but pass an "effective aspect"
    // computed from our explicit horizontal/vertical FOVs rather than the
    // physical screen aspect. That way the matrix produces the H/V FOV
    // combination we asked for, even if it doesn't match the screen aspect.
    float fov_v_rad   = WIRE_FOV_V_DEG * (float)M_PI / 180.0f;
    float fov_h_rad   = WIRE_FOV_H_DEG * (float)M_PI / 180.0f;
    // For perspective projection: tan(fov_h/2) = aspect * tan(fov_v/2)
    // So the aspect we feed to Mtx_PerspTilt is the FOV ratio, not screen ratio.
    float effective_aspect = tanf(fov_h_rad * 0.5f) / tanf(fov_v_rad * 0.5f);
    Mtx_PerspTilt(&world_projection, fov_v_rad, effective_aspect,
                  WIRE_NEAR, WIRE_FAR, true);  // true = left-handed (+Z forward, matches Descent)

    // Post-multiply by a scale+translate to squash output into the quad
    // region. NDC is [-1, 1]; we want the quad's screen footprint.
    // Scale factor: quad_width / screen_width on each axis.
    // Translate: shift center of NDC to center of quad in NDC.
    float quad_cx_ndc = ((QUAD_X0 + QUAD_X1) * 0.5f - SCREEN_W * 0.5f) / (SCREEN_W * 0.5f);
    float quad_cy_ndc = ((QUAD_Y0 + QUAD_Y1) * 0.5f - SCREEN_H * 0.5f) / (SCREEN_H * 0.5f);
    float quad_sx     = (QUAD_X1 - QUAD_X0) / (float)SCREEN_W;
    float quad_sy     = (QUAD_Y1 - QUAD_Y0) / (float)SCREEN_H;

    // Build viewport remap matrix: out = scale * in + translate (per axis).
    // Because of OrthoTilt-style rotation, X and Y are swapped in the
    // post-tilt coordinate system. The remap is applied in pre-tilt space.
    C3D_Mtx remap;
    Mtx_Identity(&remap);
    remap.r[0].x = quad_sx;
    remap.r[1].y = quad_sy;
    remap.r[0].w = quad_cx_ndc;
    remap.r[1].w = quad_cy_ndc;

    C3D_Mtx tmp;
    Mtx_Multiply(&tmp, &remap, &world_projection);
    world_projection = tmp;
#endif

#if WORLD_MODE == WORLD_MODE_WIREFRAME
    // Precompute focal length and viewport mapping (wireframe only — uses
    // CPU-side projection in world_project). Uses vertical FOV; horizontal
    // is implicitly derived via aspect ratio.
    float fov_rad = WIRE_FOV_V_DEG * (float)M_PI / 180.0f;
    world_focal      = 1.0f / tanf(fov_rad * 0.5f);
    world_aspect_inv = (float)GAME_H / (float)GAME_W;

    world_half_w   = (QUAD_X1 - QUAD_X0) * 0.5f;
    world_half_h   = (QUAD_Y1 - QUAD_Y0) * 0.5f;
    world_center_x = (QUAD_X0 + QUAD_X1) * 0.5f;
    world_center_y = (QUAD_Y0 + QUAD_Y1) * 0.5f;
#endif

    world_vbo = linearAlloc(sizeof(world_vertex) * WIRE_MAX_VERTS);
    world_vbo_count = 0;
}

// Project a camera-space point (16.16 fixed) to 3DS screen space.
// Returns true if in front of the camera, false if culled.
//
// Descent camera space: +X right, +Y up, +Z forward (left-handed).
// We flip Y so screen-y grows downward.
static inline bool world_project(fix cx, fix cy, fix cz,
                                 float* out_sx, float* out_sy)
{
    float x = (float)cx * (1.0f / 65536.0f);
    float y = (float)cy * (1.0f / 65536.0f);
    float z = (float)cz * (1.0f / 65536.0f);

    if (z < WIRE_NEAR || z > WIRE_FAR) return false;

    float ndc_x =  (x / z) * world_focal * world_aspect_inv;
    float ndc_y = -(y / z) * world_focal;

    *out_sx = world_center_x + ndc_x * world_half_w;
    *out_sy = world_center_y + ndc_y * world_half_h;
    return true;
}

#if WORLD_MODE == WORLD_MODE_WIREFRAME
// -----------------------------------------------------------------------------
// Wireframe vertex emission — line segments rendered as thin quads
// -----------------------------------------------------------------------------
static void world_emit_line(float x0, float y0, float x1, float y1)
{
    if (world_vbo_count + 6 > WIRE_MAX_VERTS) return;

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;

    float px = -dy / len * (WIRE_THICKNESS * 0.5f);
    float py =  dx / len * (WIRE_THICKNESS * 0.5f);

    world_vertex* v = &world_vbo[world_vbo_count];
    v[0].x = x0 + px; v[0].y = y0 + py; v[0].z = 0.0f;
    v[1].x = x0 - px; v[1].y = y0 - py; v[1].z = 0.0f;
    v[2].x = x1 + px; v[2].y = y1 + py; v[2].z = 0.0f;
    v[3].x = x1 + px; v[3].y = y1 + py; v[3].z = 0.0f;
    v[4].x = x0 - px; v[4].y = y0 - py; v[4].z = 0.0f;
    v[5].x = x1 - px; v[5].y = y1 - py; v[5].z = 0.0f;

    world_vbo_count += 6;
}
#else
// -----------------------------------------------------------------------------
// Textured vertex emission — fan-triangulated polygon with UVs
// Now takes camera-space 3D position; the GPU vertex shader projects.
// -----------------------------------------------------------------------------
static inline void world_emit_vert(float x, float y, float z, float u, float v)
{
    if (world_vbo_count >= WIRE_MAX_VERTS) return;
    world_vertex* p = &world_vbo[world_vbo_count++];
    p->x = x; p->y = y; p->z = z;
    p->u = u; p->v = v;
}
#endif

static void world_frame_begin(void)
{
    world_vbo_count = 0;
#if WORLD_MODE == WORLD_MODE_TEXTURED
    world_batch_count = 0;
    world_last_tex = NULL;

    // Free any GPU textures orphaned during the PREVIOUS frame.
    // Safe here: bitblt_to_screen already called C3D_FrameEnd, and the
    // next C3D_FrameBegin(SYNCDRAW) will block until GPU completion before
    // we issue any new draws referencing fresh textures.
    for (int i = 0; i < orphan_slot_count; i++) {
        gpu_tex_free_slot(orphan_slots[i]);
    }
    orphan_slot_count = 0;
#endif
}

// Bind shader state once for the world pass. Called from bitblt_to_screen
// when there's geometry to draw.
static void world_setup_state(void)
{
    C3D_BindProgram(&world_program);

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);  // position
#if WORLD_MODE == WORLD_MODE_TEXTURED
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);  // texcoord
#endif

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
#if WORLD_MODE == WORLD_MODE_TEXTURED
    BufInfo_Add(bufInfo, world_vbo, sizeof(world_vertex), 2, 0x10);
#else
    BufInfo_Add(bufInfo, world_vbo, sizeof(world_vertex), 1, 0x0);
#endif

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, world_uLoc_projection, &world_projection);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
#if WORLD_MODE == WORLD_MODE_WIREFRAME
    // Use uniform color from vertex shader
    C3D_FVUnifSet(GPU_VERTEX_SHADER, world_uLoc_color, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
#else
    // Sample texture directly
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
#endif
}

// Flush all accumulated world geometry to the screen.
// Called from bitblt_to_screen after the bitblt quad is drawn (inside frame).
static void world_frame_end(void)
{
    if (world_vbo_count == 0) return;

    world_setup_state();

#if WORLD_MODE == WORLD_MODE_WIREFRAME
    // Single draw call for all line segments
    C3D_DrawArrays(GPU_TRIANGLES, 0, world_vbo_count);
#else
    // One draw call per accumulated batch (one per unique texture run).
    for (int i = 0; i < world_batch_count; i++) {
        world_batch_t* b = &world_batches[i];
        if (b->vert_count == 0) continue;
        C3D_TexBind(0, b->tex);
        C3D_DrawArrays(GPU_TRIANGLES, b->vert_start, b->vert_count);
    }
#endif
}

// =============================================================================
// g3_draw_poly / g3_draw_tmap_tex — the actual hooks Descent calls
// =============================================================================
//
// In WIREFRAME mode: emit polygon edges as line segments.
// In TEXTURED mode: project vertices, bind texture, emit triangles, draw
//                   per-poly (Phase 1; Phase 3 will batch by texture).
// =============================================================================
#define WORLD_MAX_POLY_VERTS 16

#if WORLD_MODE == WORLD_MODE_WIREFRAME

ITCM_CODE void g3_draw_poly(int nv, vms_vector** pointlist)
{
    if (nv < 2) return;
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    float sx[WORLD_MAX_POLY_VERTS], sy[WORLD_MAX_POLY_VERTS];
    bool  visible[WORLD_MAX_POLY_VERTS];

    for (int i = 0; i < nv; i++) {
        visible[i] = world_project(pointlist[i]->x, pointlist[i]->y,
                                   pointlist[i]->z, &sx[i], &sy[i]);
    }

    for (int i = 0; i < nv; i++) {
        int j = (i + 1) % nv;
        if (visible[i] && visible[j]) {
            world_emit_line(sx[i], sy[i], sx[j], sy[j]);
        }
    }
}

ITCM_CODE void g3_draw_tmap_tex(int nv, vms_vector** pointlist,
                                g3s_uvl* uvl_list, grs_bitmap* bm)
{
    (void)uvl_list; (void)bm;
    g3_draw_poly(nv, pointlist);
}

#else // WORLD_MODE_TEXTURED

ITCM_CODE void g3_draw_poly(int nv, vms_vector** pointlist)
{
    // Untextured polys (HUD elements, flat-shaded stuff). Phase 1 ignores
    // these entirely — they'd need a separate "flat colored" path.
    (void)nv; (void)pointlist;
}

// -----------------------------------------------------------------------------
// Debug: log unique missing-texture skips so we can figure out why some
// walls don't render. Set DEBUG_MISSING_TEXTURES to 1 to enable.
// Logs each unique bitmap pointer we reject only once.
// -----------------------------------------------------------------------------
#define DEBUG_MISSING_TEXTURES 1

#if DEBUG_MISSING_TEXTURES
#define MISSING_LOG_MAX 64
static grs_bitmap* missing_logged[MISSING_LOG_MAX];
static int missing_logged_count = 0;

static void log_missing_bitmap(grs_bitmap* bm, const char* reason)
{
    // Already logged this pointer?
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

ITCM_CODE void g3_draw_tmap_tex(int nv, vms_vector** pointlist,
                                g3s_uvl* uvl_list, grs_bitmap* bm)
{
    if (nv < 3 || !bm) return;
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    // Look up the GPU texture for this bitmap. Stages:
    //
    //   1. Composite-key bitmap (from texmerge): the same grs_bitmap* gets
    //      reused for different content as the LRU cache evicts entries.
    //      Track per-bitmap by pointer, re-upload when composite key changes.
    //   2. Fast path: bmp->key is a valid bitmap_index from preload.
    //   3. Hash lookup: matches a preloaded bitmap by bm_data pointer.
    //   4. Dynamic upload: brand-new bitmap we've never seen.
    int idx;
    if (is_composite_key(bm->key)) {
        // Texmerge composite. Track this struct pointer and watch for
        // content changes (which texmerge signals by changing bm->key).
        texmerge_track_t* tt = texmerge_track_get(bm);
        if (tt->slot < 0 || tt->composite_key != bm->key) {
            // First time seeing this bitmap, OR its content changed.
            // DON'T free the old GPU texture now — earlier batches in this
            // frame still reference it. Mark it as orphaned for free at
            // the start of the next frame (when GPU work has completed).
            if (tt->slot >= 0 && orphan_slot_count < ORPHAN_SLOTS_MAX) {
                orphan_slots[orphan_slot_count++] = tt->slot;
            }
            tt->slot = gpu_tex_upload_dynamic(bm);
            if (tt->slot < 0) {
                log_missing_bitmap(bm, "texmerge-upload-failed");
                return;
            }
            tt->composite_key = bm->key;
            // gpu_tex_upload_to_slot patched bm->key to the slot index.
            // Restore the composite key so we can detect the next change.
            bm->key = tt->composite_key;
        }
        idx = tt->slot;
    } else {
        idx = bm->key;
        if (idx < 0 || idx >= GPU_TEX_MAX || !gpu_tex_pool[idx].tex) {
            idx = bm_hash_lookup(bm->bm_data);
            if (idx < 0 || !gpu_tex_pool[idx].tex) {
                idx = gpu_tex_upload_dynamic(bm);
                if (idx < 0) {
                    log_missing_bitmap(bm, "upload-failed");
                    return;
                }
            } else {
                bm->key = idx;  // cache for next time
            }
        }
    }
    gpu_tex_entry_t* gt = &gpu_tex_pool[idx];
    if (!gt->tex) {
        log_missing_bitmap(bm, "no-gpu-tex");
        return;
    }

    // Convert camera-space fixed-point to float. The GPU vertex shader will
    // apply our perspective projection matrix, doing the perspective divide
    // and producing perspective-correct UV interpolation as a side effect.
    float in_cx[WORLD_MAX_POLY_VERTS], in_cy[WORLD_MAX_POLY_VERTS], in_cz[WORLD_MAX_POLY_VERTS];
    float in_u [WORLD_MAX_POLY_VERTS], in_v [WORLD_MAX_POLY_VERTS];

    for (int i = 0; i < nv; i++) {
        in_cx[i] = (float)pointlist[i]->x * (1.0f / 65536.0f);
        in_cy[i] = (float)pointlist[i]->y * (1.0f / 65536.0f);
        in_cz[i] = (float)pointlist[i]->z * (1.0f / 65536.0f);
        // UVs: Descent stores as 16.16 fix where 1.0 = full texture.
        // Scale by u_scale/v_scale to handle non-POT padding.
        // Flip V — Descent's V origin is opposite from PICA200's.
        in_u[i] = (float)uvl_list[i].u * (1.0f / 65536.0f) * gt->u_scale;
        in_v[i] = gt->v_scale - (float)uvl_list[i].v * (1.0f / 65536.0f) * gt->v_scale;
    }

    // -------------------------------------------------------------------------
    // Near-plane clipping (Sutherland-Hodgman).
    // For each edge (curr -> next):
    //   - If both vertices are in front of the near plane, keep `next`.
    //   - If only `curr` is in front, emit the edge/plane intersection.
    //   - If only `next` is in front, emit intersection then `next`.
    //   - If both are behind, drop both.
    // Output may have up to nv+1 vertices (each behind-to-front transition
    // adds one). We size the output buffer accordingly.
    // -------------------------------------------------------------------------
    #define CLIP_MAX_OUT (WORLD_MAX_POLY_VERTS + 2)
    float cx[CLIP_MAX_OUT], cy[CLIP_MAX_OUT], cz[CLIP_MAX_OUT];
    float u [CLIP_MAX_OUT], v [CLIP_MAX_OUT];
    int out_nv = 0;

    for (int i = 0; i < nv; i++) {
        int j = (i + 1) % nv;
        bool curr_in = in_cz[i] >= WIRE_NEAR;
        bool next_in = in_cz[j] >= WIRE_NEAR;

        if (curr_in) {
            // Always keep current vertex if it's in front
            cx[out_nv] = in_cx[i]; cy[out_nv] = in_cy[i]; cz[out_nv] = in_cz[i];
            u[out_nv]  = in_u[i];  v[out_nv]  = in_v[i];
            out_nv++;
        }
        if (curr_in != next_in) {
            // Edge crosses the near plane — compute intersection
            // Parametric form: P(t) = curr + t * (next - curr), 0 <= t <= 1
            // Solve for t where P.z = WIRE_NEAR:
            //   curr.z + t * (next.z - curr.z) = WIRE_NEAR
            //   t = (WIRE_NEAR - curr.z) / (next.z - curr.z)
            float t = (WIRE_NEAR - in_cz[i]) / (in_cz[j] - in_cz[i]);
            cx[out_nv] = in_cx[i] + t * (in_cx[j] - in_cx[i]);
            cy[out_nv] = in_cy[i] + t * (in_cy[j] - in_cy[i]);
            cz[out_nv] = WIRE_NEAR;
            u[out_nv]  = in_u[i]  + t * (in_u[j]  - in_u[i]);
            v[out_nv]  = in_v[i]  + t * (in_v[j]  - in_v[i]);
            out_nv++;
        }
    }

    // If clipping eliminated everything, skip
    if (out_nv < 3) return;

    int needed = (out_nv - 2) * 3;
    if (world_vbo_count + needed > WIRE_MAX_VERTS) return;

    // Get/create a batch for this texture. If the previous draw used the
    // same texture, extend that batch (zero-cost coalescing). Otherwise
    // start a new batch.
    world_batch_t* batch;
    if (world_last_tex == gt->tex && world_batch_count > 0) {
        batch = &world_batches[world_batch_count - 1];
    } else {
        if (world_batch_count >= WORLD_MAX_BATCHES) return;
        batch = &world_batches[world_batch_count++];
        batch->tex = gt->tex;
        batch->vert_start = world_vbo_count;
        batch->vert_count = 0;
        world_last_tex = gt->tex;
    }

    // Fan triangulation of the clipped polygon
    for (int i = 1; i < out_nv - 1; i++) {
        world_emit_vert(cx[0],   cy[0],   cz[0],   u[0],   v[0]);
        world_emit_vert(cx[i],   cy[i],   cz[i],   u[i],   v[i]);
        world_emit_vert(cx[i+1], cy[i+1], cz[i+1], u[i+1], v[i+1]);
    }
    batch->vert_count += needed;
}

#endif // WORLD_MODE

// =============================================================================
// g3_draw_tmap_flat / g3_draw_bitmap
// =============================================================================
//
// g3_draw_tmap_flat is the software renderer's "flat-shaded textured polygon"
// path — same vertex format as tmap_tex but the renderer ignores u,v and
// just fills with bm->avg_color * lighting. It's used for distant walls (a
// software perf optimization) and for special effects like cloaked enemies.
//
// On a GPU there's no perf reason to flat-shade, and the polygons look
// better with their actual texture. So we route flat -> tex in TEXTURED
// mode. Tradeoff: cloaked enemies will look textured rather than shimmery.
// We can revisit with a real flat path later if we want the effect back.
//
// In WIREFRAME mode, both still route through g3_draw_poly so outlines
// continue to draw.
ITCM_CODE void g3_draw_tmap_flat(int nv, vms_vector** pointlist,
                                 g3s_uvl* uvl_list, grs_bitmap* bm)
{
#if WORLD_MODE == WORLD_MODE_TEXTURED
    g3_draw_tmap_tex(nv, pointlist, uvl_list, bm);
#else
    (void)uvl_list; (void)bm;
    g3_draw_poly(nv, pointlist);
#endif
}

ITCM_CODE void g3_draw_bitmap(vms_vector* pos, fix width, fix height, grs_bitmap* bm) {
    (void)pos; (void)width; (void)height; (void)bm;
}

// =============================================================================
// TRIANGLE MODE — simplest possible GPU sanity test
// =============================================================================
#if RENDER_MODE == RENDER_MODE_TRIANGLE

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
// =============================================================================
#else

typedef struct { float x, y, z; float u, v; } tex_vertex;

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
static void*   tex_buf;

static void tex_upload_software(C3D_Tex* tex,
                                const u32* src_linear, int src_stride,
                                int src_w, int src_h, int tex_w)
{
    u32* dst = (u32*)tex->data;
    int tiles_per_row = tex_w / 8;

    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int px = x & 7, py = y & 7;
            int z = (px & 1)        | ((py & 1) << 1) |
                    ((px & 2) << 1) | ((py & 2) << 2) |
                    ((px & 4) << 2) | ((py & 4) << 3);
            int tile_idx = (y / 8) * tiles_per_row + (x / 8);
            dst[tile_idx * 64 + z] = src_linear[y * src_stride + x];
        }
    }
    C3D_TexFlush(tex);
}

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

    tex_buf = linearAlloc(TEX_W * TEX_H * 4);
    memset(tex_buf, 0, TEX_W * TEX_H * 4);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

void bitblt_to_screen(void)
{
    hidScanInput();
    keyboard_handler();

#if RENDER_MODE == RENDER_MODE_BITBLT
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
    u32* raw = (u32*)back_tex.data;
    for (int i = 0; i < TEX_W * TEX_H; i++) raw[i] = 0xFF0000FF;
    C3D_TexFlush(&back_tex);
#endif

#if RENDER_MODE != RENDER_MODE_RAW_WRITE
    tex_upload_software(&back_tex, (u32*)tex_buf, TEX_W, GAME_W, UPLOAD_H, TEX_W);
#endif

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

    // Draw world geometry on top (textured polys or wireframes)
    world_frame_end();

    C3D_FrameEnd(0);

    // Reset world buffer for next frame
    world_frame_begin();
}

#endif // RENDER_MODE branch

// =============================================================================
// GPU bring-up — called once at startup
// =============================================================================
void init_3ds_gpu(void)
{
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    target = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W,
                                    GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    sceneInit();
    world_init();

    gpu_inited = true;

    // If piggy already loaded bitmaps before init_3ds_gpu was called, do
    // the initial preload here. Subsequent level changes will trigger
    // init_nds_textures() which calls preload again.
    extern int Num_bitmap_files;
    if (Num_bitmap_files > 0) {
        gpu_tex_preload_all();
    }
}

// =============================================================================
// Debug modes reference
// =============================================================================
// RENDER_MODE controls the bitblt quad path:
//   BITBLT        — Normal palette -> RGBA -> tiled texture -> quad
//   TRIANGLE      — Minimal GPU test (requires position+color shader)
//   SOLID         — Solid red quad (tests texture upload in isolation)
//   CHECKERBOARD  — Red/white checker (makes tiling bugs visible)
//   RAW_WRITE     — Bypass tiler, write tex->data directly
//
// WORLD_MODE controls native 3D world rendering on top of the bitblt:
//   TEXTURED      — Full textured polygons (Phase 1: no lighting yet)
//   WIREFRAME     — Polygon outlines (debug view)
// =============================================================================
