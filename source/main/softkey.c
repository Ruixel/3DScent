#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "inferno.h"
#include "game.h"
#include "screens.h"
#include "gauges.h"
#include "physics.h"
#include "error.h"

#include "mem.h"

#include "menu.h"			// For the font.
#include "mono.h"
#include "collide.h"
#include "newdemo.h"
#include "player.h"
#include "gamefont.h"
#include "hostage.h"
#include "bm.h"
#include "text.h"
#include "powerup.h"
#include "sounds.h"
#include "multi.h"
#include "network.h"
#include "endlevel.h"

#include "wall.h"
#include "text.h"
#include "render.h"
#include "piggy.h"

#include "softkey.h"

enum
{
	TYPE_EMPTY,
	TYPE_STRING_FONT_BIG,
	TYPE_STRING_FONT_MED_GREY,
	TYPE_STRING_FONT_MED_GOLD,
	TYPE_STRING_FONT_MED_BLUE,
	TYPE_STRING_FONT_SMALL,
	TYPE_IMAGE,
	TYPE_AUTOMAP,
	TYPE_SCORE,
	TYPE_HOMING_WARNING,
	TYPE_ENERY,
	TYPE_SHIELD,
	TYPE_PRIMARY_WEAPON,
	TYPE_SECONDARY_WEAPON,
	TYPE_KEYS,
	TYPE_CLOAK,
	TYPE_INVULN,
	TYPE_TIME,
	TYPE_FRAMERATE,
	TYPE_LIVES
};

// 135 138 140 142 143 136 144 146 147 149
softkey_list_t	sf_keyboard, sf_keyboard_shifted;
softkey_t		sk_default_ingame[] = {
	{0, 192, 25, 40 + 192, {135}, TYPE_IMAGE, 0, '1'},
	{25, 192, 50, 40 + 192, {138}, TYPE_IMAGE, 0, '2'},
	{50, 192, 75, 40 + 192, {140}, TYPE_IMAGE, 0, '3'},
	{75, 192, 100, 40 + 192, {142}, TYPE_IMAGE, 0, '4'},
	{100, 192, 125, 40 + 192, {143}, TYPE_IMAGE, 0, '5'},
	{125, 192, 150, 40 + 192, {136}, TYPE_IMAGE, 0, '6'},
	{150, 192, 175, 40 + 192, {144}, TYPE_IMAGE, 0, '7'},
	{175, 192, 200, 40 + 192, {146}, TYPE_IMAGE, 0, '8'},
	{200, 192, 225, 40 + 192, {147}, TYPE_IMAGE, 0, '9'},
	{225, 192, 250, 40 + 192, {149}, TYPE_IMAGE, 0, '0'},
	{-1, 192 + 52, 0, 0, {0}, TYPE_SCORE, 153, 0},
	{0x4000, -1, 0, 0, {0}, TYPE_HOMING_WARNING, 153, 0},
	{2, -1, 0, 0, {0}, TYPE_ENERY, 153, 0},
	{2, -9, 0, 0, {0}, TYPE_SHIELD, 153, 0},
	{-1, -9, 0, 0, {0}, TYPE_PRIMARY_WEAPON, 153, 0},
	{-1, -1, 0, 0, {0}, TYPE_SECONDARY_WEAPON, 153, 0},
	{2, 192 + 60, 0, 0, {0}, TYPE_KEYS, 153, 0},
	{2, -25, 0, 0, {0}, TYPE_CLOAK, 153, 0},
	{2, -33, 0, 0, {0}, TYPE_INVULN, 153, 0},
	{-1, 192 + 68, 0, 0, {0}, TYPE_TIME, 153, 0},
	{-1, 192 + 76, 0, 0, {0}, TYPE_FRAMERATE, 153, 0},
	{2, 192 + 52, 0, 0, {0}, TYPE_LIVES, 153, 0}
};

softkey_list_t	sf_default_ingame = {
	sk_default_ingame,
	22
};

softkey_list_t	*sf_custom;

softkey_list_t	*sf_active;

char	keyboard_list[] = "\4\r`1234567890-=\3\rqwertyuiop[]\rasdfghjkl;'\\\n\r\1zxcvbnm,./\2\r";
char	keyboard_shifted_list[] = "\4\r~!@#$%^&*()_+\3\rqwertyuiop{}\rasdfghjkl:\"|\n\r\1zxcvbnm<>?\2\r";

