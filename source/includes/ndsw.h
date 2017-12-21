#ifndef __NDSW_H__
#define __NDSW_H__
#define bool ds_bool
#define byte ds_byte
#define KEY_A		nds_KEY_A
#define KEY_B		nds_KEY_B
#define KEY_SELECT	nds_KEY_SELECT
#define KEY_START	nds_KEY_START
#define KEY_RIGHT	nds_KEY_RIGHT
#define KEY_LEFT	nds_KEY_LEFT
#define KEY_UP		nds_KEY_UP
#define KEY_DOWN	nds_KEY_DOWN
#define KEY_R		nds_KEY_R
#define KEY_L		nds_KEY_L
#define KEY_X		nds_KEY_X
#define KEY_Y		nds_KEY_Y
#define PCXHeader	nds_PCXHeader
#define rgb			nds_rgb
//#include <nds.h>
//#include <fat.h>
#undef rgb
#undef PCXHeader
#undef KEY_A
#undef KEY_B
#undef KEY_SELECT
#undef KEY_START
#undef KEY_RIGHT
#undef KEY_LEFT
#undef KEY_UP
#undef KEY_DOWN
#undef KEY_R
#undef KEY_L
#undef KEY_X
#undef KEY_Y
#undef byte
#undef bool
#undef MAX_TEXTURES

typedef void (*irq_func_t)(void);
#endif
