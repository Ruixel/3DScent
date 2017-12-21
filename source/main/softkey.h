#ifndef _SOFTKEY_H
#define _SOFTKEY_H

typedef struct
{
	short	x1, y1;
	short	x2, y2;
	union
	{
		int		img;
		char	*text;
	};
	byte	type;
	ubyte	color;
	ubyte	key;
	byte	border;
} softkey_t;

typedef struct
{
	softkey_t	*keys;
	int			num;
} softkey_list_t;

softkey_list_t *softkey_set_list (softkey_list_t *list);
void softkey_create_keyboards ();
void softkey_render ();
void softkey_render_ingame ();
void softkey_touch (int x, int y);
void softkey_release (void);
void softkey_enable (int enable);

#endif
