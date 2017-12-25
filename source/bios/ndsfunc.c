#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "3ds.h"

#include "fix.h"
#include "gr.h"

#include "key.h"
#include "mouse.h"

#include "3d.h"
#include "game.h"
#include "grdef.h"

#include "error.h"

//#include "texmap.h"
#define	NUM_LIGHTING_LEVELS 32
#define MAX_LIGHTING_VALUE	((NUM_LIGHTING_LEVELS-1)*F1_0/NUM_LIGHTING_LEVELS)
int Lighting_on;
int Max_perspective_depth, Max_linear_depth, Current_seg_depth;

#include "mem.h"
#include "ndsfunc.h"

#include "softkey.h"

//#define WIFI_DEBUG
//#define NO3D
#define EXCEPT_DEBUG
//#define CONSOLE

ITCM_CODE void g3_draw_poly (int nv, vms_vector **pointlist);
ITCM_CODE void g3_draw_tmap (int nv, vms_vector **pointlist, g3s_uvl *uvl_list, grs_bitmap *bm);
ITCM_CODE void g3_draw_bitmap (vms_vector *pos,fix width,fix height,grs_bitmap *bm);

#define front_buffer_top	((u16 *)0x06000000)
#define front_buffer_bottom	((u16 *)0x06200000)
ALIGN(4) u8	back_buffer[(400*240)];
u16	ds_palette[256];

int	palette_updated;

#define palette_top		((u16 *)0x5000000)
#define palette_bottom	((u16 *)0x5000400)
#define palette3d		((u16 *)0x6890000)

#define TEX_SIZE	64
#define MY_TEXTURE_SIZE	TEXTURE_SIZE_64

#define TEXTURE_MEMORY	(128 * 1024 * 4)
#define MAX_GL_TEXTURES	(TEXTURE_MEMORY / (TEX_SIZE * TEX_SIZE))

#define MAX_TEX_BUFFER	24
ALIGN(4) u8	textures[MAX_TEX_BUFFER][TEX_SIZE * TEX_SIZE];
typedef struct texture_ll_s
{
	u8	*texture;
	u32	*addr;
	struct texture_ll_s	*next;
} texture_ll_t;

texture_ll_t	texture_ll_pool[MAX_TEX_BUFFER], *texture_ll, *texture_ll_free;

/*ITCM_CODE void ITCM_DC_FlushRange (void *base, u32 size)
{
	__asm__ ("\
	add	r1, r1, r0; \
	bic	r0, r0, #31; \
.flush: \
	mcr	p15, 0, r0, c7, c14, 1; \
	add	r0, r0, #32; \
	cmp	r0, r1; \
	blt	.flush");
}*/

ITCM_CODE void sync_palette ()
{
	/*u16	*s, *dt, *db, *d3d;
	int	i;

	//vramSetBankF (VRAM_F_LCD);
	for (i=0, s=ds_palette, dt=palette_top, db=palette_bottom, d3d=palette3d; i<256; i++)
	{
		*dt++ = *s;
		*db++ = *s;
		*d3d++ = *s++;
	}
	palette3d[0] = ds_palette[255];
	palette3d[255] = ds_palette[0];
	//vramSetBankF (VRAM_F_TEX_PALETTE);*/
}

ITCM_CODE void irq_Vblank (void)
{
	/*texture_ll_t	*tex;

	if (palette_updated)
	{
		sync_palette ();
		palette_updated = 0;
	}

	if (texture_ll)
	{
		VRAM_CR = VRAM_ENABLE | (VRAM_ENABLE << 8) | (VRAM_ENABLE << 16) | (VRAM_ENABLE << 24);

		for (tex=texture_ll; tex->next; tex=tex->next)
			dmaCopyWords (3, tex->texture, tex->addr, TEX_SIZE * TEX_SIZE);
		dmaCopyWordsAsynch (3, tex->texture, tex->addr, TEX_SIZE * TEX_SIZE);
		tex->next = texture_ll_free;
		texture_ll_free = texture_ll;
		texture_ll = NULL;

		VRAM_A_CR = VRAM_ENABLE | VRAM_A_TEXTURE;
		VRAM_B_CR = VRAM_ENABLE | VRAM_B_TEXTURE;
		VRAM_C_CR = VRAM_ENABLE | VRAM_C_TEXTURE;
		VRAM_D_CR = VRAM_ENABLE | VRAM_D_TEXTURE;
// this doesnt work
//		VRAM_CR = ((VRAM_ENABLE | VRAM_A_TEXTURE) | ((VRAM_ENABLE | VRAM_B_TEXTURE) << 8) | ((VRAM_ENABLE | VRAM_C_TEXTURE) << 16) | ((VRAM_ENABLE | VRAM_D_TEXTURE) << 24));
// but this does ?
//		vramRestoreMainBanks ((VRAM_ENABLE | VRAM_A_TEXTURE) | ((VRAM_ENABLE | VRAM_B_TEXTURE) << 8) | ((VRAM_ENABLE | VRAM_C_TEXTURE) << 16) | ((VRAM_ENABLE | VRAM_D_TEXTURE) << 24));*/
	//}
}