#define W	15
#define H	15
#define X	10
#define Y	45 + 192

softkey_list_t *softkey_set_list (softkey_list_t *list)
{
	softkey_list_t	*old = sf_active;
	if (list)
		sf_active = list;
	else
		sf_active = &sf_keyboard;
	return old;
}

void softkey_create_keyboard (softkey_list_t *list, char *keyname, char *keys)
{
	int	x = X, y = Y;
	char	*kn = keyname, *km = keys, str[16];
	softkey_t	*key;

	list->num = 0;
	for (; *kn; kn++)
	{
		if (*kn != '\r')
			list->num++;
	}
	list->keys = malloc (sizeof (softkey_t) * (list->num + 1));
	kn = keyname;
	for (key=list->keys; *kn; kn++, km++)
	{
		if (*kn == '\r')
		{
			x = X;
			y += H;
			continue;
		}
		else if (*kn == '\n')
		{
			sprintf (str, "enter");
			key->x2 = x + 50;
			key->y2 = y + H;
			key->x1 = x;
			x += 50;
		}
		else if (*kn == '\1')
		{
			sprintf (str, "shift");
			key->x2 = x + 45;
			key->y2 = y + H;
			key->x1 = x;
			x += 45;
		}
		else if (*kn == '\2')
		{
			sprintf (str, "shift");
			key->x2 = x + 45;
			key->y2 = y + H;
			key->x1 = x;
			x += 45;
		}
		else if (*kn == '\3')
		{
			sprintf (str, "back");
			key->x2 = x + 40;
			key->y2 = y + H;
			key->x1 = x;
			x += 40;
		}
		else if (*kn == '\4')
		{
			sprintf (str, "esc");
			key->x2 = x + 30;
			key->y2 = y + H;
			key->x1 = x;
			x += 30;
		}
		else
		{
			if (*kn == '%')
				sprintf (str, "%%%%");
			else
				sprintf (str, "%c", *kn);
			key->x2 = x + W;
			key->y2 = y + H;
			key->x1 = x;
			x += W;
		}
		key->y1 = y;
		key->color = 153;
		key->type = TYPE_STRING_FONT_MED_GREY;
		key->text = strdup (str);
		key->key = *km;
		key->border = 1;
		key++;
	}
	key->x1 = X + 60;
	key->y1 = y;
	key->x2 = X + 165;
	key->y2 = y + H;
	key->color = 153;
	key->type = TYPE_STRING_FONT_MED_GREY;
	key->text = " ";
	key->key = ' ';
	key->border = 1;
	list->num++;
}

void softkey_create_keyboards ()
{
	softkey_create_keyboard (&sf_keyboard, keyboard_list, keyboard_list);
	softkey_create_keyboard (&sf_keyboard_shifted, keyboard_shifted_list, keyboard_list);
	sf_active = &sf_default_ingame;
}

void softkey_free (softkey_list_t *sf)
{
	int	i;
	softkey_t	*key;

	for (i=0, key=sf->keys; i<sf->num; i++, key++)
	{
		switch (key->type)
		{
		case TYPE_STRING_FONT_BIG:
			free (key->text);
			break;

		case TYPE_STRING_FONT_MED_GREY:
			free (key->text);
			break;

		case TYPE_STRING_FONT_MED_GOLD:
			free (key->text);
			break;

		case TYPE_STRING_FONT_MED_BLUE:
			free (key->text);
			break;

		case TYPE_STRING_FONT_SMALL:
			free (key->text);
			break;
		}
	}
	free (sf->keys);
	free (sf);
}

void softkey_calc_pos (softkey_t *key)
{
	if (key->x1 < 0)
		key->x1 = 256 - key->x2;
	else if (key->x1 == 0x4000)
		key->x1 = (256 - key->x2) / 2;
	if (key->y1 < 0)
		key->y1 = 384 - key->y2;
}

void softkey_calc_text_pos (softkey_t *key)
{
	int	w, h, aw;
	gr_get_string_size (key->text, &w, &h, &aw);
	if (key->x1 == 0x4000)
		key->x1 = (256 - w) / 2;
	key->x2 = w + 4;
	key->y2 = h + 4;
	softkey_calc_pos (key);
}

