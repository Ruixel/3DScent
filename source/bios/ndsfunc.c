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

#include "3ds/gpu/enums.h"
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
static C3D_RenderTarget* target_left;
static C3D_RenderTarget* target_right;
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
//#define CLEAR_COLOR  0x68B0D8FF
#define CLEAR_COLOR  0x000000FF

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

static int poly_count = 0;
static int frame_preloaded_hits = 0;     // bm->key was a valid preloaded slot
static int frame_hash_hits = 0;          // resolved via bm_hash_lookup
static int frame_dynamic_uploads = 0;    // brand-new bitmap uploaded
static int frame_composite_uploads = 0;  // texmerge composite uploaded/re-uploaded
static int frame_composite_reuses = 0;   // composite tracked, key matched, no upload
static int frame_lookup_failures = 0;    // lookup ended in log_missing_bitmap

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

    printf("Loaded: %d/%d\n", gpu_tex_loaded_count, Num_bitmap_files);
    printf("VRAM free: %u KB\n", vramSpaceFree() / 1024);
    printf("Linear free: %u KB\n", linearSpaceFree() / 1024);
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
// position (3) + texcoord (2) + color RGB (3) = 8 floats = 32 bytes per vertex
typedef struct { float x, y, z, u, v, light; u32 color; } world_vertex;
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
#define WORLD_MAX_BATCHES  2561  // unique textures per frame
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
static inline void world_emit_vert(float x, float y, float z, float u, float v, float light, u32 color)
{
    if (world_vbo_count >= WIRE_MAX_VERTS) return;
    world_vertex* p = &world_vbo[world_vbo_count++];
    p->x = x; p->y = y; p->z = z;
    p->u = u; p->v = v;
    p->light = light;
    p->color = color;
}
#endif

#if WORLD_MODE == WORLD_MODE_TEXTURED
// Build the world-space perspective projection matrix for a given eye IOD.
// iod=0 builds a mono (no offset) matrix. Negative iod = left eye, positive
// = right eye. Result is stored in world_projection (consumed by
// world_setup_state via C3D_FVUnifMtx4x4).
//
// The full matrix is: viewport_remap * camera_x_translate * perspective_tilt
// where camera_x_translate shifts the camera horizontally in camera-space
// to produce stereo parallax. This is equivalent to (but more controllable
// than) Mtx_PerspStereoTilt — and avoids potential ordering issues with
// the post-tilt viewport-remap matrix.
//
// stereo_screen_dist controls the convergence plane — objects at this
// camera-space distance appear AT screen depth. Closer objects pop out
// (toward viewer), farther objects recede behind the screen. Tuned for
// Descent's typical 5-30 unit room scale.
static const float stereo_screen_dist = 8.0f;