bool doSleep;

ITCM_CODE void bitblt_to_screen ()
{
    hidScanInput();
	/*scanKeys ();
/*
	{
		extern grs_font *Gamefonts[];
		extern soundsys_t *sndsys;
		extern int Config_master_volume, digi_volume;
		extern int channel_sounds[];
		extern char Sounds_name[][9];
		int	i;
		grs_canvas	*save_canvas;
		save_canvas = grd_curcanv;
		gr_set_current_canvas (&VR_screen_pages[0]);
		gr_clear_canvas (0);
		gr_set_curfont (Gamefonts[4]);
		gr_set_fontcolor (153, -1);
		for (i=0; i<16; i++)
			gr_printf (0, i * 10, "%2d %02x %d %02x %02x 0x%08x %04x %4d %s", i, sndsys->channels[i].state, sndsys->channels[i].loop, sndsys->channels[i].vol, sndsys->channels[i].pan, sndsys->channels[i].data, sndsys->channels[i].len, channel_sounds[i], Sounds_name[channel_sounds[i]]);
		gr_printf (0, 180, "digi : %d max : %d", digi_volume, Config_master_volume);
		gr_set_current_canvas (save_canvas);
	}
*/
	/*keyboard_handler ();
	mouse_handler ();

	DC_FlushRange (back_buffer, 256 * 192 * 2);
	dmaCopyWords (2, back_buffer, front_buffer_top, 256 * 192);
	dmaCopyWordsAsynch (2, back_buffer + (256 * 192), front_buffer_bottom, 256 * 192);

	while (GFX_STATUS & (1 << 27)); // wait till gfx engine is not busy
	GFX_FLUSH = 2;
//	while (GFX_STATUS & (1 << 27)); // wait till gfx engine is not busy

	// Clear the FIFO
//	GFX_STATUS |= (1 << 29) | (1 << 15);
	swiWaitForVBlank ();*/
	//printf("Drawing bitblt\n");

	gr_rect(10,20,30,40);

	gfxFlushBuffers();
    //DC_FlushRange (back_buffer, 256 * 192);
    u8* framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    memcpy(framebuffer, back_buffer, 400*240);

    // Flush and swap framebuffers
    gfxSwapBuffers();

    //Wait for VBlank
    gspWaitForVBlank();
}

#ifdef WIFI_DEBUG
#include <debug_stub.h>
#include <debug_tcp.h>
#endif

#ifdef EXCEPT_DEBUG
static const char *registerNames[] =
	{	"r0","r1","r2","r3","r4","r5","r6","r7",
		"r8 ","r9 ","r10","r11","r12","sp ","lr ","pc " };

extern const char __itcm_start[];
u32 getExceptionAddress( u32 opcodeAddress, u32 thumbState);