void softkey_read (const char *filename)
{
	FILE	*fptr;
	char	c, buf[128], *p, *p1, *p2, *eb;
	int		i = 0, state, r, g, b;
	softkey_t	*key = NULL;
	bitmap_index	bmi;
	grs_bitmap	*bm;

	if (sf_custom)
	{
		softkey_free (sf_custom);
		sf_custom = NULL;
		sf_active = &sf_default_ingame;
	}

	if (!(fptr = fopen (filename, "rt")))
		return;

	sf_custom = malloc (sizeof (softkey_list_t));
	sf_custom->num = 0;

	while (!feof (fptr))
	{
		c = fgetc (fptr);
		if (c == '[')
			sf_custom->num++;
	}

	sf_custom->keys = malloc (sizeof (softkey_t) * sf_custom->num);
	fseek (fptr, 0, SEEK_SET);

	while (!feof (fptr))
	{
		fgets (buf, 128, fptr);
		eb = buf + strlen (buf);
		p = buf;
		while (*p == ' ')	p++;
		p1 = p;
		p2 = NULL;
		state = 0;
		for (; p<eb; p++)
		{
			switch (state)
			{
			case 0:
				if (*p == '[')
				{
					key = &sf_custom->keys[i++];
					key->color = 153;
					key->key = 0;
					key->type = TYPE_EMPTY;
					key->text = 0;
					key->x1 = 0;
					key->y1 = 0;
					key->x2 = -1;
					key->y2 = -1;
					key->border = 0;
					break;
				}

				if (*p == ' ')
					*p = 0;
				else if (*p == '=')
				{
					*p = 0;
					state = 1;
				}
				break;

			case 1:
				if (*p != ' ' && *p != '\n' && *p != '\r')
				{
					state = 2;
					p2 = p;
				}
				break;

			case 2:
				if (*p == '\n' || *p == '\r')
					*p = 0;
				break;
			}
		}
		if (state < 2)
			continue;

		if (!stricmp (p1, "x"))
			key->x1 = atoi (p2);
		else if (!stricmp (p1, "y"))
			key->y1 = atoi (p2);
		else if (!stricmp (p1, "w"))
			key->x2 = atoi (p2);
		else if (!stricmp (p1, "h"))
			key->y2 = atoi (p2);
		else if (!stricmp (p1, "color"))
		{
			p = p2;
			r = atoi (p);
			while (*p && *p != ' ')	p++;
			while (*p && *p == ' ')	p++;
			if (!*p)
				continue;
			g = atoi (p);
			while (*p && *p != ' ')	p++;
			while (*p && *p == ' ')	p++;
			if (!*p)
				continue;
			b = atoi (p);

			if (r < 0)	r = 0;
			else if (r > 255)	r = 255;
			if (g < 0)	g = 0;
			else if (g > 255)	g = 255;
			if (b < 0)	b = 0;
			else if (b > 255)	b = 255;

			key->color = gr_find_closest_color (r >> 3, g >> 3, b >> 3);
		}
		else if (!stricmp (p1, "type"))
		{
			if (!stricmp (p2, "text_big"))
				key->type = TYPE_STRING_FONT_BIG;
			else if (!stricmp (p2, "text_med_grey"))
				key->type = TYPE_STRING_FONT_MED_GREY;
			else if (!stricmp (p2, "text_med_gold"))
				key->type = TYPE_STRING_FONT_MED_GOLD;
			else if (!stricmp (p2, "text_med_blue"))
				key->type = TYPE_STRING_FONT_MED_BLUE;
			else if (!stricmp (p2, "text_small"))
				key->type = TYPE_STRING_FONT_SMALL;
			else if (!stricmp (p2, "image"))
				key->type = TYPE_IMAGE;
			else if (!stricmp (p2, "automap"))
				key->type = TYPE_AUTOMAP;
			else if (!stricmp (p2, "score"))
				key->type = TYPE_SCORE;
			else if (!stricmp (p2, "homing_warning"))
				key->type = TYPE_HOMING_WARNING;
			else if (!stricmp (p2, "energy"))
				key->type = TYPE_ENERY;
			else if (!stricmp (p2, "shield"))
				key->type = TYPE_SHIELD;
			else if (!stricmp (p2, "primary_weapon"))
				key->type = TYPE_PRIMARY_WEAPON;
			else if (!stricmp (p2, "secondary_weapon"))
				key->type = TYPE_SECONDARY_WEAPON;
			else if (!stricmp (p2, "keys"))
				key->type = TYPE_KEYS;
			else if (!stricmp (p2, "cloak"))
				key->type = TYPE_CLOAK;
			else if (!stricmp (p2, "invuln"))
				key->type = TYPE_INVULN;
			else if (!stricmp (p2, "time"))
				key->type = TYPE_TIME;
			else if (!stricmp (p2, "framerate"))
				key->type = TYPE_FRAMERATE;
			else if (!stricmp (p2, "lives"))
				key->type = TYPE_LIVES;
		}
		else if (!stricmp (p1, "image"))
		{
			bmi = piggy_find_bitmap (p2);
			if (bmi.index == 0xffff)
				key->type = TYPE_EMPTY;
			else
				key->img = bmi.index;
		}
		else if (!stricmp (p1, "text"))
			key->text = strdup (p2);
		else if (!stricmp (p1, "key"))
			key->key = p2[0];
		else if (!stricmp (p1, "border"))
			key->border = atoi (p2);
	}

	for (i=0, key=sf_custom->keys; i<sf_custom->num; i++, key++)
	{
		if (key->x2 == -1 && key->y2 == -1)
		{
			switch (key->type)
			{
			case TYPE_IMAGE:
				bmi.index = key->img;
				bm = &GameBitmaps[bmi.index];
				PIGGY_PAGE_IN (bmi);
				key->x2 = bm->bm_w;
				key->y2 = bm->bm_h;
				softkey_calc_pos (key);
				break;

			case TYPE_STRING_FONT_BIG:
				gr_set_curfont (Gamefonts[0]);
				softkey_calc_text_pos (key);
				break;

			case TYPE_STRING_FONT_MED_GREY:
				gr_set_curfont (Gamefonts[1]);
				softkey_calc_text_pos (key);
				break;

			case TYPE_STRING_FONT_MED_GOLD:
				gr_set_curfont (Gamefonts[2]);
				softkey_calc_text_pos (key);
				break;

			case TYPE_STRING_FONT_MED_BLUE:
				gr_set_curfont (Gamefonts[3]);
				softkey_calc_text_pos (key);
				break;

			case TYPE_STRING_FONT_SMALL:
				gr_set_curfont (Gamefonts[4]);
				softkey_calc_text_pos (key);
				break;
			}
		}
		if (key->x2 > -1)
			key->x2 += key->x1 - 1;
		else
			key->border = 0;
		if (key->y2 > -1)
			key->y2 += key->y1 - 1;
		else
			key->border = 0;
	}

	sf_active = sf_custom;

	fclose (fptr);
}

