#ifndef __NDSFUNC_H__
#define __NDSFUNC_H__

#include "3d.h"

void set_main_lcd (int top);
ITCM_CODE void bitblt_to_screen();
void delay (int time);
void nds_init ();
void nds_set_render_size (int x, int y, int w, int h);
void init_nds_textures ();

#endif