void set_error_console ()
{
	/*videoSetMode(0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankC(VRAM_C_SUB_BG);

	REG_BG0CNT_SUB = BG_MAP_BASE(31);

	BG_PALETTE_SUB[0] = RGB15(31,0,0);
	BG_PALETTE_SUB[255] = RGB15(31,31,31);

	consoleDemoInit();*/
}
//---------------------------------------------------------------------------------
static void my_handler()
{
//---------------------------------------------------------------------------------
	/*set_error_console ();

	iprintf("\x1b[5CException Occured!\n");
	u32	currentMode = getCPSR() & 0x1f;
	u32 thumbState = ((*(u32*)0x027FFD90) & 0x20);

	u32 codeAddress, exceptionAddress = 0;

	int offset = 8;

	if ( currentMode == 0x17 ) {
		iprintf ("\x1b[10Cdata abort!\n\n");
		codeAddress = exceptionRegisters[15] - offset;
		if (	(codeAddress > 0x02000000 && codeAddress < 0x02400000) ||
				(codeAddress > (u32)__itcm_start && codeAddress < (u32)(__itcm_start + 32768)) )
			exceptionAddress = getExceptionAddress( codeAddress, thumbState);
		else
			exceptionAddress = codeAddress;

	} else {
		if (thumbState)
			offset = 2;
		else
			offset = 4;
		iprintf("\x1b[5Cundefined instruction!\n\n");
		codeAddress = exceptionRegisters[15] - offset;
		exceptionAddress = codeAddress;
	}

	iprintf("  pc: %08X addr: %08X\n\n",codeAddress,exceptionAddress);

	int i;
	for ( i=0; i < 8; i++ ) {
		iprintf(	"  %s: %08X   %s: %08X\n",
					registerNames[i], exceptionRegisters[i],
					registerNames[i+8],exceptionRegisters[i+8]);
	}
	iprintf("\n");
	u32 *stack = (u32 *)exceptionRegisters[13];
	for ( i=0; i<10; i++ ) {
		iprintf( "\x1b[%d;2H%08X: %08X %08X", i + 14, (u32)&stack[i*2],stack[i*2], stack[(i*2)+1] );
	}
/*	{
		FILE	*fptr;
		int	i;
		u8	j;
		fptr = fopen ("stack.dmp", "wb");
		for (i=0; i<0x4000; i++)
		{
			j = *(u8 *)(0xb000000 + i);
			fwrite (&j, 1, 1, fptr);
		}
		fclose (fptr);
	}
*/	//while(1);
}
#endif

void Snd_ParseMessage (u32 msg);

ITCM_CODE void irq_arm9_fifo (void)
{
	/*while ( !(REG_IPC_FIFO_CR & IPC_FIFO_RECV_EMPTY))
	{
		u32 value = REG_IPC_FIFO_RX;
		u32 msg = value & 0xffffff;
		switch (value >> 24)
		{
		case 0x01:	// Sound Message
			//Snd_ParseMessage (msg);
			break;

		case 0x02:	// Sleep
			doSleep = msg >> 8 & 0xff;
			break;
		}
	}*/
}

void nds_init ()
{
	/*softkey_create_keyboards ();

	powerOn (POWER_ALL);

	irqSet (IRQ_VBLANK, (irq_func_t)irq_Vblank);
	videoSetMode (MODE_5_3D | DISPLAY_BG3_ACTIVE);
	vramSetBankA (VRAM_A_TEXTURE);
	vramSetBankB (VRAM_B_TEXTURE);
	vramSetBankC (VRAM_C_TEXTURE);
	vramSetBankD (VRAM_D_TEXTURE);
	vramSetBankE (VRAM_E_MAIN_BG);
	vramSetBankF (VRAM_F_TEX_PALETTE);
	REG_BG0CNT = BG_PRIORITY_1;
	REG_BG3CNT = BG_BMP8_256x256 | BG_PRIORITY_0;
	REG_BG3PA = 1 << 8;
	REG_BG3PB = 0;
	REG_BG3PC = 0;
	REG_BG3PD = 1 << 8;
	REG_BG3X = 0;
	REG_BG3Y = 0;

#ifdef CONSOLE
	videoSetModeSub (MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankH (VRAM_H_SUB_BG);
	vramSetBankI (VRAM_I_SUB_BG_0x06208000);
	REG_BG0CNT_SUB = BG_MAP_BASE (31);
	BG_PALETTE_SUB[255] = RGB15 (31, 31, 31);
	consoleDemoInit ();
#else
	videoSetModeSub (MODE_5_2D | DISPLAY_BG3_ACTIVE);
	vramSetBankH (VRAM_H_SUB_BG);
	vramSetBankI (VRAM_I_SUB_BG_0x06208000);
	REG_BG3CNT_SUB = BG_BMP8_256x256;
	REG_BG3PA_SUB = 1 << 8;
	REG_BG3PB_SUB = 0;
	REG_BG3PC_SUB = 0;
	REG_BG3PD_SUB = 1 << 8;
	REG_BG3X_SUB = 0;
	REG_BG3Y_SUB = 0;
	consoleDebugInit(DebugDevice_NOCASH);
#endif

	defaultExceptionHandler();

	glInit ();

	glEnable (GL_TEXTURE_2D);
	glEnable (GL_BLEND);

	glViewport (0, 0, 255, 191);
	glClearColor (0, 0, 0, 31);
	glClearPolyID (63);
	glClearDepth (0x7FFF);

	glPolyFmt (POLY_ALPHA (31) | POLY_CULL_BACK);

	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	gluPerspective (35, 1.0, 1.0, 5000);

	glMatrixMode (GL_TEXTURE);
	glLoadIdentity ();

	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	MATRIX_SCALE = 1048576;
	MATRIX_SCALE = 1048576;
	MATRIX_SCALE = -1048576;

	glColor (0x7fff);

	glColorTable (GL_RGB256, 0);

#ifdef WIFI_DEBUG
	{
		struct tcp_debug_comms_init_data init_data = {
			.port = 30000
		};
		if (!init_debug (&tcpCommsIf_debug, &init_data))
		{
			printf ("Failed to initialise the debugger stub - cannot continue\n");
			while (1) ;
		}
		debugHalt ();
	}
#endif

	if (!fatInitDefault ())
		Error ("fatInitDefault () failed");
	chdir("/dscent");
	init_nds_textures ();

#ifdef EXCEPT_DEBUG
	setExceptionHandler (my_handler) ;
#endif
   */
}