int gr_hline (int x1, int x2, int y);
int gr_vline (int y1, int y2, int x);

void hud_printf (int *x, int *y, const char *format, ...);
void do_automap_multiscreen (int x, int y, int w, int h);
void hud_show_score (int x, int y);
void hud_show_homing_warning (int x, int y);
void hud_show_energy (int x, int y);
void hud_show_shield (int x, int y);
void hud_show_primary_weapon (int x, int y);
void hud_show_secondary_weapon (int x, int y);
void hud_show_keys (int x, int y);
void hud_show_cloak (int x, int y);
void hud_show_invuln (int x, int y);
void show_time (int x, int y);
void show_framerate (int x, int y);
void hud_show_lives (int x, int y);

void softkey_drawborder (softkey_t *key)
{
	gr_setcolor (key->color);
	gr_hline (key->x1, key->x2, key->y1);
	gr_hline (key->x1, key->x2, key->y2);
	gr_vline (key->y1, key->y2, key->x1);
	gr_vline (key->y1, key->y2, key->x2);
}

void softkey_render_ingame (void)
{
	int	i;
	softkey_t	*key;
	grs_bitmap	*bm;
	bitmap_index	bmi;

	if (!sf_active)
		return;

	for (i=0, key=sf_active->keys; i<sf_active->num; i++, key++)
	{
		if (key->border)
			softkey_drawborder (key);

		switch (key->type)
		{
		case TYPE_IMAGE:
			bmi.index = key->img;
			bm = &GameBitmaps[bmi.index];
			PIGGY_PAGE_IN (bmi);
			gr_ubitmapm (key->x1, key->y1, bm);
			break;

		case TYPE_STRING_FONT_BIG:
			gr_set_curfont (Gamefonts[0]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_GREY:
			gr_set_curfont (Gamefonts[1]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_GOLD:
			gr_set_curfont (Gamefonts[2]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_BLUE:
			gr_set_curfont (Gamefonts[3]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_SMALL:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (Gamefonts[4]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_AUTOMAP:
			do_automap_multiscreen (key->x1, key->y1, key->x2 - key->x1, key->y2 - key->y1);
			break;

		case TYPE_SCORE:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_score (key->x1, key->y1);
			break;

		case TYPE_HOMING_WARNING:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_homing_warning (key->x1, key->y1);
			break;

		case TYPE_ENERY:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_energy (key->x1, key->y1);
			break;

		case TYPE_SHIELD:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_shield (key->x1, key->y1);
			break;

		case TYPE_PRIMARY_WEAPON:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_primary_weapon (key->x1, key->y1);
			break;

		case TYPE_SECONDARY_WEAPON:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_secondary_weapon (key->x1, key->y1);
			break;

		case TYPE_KEYS:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_keys (key->x1, key->y1);
			break;

		case TYPE_CLOAK:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_cloak (key->x1, key->y1);
			break;

		case TYPE_INVULN:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_invuln (key->x1, key->y1);
			break;

		case TYPE_TIME:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			show_time (key->x1, key->y1);
			break;

		case TYPE_FRAMERATE:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			show_framerate (key->x1, key->y1);
			break;

		case TYPE_LIVES:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (GAME_FONT);
			hud_show_lives (key->x1, key->y1);
			break;
		}
	}
}

void softkey_render (void)
{
	int	i;
	softkey_t	*key;
	grs_bitmap	*bm;
	bitmap_index	bmi;

	if (!sf_active)
		return;

	for (i=0, key=sf_active->keys; i<sf_active->num; i++, key++)
	{
		if (key->border)
			softkey_drawborder (key);

		switch (key->type)
		{
		case TYPE_IMAGE:
			bmi.index = key->img;
			bm = &GameBitmaps[bmi.index];
			PIGGY_PAGE_IN (bmi);
			gr_ubitmapm (key->x1, key->y1, bm);
			break;

		case TYPE_STRING_FONT_BIG:
			gr_set_curfont (Gamefonts[0]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_GREY:
			gr_set_curfont (Gamefonts[1]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_GOLD:
			gr_set_curfont (Gamefonts[2]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_MED_BLUE:
			gr_set_curfont (Gamefonts[3]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;

		case TYPE_STRING_FONT_SMALL:
			gr_set_fontcolor (key->color, -1);
			gr_set_curfont (Gamefonts[4]);
			gr_printf (key->x1 + 2, key->y1 +2, key->text);
			break;
		}
	}
}

#include "key.h"
void keyboard_updatekey (int keycode, int state);

static int last_key_touched = -1;
static int softkey_enabled = 0;

void softkey_enable (int enable)
{
	softkey_enabled = enable;
}

void softkey_touch (int x, int y)
{
	int	i, y1, y2;
	softkey_t	*key;

	if (!sf_active || !softkey_enabled)
		return;

	for (i=0, key=sf_active->keys; i<sf_active->num; i++, key++)
	{
		y1 = key->y1;
		y2 = key->y2;
		if (key->key == 0 || key->y2 < 192)
			continue;
		if (y1 >= 192)
			y1 -= 192;
		if (y2 >= 192)
			y2 -= 192;
		if (x >= key->x1 && x <= key->x2 &&
			y >= y1 && y <= y2)
		{
			if (key->key >= ' ')
			{
				last_key_touched = ascii_to_key (key->key);
			}
			else
			{
				switch (key->key)
				{
				case 1:
				case 2:
					if (sf_active == &sf_keyboard)
					{
						sf_active = &sf_keyboard_shifted;
						keyboard_updatekey (KEY_LSHIFT, 1);
					}
					else
					{
						sf_active = &sf_keyboard;
						keyboard_updatekey (KEY_LSHIFT, 0);
					}
					return;

				case 3:
					last_key_touched = KEY_BACKSP;
					break;

				case 4:
					last_key_touched = KEY_ESC;
					break;

				case '\n':
					last_key_touched = KEY_ENTER;
					break;
				}
			}
			if (last_key_touched == 255)
				last_key_touched = -1;
			if (last_key_touched == -1)
				return;
			keyboard_updatekey (last_key_touched, 1);
			return;
		}
	}
}

void softkey_release (void)
{
	if (last_key_touched != -1)
	{
		keyboard_updatekey (last_key_touched, 0);
		last_key_touched = -1;
	}
}