static void world_build_projection(float iod)
{
    float fov_v_rad = WIRE_FOV_V_DEG * (float)M_PI / 180.0f;
    float fov_h_rad = WIRE_FOV_H_DEG * (float)M_PI / 180.0f;
    float effective_aspect = tanf(fov_h_rad * 0.5f) / tanf(fov_v_rad * 0.5f);

    // Standard perspective+tilt — same for both eyes.
    Mtx_PerspTilt(&world_projection, fov_v_rad, effective_aspect,
                  WIRE_NEAR, WIRE_FAR, true);

    // Apply stereo by translating the camera horizontally and shearing.
    // We shear X by -iod * (Z - screen_dist) / screen_dist so that:
    //   - At Z = screen_dist: no shift (convergence plane is at screen depth)
    //   - Far objects: shifted in one direction
    //   - Near objects: shifted in opposite direction
    // This is what PerspStereoTilt does internally, but applied in
    // camera-space pre-perspective — avoiding any post-tilt ordering issues.
    if (iod != 0.0f) {
        C3D_Mtx shear;
        Mtx_Identity(&shear);
        // shear X by Z: X_new = X + (iod / screen_dist) * Z - iod
        // In matrix form (column vector): row 0: [1, 0, iod/screen_dist, -iod]
        shear.r[0].z = iod / stereo_screen_dist;
        shear.r[0].w = -iod;

        C3D_Mtx tmp;
        Mtx_Multiply(&tmp, &world_projection, &shear);
        world_projection = tmp;
    }

    // Post-multiply by viewport-remap to confine output to the QUAD region.
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
#endif

static void world_frame_begin(void)
{
    // Wait for GPU to finish processing the previous frame's draws BEFORE
    // we start overwriting world_vbo. Without this:
    //   1. We submit draws referencing world_vbo at frame N
    //   2. C3D_FrameEnd returns immediately (GPU still processing)
    //   3. Descent's render walk happens, OVERWRITES world_vbo for frame N+1
    //   4. GPU is still reading frame N's data — now corrupted
    // The race is much wider in stereo because right-eye draws are submitted
    // later, leaving more pending GPU work to overlap with CPU writes.
    // SYNCDRAW at FrameBegin handles this for state set there, but world_vbo
    // gets written by Descent BEFORE FrameBegin, so it's outside that wait.
    gspWaitForP3D();

    world_vbo_count = 0;
#if WORLD_MODE == WORLD_MODE_TEXTURED
    world_batch_count = 0;
    world_last_tex = NULL;

    // Free any GPU textures orphaned during the PREVIOUS frame.
    // Safe here: gspWaitForP3D above guaranteed GPU completion.
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
    AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 1);  // light
    AttrInfo_AddLoader(attrInfo, 3, GPU_UNSIGNED_BYTE, 4);  // color (RGB)
#endif

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
#if WORLD_MODE == WORLD_MODE_TEXTURED
    //BufInfo_Add(bufInfo, world_vbo, sizeof(world_vertex), 3, 0x210);  // 3 attrs: pos@0, tex@1, color@2
    BufInfo_Add(bufInfo, world_vbo, sizeof(world_vertex), 4, 0x3210);  // 4 attrs: pos@0, tex@1, light@2, color@3
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
    // Modulate texture by per-vertex color (lighting).
    // GPU_PRIMARY_COLOR is the interpolated vertex color (Gouraud).
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
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
    printf("Drawing %d world batches, %d verts\n", world_batch_count, world_vbo_count);
int unique_tex = 0;
C3D_Tex* seen[WORLD_MAX_BATCHES];
for (int i = 0; i < world_batch_count; i++) {
    bool found = false;
    for (int j = 0; j < unique_tex; j++)
        if (seen[j] == world_batches[i].tex) { found = true; break; }
    if (!found) seen[unique_tex++] = world_batches[i].tex;
}
printf("unique textures: %d\n", unique_tex);
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

// -----------------------------------------------------------------------------
// Flat-shaded polygon rendering.
//
// OP_FLATPOLY in interp.c passes the color via gr_setcolor(w(p+28)), which
// writes to the current canvas's cv_color. Rather than depend on that
// global, we expose a new entry point g3_draw_poly_flat_color() that takes
// the color as an argument. OP_FLATPOLY is patched to call this directly.
//
// Used for: laser bolts (polygon models with flat-shaded geometry), some
// weapon trails, missile particle effects, occasional HUD elements.
//
// Implementation: emit a triangle fan via the normal world batch path,
// using a shared 1x1 white texture. Vertex color = palette[color_idx],
// so (white texture) × (vertex color) = palette color. Gets us free
// near-plane clipping + stereo + batching through existing pipeline.
// -----------------------------------------------------------------------------
static C3D_Tex white_tex;
static bool    white_tex_ready = false;

extern ubyte gr_palette[];

static void white_tex_init(void)
{
    if (white_tex_ready) return;
    // 8x8 minimum for the tiler (which operates on 8x8 tiles)
    if (!C3D_TexInit(&white_tex, 8, 8, GPU_RGBA5551)) return;
    C3D_TexSetFilter(&white_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&white_tex, GPU_REPEAT, GPU_REPEAT);

    u16 src[64];
    for (int i = 0; i < 64; i++) src[i] = 0xFFFF;  // white, alpha=1
    tex_upload_5551(&white_tex, src, 8, 8, 8);
    white_tex_ready = true;
}

// Public entry point: flat-shaded polygon with explicit color.
ITCM_CODE void g3_draw_poly_flat_color(int nv, vms_vector** pointlist, int color_idx)
{
    if (nv < 3) return;
    if (!white_tex_ready) white_tex_init();
    if (!white_tex_ready) return;

    poly_count++;

    // Convert palette index to RGB (6-bit → normalized float)
    float r = (float)gr_palette[color_idx * 3 + 0] * (1.0f / 63.0f);
    float g = (float)gr_palette[color_idx * 3 + 1] * (1.0f / 63.0f);
    float b = (float)gr_palette[color_idx * 3 + 2] * (1.0f / 63.0f);
    //u32 color = ((u32)(b * 255) << 24) | ((u32)(g * 255) << 16) | ((u32)(r * 255) << 8) | 0xFF;
    u32 color = 0xFF000000 | ((u32)(b * 255) << 16) | ((u32)(g * 255) << 8) | (u32)(r * 255);

    // Near-plane clipping via Sutherland-Hodgman (same approach as tmap_tex)
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    float in_cx[WORLD_MAX_POLY_VERTS], in_cy[WORLD_MAX_POLY_VERTS], in_cz[WORLD_MAX_POLY_VERTS];
    for (int i = 0; i < nv; i++) {
        in_cx[i] = (float)pointlist[i]->x * (1.0f / 65536.0f);
        in_cy[i] = (float)pointlist[i]->y * (1.0f / 65536.0f);
        in_cz[i] = (float)pointlist[i]->z * (1.0f / 65536.0f);
    }

    #define FLAT_CLIP_MAX (WORLD_MAX_POLY_VERTS + 2)
    float cx[FLAT_CLIP_MAX], cy[FLAT_CLIP_MAX], cz[FLAT_CLIP_MAX];
    int out_nv = 0;

    for (int i = 0; i < nv; i++) {
        int j = (i + 1) % nv;
        bool curr_in = in_cz[i] >= WIRE_NEAR;
        bool next_in = in_cz[j] >= WIRE_NEAR;

        if (curr_in) {
            cx[out_nv] = in_cx[i]; cy[out_nv] = in_cy[i]; cz[out_nv] = in_cz[i];
            out_nv++;
        }
        if (curr_in != next_in) {
            float t = (WIRE_NEAR - in_cz[i]) / (in_cz[j] - in_cz[i]);
            cx[out_nv] = in_cx[i] + t * (in_cx[j] - in_cx[i]);
            cy[out_nv] = in_cy[i] + t * (in_cy[j] - in_cy[i]);
            cz[out_nv] = WIRE_NEAR;
            out_nv++;
        }
    }

    if (out_nv < 3) return;

    int needed = (out_nv - 2) * 3;
    if (world_vbo_count + needed > WIRE_MAX_VERTS) return;

    // Get/create batch for the white texture
    world_batch_t* batch;
    if (world_last_tex == &white_tex && world_batch_count > 0) {
        batch = &world_batches[world_batch_count - 1];
    } else {
        if (world_batch_count >= WORLD_MAX_BATCHES) return;
        batch = &world_batches[world_batch_count++];
        batch->tex = &white_tex;
        batch->vert_start = world_vbo_count;
        batch->vert_count = 0;
        world_last_tex = &white_tex;
    }

    // Fan triangulate; UV 0.5 samples middle of white texture (always white)
    for (int i = 1; i < out_nv - 1; i++) {
      world_emit_vert(cx[0],   cy[0],   cz[0],   0.5f, 0.5f, 1.0f, color);
      world_emit_vert(cx[i],   cy[i],   cz[i],   0.5f, 0.5f, 1.0f, color);
      world_emit_vert(cx[i+1], cy[i+1], cz[i+1], 0.5f, 0.5f, 1.0f, color);
    }
    batch->vert_count += needed;
}

// Backwards-compat: legacy g3_draw_poly with no color. Used by a few paths
// that set color via gr_setcolor. For now, default to white since we
// haven't wired up reading the current canvas color.
ITCM_CODE void g3_draw_poly(int nv, vms_vector** pointlist)
{
    g3_draw_poly_flat_color(nv, pointlist, 15);  // 15 = light gray in Descent palette
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

// -----------------------------------------------------------------------------
// Per-vertex lighting via fade table + palette
//
// Descent's lighting model: each vertex has a light value `l` in fix-point,
// roughly [0, MAX_LIGHT]. The software renderer uses gr_fade_table[] (34
// brightness levels x 256 palette entries) to remap palette indices to
// dimmer ones based on light level.
//
// We can't do that exact technique on PICA200 (would need dependent texture
// reads). Instead: per-vertex, look up "what does WHITE look like at this
// light level" and use that as an RGB multiplier on the texture sample.
// Result: smooth Gouraud-shaded lighting that captures Descent's color shift
// at low light (since the fade table also cools/desaturates as it dims).
//
// Constants:
//   gr_fade_table layout: 34 rows (light levels) x 256 entries (palette)
//   gr_current_pal layout: 256 entries x 3 bytes (R, G, B), 6-bit (0..63)
//   MAX_LIGHT: f1_0 = 0x10000 = max input light value
// -----------------------------------------------------------------------------
extern ubyte gr_fade_table[];
extern ubyte gr_current_pal[];
extern ubyte gr_palette[];

#define LIGHT_FADE_LEVELS  34
#define LIGHT_WHITE_INDEX  255   // standard Descent: palette index 255 = white

static void light_to_rgb(fix l, float* out_r, float* out_g, float* out_b)
{
    // Simple linear brightness. We tried the full fade-table + palette
    // approach but the palette in use is essentially grayscale, so the
    // color shift of the fade table wasn't visible — not worth the
    // extra CPU work. Linear brightness matches Descent's atmosphere
    // closely enough on a 3DS screen.
    float brightness = (float)l * (1.0f / 65536.0f);
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    *out_r = brightness;
    *out_g = brightness;
    *out_b = brightness;
}

u64 tmap_ticks = 0;
u64 t_lookup = 0, t_convert = 0, t_clip = 0, t_emit = 0;
ITCM_CODE void g3_draw_tmap_tex(int nv, vms_vector** pointlist,
                                g3s_uvl* uvl_list, grs_bitmap* bm)
{
    u64 t0 = svcGetSystemTick();
    u64 t1 = t0;
    if (nv < 3 || !bm) return;
    if (nv > WORLD_MAX_POLY_VERTS) nv = WORLD_MAX_POLY_VERTS;

    poly_count++;

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
                frame_lookup_failures++;
                log_missing_bitmap(bm, "texmerge-upload-failed");
                return;
            }
            tt->composite_key = bm->key;
            // gpu_tex_upload_to_slot patched bm->key to the slot index.
            // Restore the composite key so we can detect the next change.
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
                bm->key = idx;  // cache for next time
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

    t_lookup += svcGetSystemTick() - t0;

    // Convert camera-space fixed-point to float. The GPU vertex shader will
    // apply our perspective projection matrix, doing the perspective divide
    // and producing perspective-correct UV interpolation as a side effect.
    t0 = svcGetSystemTick();
    float in_cx[WORLD_MAX_POLY_VERTS], in_cy[WORLD_MAX_POLY_VERTS], in_cz[WORLD_MAX_POLY_VERTS];
    float in_u [WORLD_MAX_POLY_VERTS], in_v [WORLD_MAX_POLY_VERTS];
    float in_light[WORLD_MAX_POLY_VERTS];

    for (int i = 0; i < nv; i++) {
        in_cx[i] = (float)pointlist[i]->x * (1.0f / 65536.0f);
        in_cy[i] = (float)pointlist[i]->y * (1.0f / 65536.0f);
        in_cz[i] = (float)pointlist[i]->z * (1.0f / 65536.0f);
        in_u[i] = (float)uvl_list[i].u * (1.0f / 65536.0f) * gt->u_scale;
        in_v[i] = gt->v_scale - (float)uvl_list[i].v * (1.0f / 65536.0f) * gt->v_scale;
        in_light[i] = (float)uvl_list[i].l * (1.0f / 65536.0f);
        if (in_light[i] < 0.0f) in_light[i] = 0.0f;
        if (in_light[i] > 1.0f) in_light[i] = 1.0f;
    }
    t_convert += svcGetSystemTick() - t0;

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
    t0 = svcGetSystemTick();
    /*
    #define CLIP_MAX_OUT (WORLD_MAX_POLY_VERTS + 2)
    float *use_cx, *use_cy, *use_cz;
    float *use_u,  *use_v;
    float *use_light;
    int out_nv;

    bool needs_clip = false;
    for (int i = 0; i < nv; i++) {
        if (in_cz[i] < WIRE_NEAR) { needs_clip = true; break; }
    }

    if (!needs_clip) {
        use_cx    = in_cx;    use_cy    = in_cy;    use_cz    = in_cz;
        use_u     = in_u;     use_v     = in_v;
        use_light = in_light;
        out_nv = nv;
    } else {
        static float cx[CLIP_MAX_OUT], cy[CLIP_MAX_OUT], cz[CLIP_MAX_OUT];
        static float cu[CLIP_MAX_OUT], cv[CLIP_MAX_OUT];
        static float cl[CLIP_MAX_OUT];
        out_nv = 0;

        for (int i = 0; i < nv; i++) {
            int j = (i + 1) % nv;
            bool curr_in = in_cz[i] >= WIRE_NEAR;
            bool next_in = in_cz[j] >= WIRE_NEAR;

            if (curr_in) {
                cx[out_nv] = in_cx[i]; cy[out_nv] = in_cy[i]; cz[out_nv] = in_cz[i];
                cu[out_nv] = in_u[i];  cv[out_nv] = in_v[i];
                cl[out_nv] = in_light[i];
                out_nv++;
            }
            if (curr_in != next_in) {
                float t = (WIRE_NEAR - in_cz[i]) / (in_cz[j] - in_cz[i]);
                cx[out_nv] = in_cx[i] + t * (in_cx[j] - in_cx[i]);
                cy[out_nv] = in_cy[i] + t * (in_cy[j] - in_cy[i]);
                cz[out_nv] = WIRE_NEAR;
                cu[out_nv] = in_u[i]  + t * (in_u[j]  - in_u[i]);
                cv[out_nv] = in_v[i]  + t * (in_v[j]  - in_v[i]);
                cl[out_nv] = in_light[i] + t * (in_light[j] - in_light[i]);
                out_nv++;
            }
        }

        use_cx    = cx; use_cy    = cy; use_cz    = cz;
        use_u     = cu; use_v     = cv;
        use_light = cl;
    }

    // If clipping eliminated everything, skip
    if (out_nv < 3) return;

    int needed = (out_nv - 2) * 3;
    if (world_vbo_count + needed > WIRE_MAX_VERTS) return;
      */

    //testing
  
    float *use_cx, *use_cy, *use_cz;
    float *use_u,  *use_v;
    float *use_light;
    int out_nv;
    use_cx    = in_cx;    use_cy    = in_cy;    use_cz    = in_cz;
    use_u     = in_u;     use_v     = in_v;
    use_light = in_light;
    out_nv = nv;
    int needed = (out_nv - 2) * 3;

    // Get/create batch
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
    t_clip += svcGetSystemTick() - t0;

    // Fan triangulation
    t0 = svcGetSystemTick();
    for (int i = 1; i < out_nv - 1; i++) {
      world_emit_vert(use_cx[0],   use_cy[0],   use_cz[0],   use_u[0],   use_v[0],   use_light[0],   0xFFFFFFFF);
      world_emit_vert(use_cx[i],   use_cy[i],   use_cz[i],   use_u[i],   use_v[i],   use_light[i],   0xFFFFFFFF);
      world_emit_vert(use_cx[i+1], use_cy[i+1], use_cz[i+1], use_u[i+1], use_v[i+1], use_light[i+1], 0xFFFFFFFF);
    }
    batch->vert_count += needed;

    t_emit += svcGetSystemTick() - t0;
    tmap_ticks += svcGetSystemTick() - t1;
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

// =============================================================================
// Sprite rendering: g3_draw_bitmap (billboards)
// =============================================================================
//
// Billboards: 2D bitmaps that always face the camera. Descent passes
// world-space position + radius; we transform to camera space and emit
// a quad centered on the transformed position, aligned with camera axes.
//
// Reuses g3_draw_tmap_tex for actual rendering, which gets us proper
// near-plane clipping, texture lookup, batching, and light modulation
// for free. The only cost is converting our float coords back to
// fix-point vms_vectors.
//
// Note: g3_draw_rod_tmap is defined in rod.c (forwards to g3_draw_tmap_tex)
// so we don't implement it here.
// -----------------------------------------------------------------------------
#define MAX_LIGHT 0x10000  // fix-point 1.0 — used as "full bright" for sprites

extern vms_vector View_position;
extern vms_matrix View_matrix;

// Transform a world-space point to camera space (float). Same math as
// g3_rotate_point: subtract camera position, rotate by View_matrix.
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

// Emit a 4-vert textured quad (as 2 triangles = 6 verts) into the world
// batch system. Camera-space positions and UVs, plus a single light value
// applied to all 4 corners.
//
// Routes through g3_draw_tmap_tex by constructing fake pointlist/uvl_list.
// That path does near-plane clipping, perspective setup, and batching — we
// get it all for free. The only cost is we need to convert our float
// camera-space coords back to fix-point vms_vectors. Small CPU cost,
// saves us from duplicating the clipping math.
// Emit a 4-vert textured quad directly into the world batch system.
// Takes UVs in [0, 1] over the VALID (non-padded) image region. Applies
// u_scale/v_scale to get the actual GPU texture coords, WITHOUT the V-flip
// that tmap_tex does for walls. Sprite bitmaps have their own convention
// and get their own path.
//
// This bypasses tmap_tex's near-plane clipping. We handle it manually here.
static void emit_sprite_quad(grs_bitmap* bm,
                             float x0, float y0, float z0, float u0, float v0,
                             float x1, float y1, float z1, float u1, float v1,
                             float x2, float y2, float z2, float u2, float v2,
                             float x3, float y3, float z3, float u3, float v3,
                             fix light)
{
    if (!bm) return;

    // Find GPU texture (same lookup chain as tmap_tex uses)
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

    // Scale input UVs [0, 1] to GPU texture coords. NO V flip (sprite has
    // its own convention, unlike walls).
    //
    // NOTE: u_scale/v_scale handling for non-POT textures appears to be
    // wrong somewhere — when we apply them, sprites get cropped. When we
    // skip them (sample [0, 1]), sprites render fully without cropping.
    // This suggests the GPU is already sampling [0, 1] over the bitmap
    // dimensions, not the POT texture dimensions. Possibly C3D_TexInit
    // configures sampling bounds based on a parameter we're missing.
    //
    // For now: pass UVs through without scaling. Walls still get scaling
    // via tmap_tex's separate path.
    float su0 = u0, sv0 = v0;
    float su1 = u1, sv1 = v1;
    float su2 = u2, sv2 = v2;
    float su3 = u3, sv3 = v3;

    // Compute lighting RGB
    float brightness = (float)light * (1.0f / 65536.0f);
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;

    // Near-plane clipping for 4-vertex polygon
    float in_cx[4] = {x0, x1, x2, x3};
    float in_cy[4] = {y0, y1, y2, y3};
    float in_cz[4] = {z0, z1, z2, z3};
    float in_u [4] = {su0, su1, su2, su3};
    float in_v [4] = {sv0, sv1, sv2, sv3};
    float in_light[4] = {brightness, brightness, brightness, brightness};
    float cl[6];

    float cx[6], cy[6], cz[6], u[6], v[6];
    int out_nv = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        bool curr_in = in_cz[i] >= WIRE_NEAR;
        bool next_in = in_cz[j] >= WIRE_NEAR;
        if (curr_in) {
            cx[out_nv] = in_cx[i]; cy[out_nv] = in_cy[i]; cz[out_nv] = in_cz[i];
            u[out_nv]  = in_u[i];  v[out_nv]  = in_v[i];  cl[out_nv] = in_light[i];
            out_nv++;
        }
        if (curr_in != next_in) {
            float t = (WIRE_NEAR - in_cz[i]) / (in_cz[j] - in_cz[i]);
            cx[out_nv] = in_cx[i] + t * (in_cx[j] - in_cx[i]);
            cy[out_nv] = in_cy[i] + t * (in_cy[j] - in_cy[i]);
            cz[out_nv] = WIRE_NEAR;
            u[out_nv]  = in_u[i]  + t * (in_u[j]  - in_u[i]);
            v[out_nv]  = in_v[i]  + t * (in_v[j]  - in_v[i]);
            cl[out_nv] = in_light[i] + t * (in_light[j] - in_light[i]);
            out_nv++;
        }
    }
    if (out_nv < 3) return;

    int needed = (out_nv - 2) * 3;
    if (world_vbo_count + needed > WIRE_MAX_VERTS) return;

    // Get/create batch
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

    // Fan triangulate
    for (int i = 1; i < out_nv - 1; i++) {
      world_emit_vert(cx[0],   cy[0],   cz[0],   u[0],   v[0],   cl[0], 0xFFFFFFFF);
      world_emit_vert(cx[i],   cy[i],   cz[i],   u[i],   v[i],   cl[i], 0xFFFFFFFF);
      world_emit_vert(cx[i+1], cy[i+1], cz[i+1], u[i+1], v[i+1], cl[i+1], 0xFFFFFFFF);
    }
    batch->vert_count += needed;
}

// Billboard sprite: a 2D bitmap at world position `pos`, always facing the
// camera, with given world-space width x height. Used for explosions,
// powerups, and some effects.
ITCM_CODE void g3_draw_bitmap(vms_vector* pos, fix width, fix height, grs_bitmap* bm)
{
    if (!bm) return;

    // Transform center to camera space
    float cx, cy, cz;
    world_to_camera(pos, &cx, &cy, &cz);

    // Build billboard axes in CAMERA SPACE. The sprite faces the camera
    // (its plane is perpendicular to the view direction), which means in
    // camera space it's aligned with the XY plane. So right = +X, up = +Y.
    //
    // Descent passes `width`/`height` as the object radius (half-extent),
    // NOT the full extent. So no *0.5 here.
    //
    // Height compensation: for non-POT bitmaps, the rendered sprite ends
    // up vertically compressed for reasons not fully understood (likely
    // related to how PICA200 normalizes texture coords vs how the GPU
    // calculates the perspective projection). Dividing world-space height
    // by v_scale compensates and matches the original PC Descent look
    // closely enough. Not mathematically pure but ships a result that's
    // visually correct.
    //
    // We need gt for the v_scale value, so do the texture lookup here too.
    // (emit_sprite_quad does its own lookup; this is a small duplication
    // but keeps the code straightforward.)
    int idx2 = (bm->key >= 0 && bm->key < GPU_TEX_MAX) ? bm->key : 0;
    gpu_tex_entry_t* gt2 = &gpu_tex_pool[idx2];
    float v_compensate = (gt2->tex && gt2->v_scale > 0.0f) ? (1.0f / gt2->v_scale) : 1.0f;

    float hw = (float)width  * (1.0f / 65536.0f);
    float hh = (float)height * (1.0f / 65536.0f) * v_compensate;

    // UV mapping for sprites — based on the working diagnostic from user:
    //   u_lo=0,            u_hi=1.0/u_scale  -> effective sampling [0, 1] in POT
    //   v_lo=1.0/v_scale,  v_hi=0            -> V is inverted in PICA200's
    //                                            sampling convention
    //
    // After removing u_scale/v_scale from emit_sprite_quad, we just send
    // [0, 1] UVs directly. V is inverted to match the user's working fix:
    // top of sprite needs V=1 to sample top of image.
    float u_lo = 0.0f, u_hi = 1.0f;
    float v_lo = 1.0f, v_hi = 0.0f;

    // Corner layout in camera space (+Y is UP):
    emit_sprite_quad(bm,
                     cx - hw, cy + hh, cz,  u_lo, v_lo,   // top-left
                     cx + hw, cy + hh, cz,  u_hi, v_lo,   // top-right
                     cx + hw, cy - hh, cz,  u_hi, v_hi,   // bottom-right
                     cx - hw, cy - hh, cz,  u_lo, v_hi,   // bottom-left
                     MAX_LIGHT);
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

    C3D_RenderTargetClear(target_left, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target_left);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_DrawArrays(GPU_TRIANGLES, 0, triangle_vert_count);

    if (osGet3DSliderState() > 0.0f) {
        C3D_RenderTargetClear(target_right, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target_right);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
        C3D_DrawArrays(GPU_TRIANGLES, 0, triangle_vert_count);
    }

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

static u8 swizzle_lut[64];  // maps py*8+px -> z
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

static void tex_upload_software(C3D_Tex* tex,
                                const u32* src_linear, int src_stride,
                                int src_w, int src_h, int tex_w)
{
    if (!swizzle_lut_init) init_swizzle_lut();

    u32* dst = (u32*)tex->data;
    int tiles_per_row = tex_w / 8;

    // Iterate in tile order — better cache behavior on dst
    int tiles_y = src_h / 8;
    int tiles_x = src_w / 8;

    for (int ty = 0; ty < tiles_y; ty++) {
        for (int tx = 0; tx < tiles_x; tx++) {
            u32* tile_dst = dst + (ty * tiles_per_row + tx) * 64;
            const u32* tile_src = src_linear + (ty * 8) * src_stride + (tx * 8);

            // Unroll the 8×8 tile
            for (int py = 0; py < 8; py++) {
                const u32* row_src = tile_src + py * src_stride;
                const u8* lut_row = &swizzle_lut[py * 8];
                tile_dst[lut_row[0]] = row_src[0];
                tile_dst[lut_row[1]] = row_src[1];
                tile_dst[lut_row[2]] = row_src[2];
                tile_dst[lut_row[3]] = row_src[3];
                tile_dst[lut_row[4]] = row_src[4];
                tile_dst[lut_row[5]] = row_src[5];
                tile_dst[lut_row[6]] = row_src[6];
                tile_dst[lut_row[7]] = row_src[7];
            }
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
    // Enable depth test + write. Needed for polygon models (robots, reactor)
    // so their faces occlude each other correctly.
    //
    // GPU_GEQUAL: PICA200 uses reversed-Z convention where near=1, far=0
    // in the depth buffer. "nearer Z wins" = greater-or-equal passes.
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    // Enable alpha test: discard pixels with alpha = 0. Critical for sprites:
    // their transparent edges would otherwise WRITE depth, causing geometry
    // behind them to be incorrectly z-culled (showing as holes through
    // models). Condition: alpha > 0 passes. Also benefits transparent wall
    // textures (grates, energy fields).
    C3D_AlphaTest(true, GPU_GREATER, 0);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// Draws one eye's worth of screen contents: the bitblt quad (HUD/menus
// from software framebuffer) plus the world geometry on top. Called once
// for mono, twice (left + right) for stereo. Caller is responsible for
// FrameDrawOn(target) and (for world mode) building world_projection
// before calling.
//
// Note: world_frame_end is called once per eye, but it only ISSUES
// draw commands. The world VBO and batch list are populated once per
// game frame and reused across both eyes. That's the whole win of
// stereo done this way: vertex transform happens once on CPU, GPU
// draws twice with different projection matrices.
static void draw_screen_contents(void)
{

    // Re-enable depth test + write for world geometry drawn after this.
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);

    world_frame_end();

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

    // Bitblt quad is the 2D UI/HUD layer. Draw it with depth writes DISABLED
    // so it doesn't occlude world geometry. Depth test is irrelevant here
    // because bitblt is drawn first (nothing to test against yet).
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_DrawArrays(GPU_TRIANGLES, 0, quad_list_count);
}

static u32 packed_palette[256];
static u8 last_palette[768];  // 256 * 3
static bool palette_dirty = true;

// Call this when the palette changes (or just every frame — it's 256 iters):
static void update_packed_palette(void) {
    extern ubyte gr_current_pal[];
    if (memcmp(last_palette, gr_current_pal, 768) == 0) return;  // unchanged
    memcpy(last_palette, gr_current_pal, 768);
    
    for (int i = 0; i < 256; i++) {
        u8 r = gr_current_pal[i*3+0] << 2;
        u8 g = gr_current_pal[i*3+1] << 2;
        u8 b = gr_current_pal[i*3+2] << 2;
        u8 a = (i == 0) ? 0x00 : 0xFF;
        packed_palette[i] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
    }
}

extern u64 xform_ticks;
extern u64 polygon_ticks;
void bitblt_to_screen(void)
{
  if (!aptMainLoop()) {
    printf("TODO: Shutdown handling\n");
  }
    u64 t0, t1, t2, t3, t4, t5;
    t0 = svcGetSystemTick();
    hidScanInput();
    keyboard_handler();

#if RENDER_MODE == RENDER_MODE_BITBLT
  update_packed_palette();
    if (!swizzle_lut_init) init_swizzle_lut();

    u32* dst = (u32*)back_tex.data;
    int tiles_per_row = 512 / 8;

    // Iterate in tile order — better cache behavior on dst
    int tiles_y = UPLOAD_H / 8;
    int tiles_x = GAME_W / 8;
  for (int ty = 0; ty < 200 / 8; ty++) {
    for (int tx = 0; tx < 320 / 8; tx++) {
      u32* tile_dst = dst + (ty * tiles_per_row + tx) * 64;
      const u8* tile_src = back_buffer + (ty * 8) * 320 + (tx * 8);
      
      for (int py = 0; py < 8; py++) {
        const u8* row_src = tile_src + py * 320;
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
C3D_TexFlush(&back_tex);
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
    t1 = svcGetSystemTick();
    //tex_upload_software(&back_tex, (u32*)tex_buf, TEX_W, GAME_W, UPLOAD_H, TEX_W);
#endif

    // ----- Stereo 3D support -----
    // The 3D slider on the console returns 0.0 (off) to 1.0 (max). We map
    // it to an interocular distance (IOD). The /3 divisor matches the
    // citro3d sample's "tame the effect" tweak — full slider produces a
    // comfortable depth feel without too much eye strain.
    t2 = svcGetSystemTick();
    float slider = osGet3DSliderState();
    float iod = slider / 3.0f;
    bool stereo = (iod > 0.0f);

    // Flush world_vbo from CPU cache so GPU reads the freshly-written data.
    // Without this, GPU may sample stale cache lines, producing geometry
    // that doesn't match what we wrote — most visible in stereo where
    // the right eye's draws come after the left's and timing differences
    // can expose the cache coherency issue. Mono mode mostly worked by
    // luck (citro3d's frame-begin path happened to flush enough).
    if (world_vbo_count > 0) {
        GSPGPU_FlushDataCache(world_vbo, sizeof(world_vertex) * world_vbo_count);
    }

    // Pass 0 instead of C3D_FRAME_SYNCDRAW to allow CPU to start the next
    // frame while GPU is still rendering this one. SYNCDRAW was capping us
    // at 30 FPS by serializing CPU and GPU to vblank rate.
    //
    // Race protection: world_frame_begin calls gspWaitForP3D() before
    // allowing CPU writes to world_vbo. Texture uploads use C3D_TexFlush
    // which is synchronous. Risk: the bitblt back_tex gets re-uploaded
    // every frame; if GPU is still reading it during next frame's CPU
    // write, we'd see tearing in the bitblt area. If that happens, we
    // need to double-buffer back_tex.
    t3 = svcGetSystemTick();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    // ----- Left eye (always drawn) -----
    C3D_RenderTargetClear(target_left, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target_left);
#if WORLD_MODE == WORLD_MODE_TEXTURED
    world_build_projection(stereo ? -iod : 0.0f);  // negative iod = left eye
#endif
    draw_screen_contents();
    
    t4 = svcGetSystemTick();
    // ----- Right eye (only if slider engaged) -----
    if (stereo) {
        C3D_RenderTargetClear(target_right, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target_right);
#if WORLD_MODE == WORLD_MODE_TEXTURED
        world_build_projection(iod);  // positive iod = right eye
#endif
        draw_screen_contents();
    }

    C3D_FrameEnd(0);
    t5 = svcGetSystemTick();

    //printf("Rendered %d polygons\n", poly_count);
    // Print counters every 30 frames to reduce spam
    // static int print_throttle = 0;
    // if (++print_throttle >= 30) {
    //     print_throttle = 0;
    //     printf("polys=%d preload=%d hash=%d dynUp=%d compUp=%d compReuse=%d fail=%d\n",
    //            poly_count,
    //            frame_preloaded_hits, frame_hash_hits,
    //            frame_dynamic_uploads,
    //            frame_composite_uploads, frame_composite_reuses,
    //            frame_lookup_failures);
    // }

    poly_count = 0;
    frame_preloaded_hits = 0;
    frame_hash_hits = 0;
    frame_dynamic_uploads = 0;
    frame_composite_uploads = 0;
    frame_composite_reuses = 0;
    frame_lookup_failures = 0;

    // Reset world buffer for next frame
    world_frame_begin();

     printf("init %f ms, upload %f ms, pre-draw %f ms, draw(left) %f ms, draw(right) %f ms, cleanup %f ms\n",
            (t1 - t0) / (double)CPU_TICKS_PER_MSEC,
             (t2 - t1) / (double)CPU_TICKS_PER_MSEC,
             (t3 - t2) / (double)CPU_TICKS_PER_MSEC,
             (t4 - t3) / (double)CPU_TICKS_PER_MSEC,
             (t5 - t4) / (double)CPU_TICKS_PER_MSEC,
             (svcGetSystemTick() - t5) / (double)CPU_TICKS_PER_MSEC);

    // printf("xform: %f ms, poly: %f ms, tmap: %f ms\n",
    //        xform_ticks / (double)CPU_TICKS_PER_MSEC,
    //        polygon_ticks / (double)CPU_TICKS_PER_MSEC,
    //        tmap_ticks / (double)CPU_TICKS_PER_MSEC);
    // xform_ticks = 0;
    // polygon_ticks = 0;
    // tmap_ticks = 0;
  
    // printf("tmap %f ms: t_lookup %f ms, t_convert %f ms, t_clip %f ms, t_emit %f ms\n",
    //        tmap_ticks / (double)CPU_TICKS_PER_MSEC,
    //        t_lookup / (double)CPU_TICKS_PER_MSEC,
    //        t_convert / (double)CPU_TICKS_PER_MSEC,
    //        t_clip / (double)CPU_TICKS_PER_MSEC,
    //        t_emit / (double)CPU_TICKS_PER_MSEC);

    // tmap_ticks = 0; t_lookup = 0; t_convert = 0; t_clip = 0; t_emit = 0;
}

#endif // RENDER_MODE branch

// =============================================================================
// GPU bring-up — called once at startup
// =============================================================================
void init_3ds_gpu(void)
{
    gfxSet3D(true);   // enable stereoscopic 3D — used when slider > 0
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    // Two render targets so we can draw left + right eye separately.
    // When the 3D slider is at 0, only target_left is drawn (mono mode).
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