// sorta hack to support the cloaked effect
//extern void draw_tmap_flat(grs_bitmap *bp,int nverts,g3s_point **vertbuf);
/*
void g3_set_special_render(void (*tmap_drawer)(grs_bitmap *bm,int nv,g3s_point **vertlist),
						   void (*flat_drawer)(int nv,fix *vertlist),
						   int (*line_drawer)(fix x0,fix y0,fix x1,fix y1))
{
	if (tmap_drawer == 1)
	{
//		g3_draw_tmap_func = (g3_draw_tmap_func_t) g3_draw_tmap_flat;
		glPolyFmt (POLY_ALPHA (31 - Gr_scanline_darkening_level) | POLY_CULL_NONE);
	}
	else
	{
//		g3_draw_tmap_func = (g3_draw_tmap_func_t) g3_draw_tmap;
		glPolyFmt (POLY_ALPHA (31) | POLY_CULL_BACK);
	}
}
*/
void nds_set_render_size (int x, int y, int w, int h)
{
	//glViewport (x, y, x + w, y + h);
}

typedef struct
{
	int	last_frame_used;
	int	key;
	u32	format;
	grs_bitmap	*bm;
} gltexture_t;

gltexture_t	gl_textures[MAX_GL_TEXTURES];

u32	*next_tex_addr = (u32 *)0x06800000;
int	last_bound_key = -1;

void init_nds_textures (void)
{
	/*int	i;

	next_tex_addr = (u32 *)0x06800000;

	for (i=0; i<MAX_GL_TEXTURES; i++)
	{
		gl_textures[i].last_frame_used = -1;
		gl_textures[i].key = -1;
		gl_textures[i].format = 0;
		gl_textures[i].bm = NULL;
	}

	texture_ll = NULL;
	texture_ll_free = &texture_ll_pool[0];
	texture_ll_pool[0].texture = textures[0];
	for (i=1; i<MAX_TEX_BUFFER; i++)
	{
		texture_ll_pool[i - 1].next = &texture_ll_pool[i];
		texture_ll_pool[i].texture = textures[i];
	}
	texture_ll_pool[MAX_TEX_BUFFER - 1].next = NULL;*/
}

void scale_inbitmap(grs_bitmap *dst, grs_bitmap *src );
void gr_bm_ubitblt_rle(int w, int h, int dx, int dy, int sx, int sy, grs_bitmap * src, grs_bitmap * dest);

int find_bitmap_in_vram (int key)
{
	/*int	i;

	for (i=0; i<MAX_GL_TEXTURES; i++)
	{
		if (gl_textures[i].key == key)
			return i;
	}

	return -1;*/
}

