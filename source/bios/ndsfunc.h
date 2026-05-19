#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "3ds.h"
#include "fix.h"
#include "gr.h"
#include "3d.h"

// -----------------------------------------------------------------------------
// Framebuffer state
// -----------------------------------------------------------------------------
extern u8   back_buffer[400 * 240];
extern u16  ds_palette[256];
extern int  palette_updated;
extern bool doSleep;

// -----------------------------------------------------------------------------
// Lighting (read by game code)
// -----------------------------------------------------------------------------
extern int Lighting_on;
extern int Max_perspective_depth;
extern int Max_linear_depth;
extern int Current_seg_depth;

// -----------------------------------------------------------------------------
// GPU state
// -----------------------------------------------------------------------------
extern bool gpu_inited;

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------
void init_3ds_gpu(void);
void init_nds_textures(void);
void sceneInit(void);

// -----------------------------------------------------------------------------
// Per-frame
// -----------------------------------------------------------------------------
void bitblt_to_screen(void);

// -----------------------------------------------------------------------------
// Rendering hooks called by Descent's game/object code
// -----------------------------------------------------------------------------
typedef void (*g3_draw_tmap_func_t)(int nv, vms_vector** pointlist,
                                    g3s_uvl* uvl_list, grs_bitmap* bm);

extern g3_draw_tmap_func_t g3_draw_tmap_func;

void g3_draw_poly(int nv, vms_vector** pointlist);
void g3_draw_poly_flat_color(int nv, vms_vector** pointlist, int color_idx);
void g3_draw_tmap_tex(int nv, vms_vector** pointlist,
                      g3s_uvl* uvl_list, grs_bitmap* bm);
void g3_draw_tmap_flat(int nv, vms_vector** pointlist,
                       g3s_uvl* uvl_list, grs_bitmap* bm);
void g3_draw_bitmap(vms_vector* pos, fix width, fix height, grs_bitmap* bm);