ITCM_CODE void bind_texture (grs_bitmap *bmp)
{
	unsigned char	*data, *in;
	grs_bitmap		full;
	gltexture_t		*gltex;
	texture_ll_t	*tex;
	int	i;
	int	least_recently_used, lowest_frame_count;
	u32	*addr;
/*
	if (bmp->key == -1)
	{
		printf ("will not bind unkeyd texture\n");
		return;
	}
*/
	/*if (bmp->texname != -1 && gl_textures[bmp->texname].key == bmp->key)
	{
		gltex = &gl_textures[bmp->texname];
		gltex->last_frame_used = FrameCount;
		if (bmp->bm_flags & BM_FLAG_FORCE_REFRESH)
			goto refresh;
		GFX_TEX_FORMAT = gltex->format;
		return;
	}

	lowest_frame_count = gl_textures[0].last_frame_used;
	least_recently_used = 0;

	for (i=0, gltex=gl_textures; i<MAX_GL_TEXTURES; i++, gltex++)
	{
		if (gltex->key == bmp->key)
		{
			bmp->texname = i;
			GFX_TEX_FORMAT = gltex->format;
			gltex->last_frame_used = FrameCount;
			gltex->bm = bmp;
			return;
		}
		if (lowest_frame_count > gltex->last_frame_used)
		{
			lowest_frame_count = gltex->last_frame_used;
			least_recently_used = i;
		}
	}

	gltex = &gl_textures[least_recently_used];
	gltex->key = bmp->key;
	gltex->last_frame_used = FrameCount;
	if (gltex->bm)
		gltex->bm->bm_flags &= ~BM_FLAG_IN_VRAM;
	gltex->bm = NULL;
	bmp->texname = least_recently_used;
refresh:
	if (bmp->bm_flags & BM_FLAG_PAGED_OUT)
	{
		bitmap_index	idx;
		bmp->texname = -1;
		idx.index = bmp->key;
		bmp->bm_flags &= ~BM_FLAG_IN_VRAM;
		piggy_bitmap_page_in (idx);
	}

	if (!texture_ll_free)
	{
//		printf ("maximum buffered textured reached, skipping\n");
		return;
	}
	tex = texture_ll_free;
	texture_ll_free = texture_ll_free->next;
	data = tex->texture;

	full.bm_w = TEX_SIZE;
	full.bm_h = TEX_SIZE;
	full.bm_data = data;
	full.bm_rowsize = TEX_SIZE;

	if (bmp->bm_w != TEX_SIZE || bmp->bm_h != TEX_SIZE)
	{
		memset (data, 255, TEX_SIZE * TEX_SIZE);
		scale_inbitmap (&full, bmp);
		goto switch_transparency;
	}
	else if (bmp->bm_flags & BM_FLAG_RLE)
	{
		gr_bm_ubitblt_rle (bmp->bm_w, bmp->bm_h, 0, 0, 0, 0, bmp, &full);
		goto switch_transparency;
	}
	else
	{
		in = bmp->bm_data;
		for (i=0; i<TEX_SIZE * TEX_SIZE; i++, in++, data++)
		{
			if (*in == 255)
				*data = 0;
			else if (*in == 0)
				*data = 255;
			else
				*data = *in;
		}
		goto moveon;
	}
switch_transparency:
	for (i=0, in=data; i<TEX_SIZE * TEX_SIZE; i++, in++)
	{
		if (*in == 255)
			*in = 0;
		else if (*in == 0)
			*in = 255;
	}
moveon:
	DC_FlushRange (data, TEX_SIZE * TEX_SIZE);
	if (!(bmp->bm_flags & BM_FLAG_NO_PAGE_OUT))
	{
		free (bmp->bm_data);
		bmp->bm_data = NULL;
		bmp->bm_flags |= BM_FLAG_PAGED_OUT | BM_FLAG_IN_VRAM;
		gltex->bm = bmp;
	}

	if (gltex->format == 0)
	{
		addr = next_tex_addr;
		next_tex_addr += (TEX_SIZE * TEX_SIZE) >> 2;
	}
	else
	{
		addr = (u32 *)(((gltex->format & 0xffff) << 3) + 0x6800000);
	}

	if (bmp->bm_flags & BM_FLAG_TRANSPARENT)
		gltex->format = (GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T | GL_TEXTURE_COLOR0_TRANSPARENT) | (MY_TEXTURE_SIZE << 20) | (MY_TEXTURE_SIZE << 23) | (((uint32)addr >> 3) & 0xFFFF) | (GL_RGB256 << 26);
	else
		gltex->format = (GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T) | (MY_TEXTURE_SIZE << 20) | (MY_TEXTURE_SIZE << 23) | (((uint32)addr >> 3) & 0xFFFF) | (GL_RGB256 << 26);

	GFX_TEX_FORMAT = gltex->format;
	tex->addr = addr;
	tex->next = texture_ll;
	texture_ll = tex;*/
}

#define do_vertex_color(n) \
	light = uvl_list[(n)].l * NUM_LIGHTING_LEVELS; \
	if (light < (F1_0 / 2))	light = (F1_0 / 2); \
	if (light > MAX_LIGHTING_VALUE * NUM_LIGHTING_LEVELS) light = MAX_LIGHTING_VALUE * NUM_LIGHTING_LEVELS; \
	color = gr_fade_table[((light >> 8) & 0xff00) + 48]; \
	GFX_COLOR = ds_palette[color];

// Remove 6 bits of precision
#define do_vertex_tex_coord(n) \
	GFX_TEX_COORD = TEXTURE_PACK(uvl_list[(n)].u >> 6, uvl_list[(n)].v >> 6);

#define do_vertex(n) \
	do_vertex_coord ((n));

#define do_tex_vertex(n) \
	do_vertex_tex_coord ((n)); \
	do_vertex_coord ((n));

#define do_tex_lit_vertex(n) \
	do_vertex_color ((n)); \
	do_vertex_tex_coord ((n)); \
	do_vertex_coord ((n));

#define check_and_bind_texture(bm) \
	if (bm->key != last_bound_key) \
	{ \
		last_bound_key = bm->key; \
		bind_texture (bm); \
	}

g3_draw_tmap_func_t	g3_draw_tmap_func = (g3_draw_tmap_func_t)g3_draw_tmap_tex;

// Remove 12 bits of precision
#define do_vertex_coord(n) \
	GFX_VERTEX16 = VERTEX_PACK(pointlist[(n)]->x >> 12, pointlist[(n)]->y >> 12); \
	GFX_VERTEX16 = (pointlist[(n)]->z >> 12) & 0xffff;

ITCM_CODE void g3_draw_poly (int nv, vms_vector **pointlist)
{
	/*int	i;

	GFX_TEX_FORMAT = 0;
	last_bound_key = -1;
	GFX_COLOR = ds_palette[COLOR];
	if (nv == 3)
	{
		GFX_BEGIN = GL_TRIANGLES;
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
	}
	else if (nv == 4)
	{
		GFX_BEGIN = GL_QUADS;
		do_vertex (3);
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
	}
	else
	{
		GFX_BEGIN = GL_TRIANGLES;
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
		for (i=2; i<nv-1; i++)
		{
			do_vertex (i + 1);
			do_vertex (i);
			do_vertex (0);
		}
	}
	GFX_END = 0;*/
}

ITCM_CODE void g3_draw_tmap_flat (int nv, vms_vector **pointlist, g3s_uvl *uvl_list, grs_bitmap *bm)
{
	/*int	color, i;
	fix	average_light;

	GFX_TEX_FORMAT = 0;
	last_bound_key = -1;

	average_light = uvl_list[0].l;
	for (i=1; i<nv; i++)
		average_light += uvl_list[i].l;

	if (nv == 4)
		average_light = f2i (average_light * NUM_LIGHTING_LEVELS / 4);
	else
		average_light = f2i (average_light * NUM_LIGHTING_LEVELS / nv);

	if (average_light < 0)
		average_light = 0;
	else if (average_light > NUM_LIGHTING_LEVELS - 1)
		average_light = NUM_LIGHTING_LEVELS - 1;

	color = gr_fade_table[average_light * 256 + bm->avg_color];

	GFX_COLOR = ds_palette[color];
	if (nv == 3)
	{
		GFX_BEGIN = GL_TRIANGLES;
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
	}
	else if (nv == 4)
	{
		GFX_BEGIN = GL_QUADS;
		do_vertex (3);
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
	}
	else
	{
		GFX_BEGIN = GL_TRIANGLES;
		do_vertex (2);
		do_vertex (1);
		do_vertex (0);
		for (i=2; i<nv-1; i++)
		{
			do_vertex (i + 1);
			do_vertex (i);
			do_vertex (0);
		}
	}
	GFX_END = 0;*/
}

ITCM_CODE void g3_draw_tmap_tex (int nv, vms_vector **pointlist, g3s_uvl *uvl_list, grs_bitmap *bm)
{
	/*int	i;

	check_and_bind_texture (bm);

	if (!(bm->bm_flags & BM_FLAG_NO_LIGHTING) && Lighting_on)
	{
		u8	color;
		s32	light;
		if (nv == 3)
		{
			GFX_BEGIN = GL_TRIANGLES;
			do_tex_lit_vertex (2);
			do_tex_lit_vertex (1);
			do_tex_lit_vertex (0);
		}
		else if (nv == 4)
		{
			GFX_BEGIN = GL_QUADS;
			do_tex_lit_vertex (3);
			do_tex_lit_vertex (2);
			do_tex_lit_vertex (1);
			do_tex_lit_vertex (0);
		}
		else
		{
			GFX_BEGIN = GL_TRIANGLES;
			do_tex_lit_vertex (2);
			do_tex_lit_vertex (1);
			do_tex_lit_vertex (0);
			for (i=2; i<nv-1; i++)
			{
				do_tex_lit_vertex (i + 1);
				do_tex_lit_vertex (i);
				do_tex_lit_vertex (0);
			}
		}
		GFX_END = 0;
	}
	else
	{
		GFX_COLOR = 0x7fff;
		if (nv == 3)
		{
			GFX_BEGIN = GL_TRIANGLES;
			do_tex_vertex (2);
			do_tex_vertex (1);
			do_tex_vertex (0);
		}
		else if (nv == 4)
		{
			GFX_BEGIN = GL_QUADS;
			do_tex_vertex (3);
			do_tex_vertex (2);
			do_tex_vertex (1);
			do_tex_vertex (0);
		}
		else
		{
			GFX_BEGIN = GL_TRIANGLES;
			do_tex_vertex (2);
			do_tex_vertex (1);
			do_tex_vertex (0);
			for (i=2; i<nv-1; i++)
			{
				do_tex_vertex (i + 1);
				do_tex_vertex (i);
				do_tex_vertex (0);
			}
		}
		GFX_END = 0;
	}*/
}

ITCM_CODE void g3_draw_bitmap (vms_vector *pos,fix width,fix height,grs_bitmap *bm)
{
	/*g3s_point	pnt;
	fix			x1, x2, y1, y2, z;
	fix			u1, u2, v1, v2;

	if (g3_rotate_point (&pnt, pos) & CC_BEHIND)
		return;

	x1 = (pnt.p3_x - width) >> 12;
	x2 = (pnt.p3_x + width) >> 12;
	y1 = (pnt.p3_y - height) >> 12;
	y2 = (pnt.p3_y + height) >> 12;
	z  = (pnt.p3_z >> 12) & 0xffff;

	u1 = v1 = 0;
	u2 = v2 = 63 << 4;

	check_and_bind_texture (bm);

	GFX_COLOR = 0x7fff;
	GFX_BEGIN = GL_QUADS;
	GFX_TEX_COORD = TEXTURE_PACK (u1, v2);
	GFX_VERTEX16 = VERTEX_PACK (x1, y1);
	GFX_VERTEX16 = z;

	GFX_TEX_COORD = TEXTURE_PACK (u2, v2);
	GFX_VERTEX16 = VERTEX_PACK (x2, y1);
	GFX_VERTEX16 = z;
	GFX_TEX_COORD = TEXTURE_PACK (u2, v1);
	GFX_VERTEX16 = VERTEX_PACK (x2, y2);
	GFX_VERTEX16 = z;

	GFX_TEX_COORD = TEXTURE_PACK (u1, v1);
	GFX_VERTEX16 = VERTEX_PACK (x1, y2);
	GFX_VERTEX16 = z;
	GFX_END = 0;*/
}
