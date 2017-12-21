/*
THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.  
COPYRIGHT 1993-1998 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/
/*
 * $Source: BigRed:miner:source:bios::RCS:mouse.c $
 * $Revision: 1.8 $
 * $Author: allender $
 * $Date: 1996/02/21 13:57:36 $
 * 
 * Functions to access Mouse and Cyberman...
 * 
 * $Log: mouse.c $
 * Revision 1.8  1996/02/21  13:57:36  allender
 * cursor device manager stuff added here so as not to
 * rely on InterfaceLib anymore
 *
 * Revision 1.7  1995/10/17  15:42:21  allender
 * new mouse function to determine single button press
 *
 * Revision 1.6  1995/10/03  11:27:31  allender
 * fixed up hotspot problems with the mouse on multiple monitors
 *
 * Revision 1.5  1995/07/13  11:27:08  allender
 * trap button checks at MAX_MOUSE_BUTTONS
 *
 * Revision 1.4  1995/06/25  21:56:53  allender
 * added events include
 *
 * Revision 1.3  1995/05/11  17:06:38  allender
 * fixed up mouse routines
 *
 * Revision 1.2  1995/05/11  13:05:53  allender
 * of mouse handler code
 *
 * Revision 1.1  1995/05/05  09:54:45  allender
 * Initial revision
 *
 * Revision 1.9  1995/01/14  19:19:52  john
 * Fixed signed short error cmp with -1 that caused mouse
 * to break under Watcom 10.0
 * 
 * Revision 1.8  1994/12/27  12:38:23  john
 * Made mouse use temporary dos buffer instead of
 * 
 * allocating its own.
 * 
 * 
 * Revision 1.7  1994/12/05  23:54:53  john
 * Fixed bug with mouse_get_delta only returning positive numbers..
 * 
 * Revision 1.6  1994/11/18  23:18:18  john
 * Changed some shorts to ints.
 * 
 * Revision 1.5  1994/09/13  12:34:02  john
 * Added functions to get down count and state.
 * 
 * Revision 1.4  1994/08/29  20:52:19  john
 * Added better cyberman support; also, joystick calibration
 * value return funcctiionn,
 * 
 * Revision 1.3  1994/08/24  18:54:32  john
 * *** empty log message ***
 * 
 * Revision 1.2  1994/08/24  18:53:46  john
 * Made Cyberman read like normal mouse; added dpmi module; moved
 * mouse from assembly to c. Made mouse buttons return time_down.
 * 
 * Revision 1.1  1994/08/24  13:56:37  john
 * Initial revision
 * 
 * 
 */
/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: mouse.c 1.8 1996/02/21 13:57:36 allender Exp $";
#pragma on (unreferenced)
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "fix.h"
#include "mouse.h"
#include "timer.h"
#include "softkey.h"

#include "ndsw.h"

#define MOUSE_MAX_BUTTONS	1

typedef struct mouse_info {
	fix		ctime;
	int		num_buttons;
	ubyte	pressed[MOUSE_MAX_BUTTONS];
	fix		time_went_down[MOUSE_MAX_BUTTONS];
	fix		time_held_down[MOUSE_MAX_BUTTONS];
	uint	num_downs[MOUSE_MAX_BUTTONS];
	uint	num_ups[MOUSE_MAX_BUTTONS];
	ubyte	went_down;
	short	x, y, dx, dy;
} mouse_info;

static mouse_info Mouse;

static int Mouse_installed = 0;

int	touch_dx, touch_dy, old_px, old_py;

void mouse_handler()
{
	touchPosition touchPos;
 
	int	held = keysHeld ();
	int	px, py;
 
	touchRead(&touchPos);
	px = touchPos.px;
	py = touchPos.py;
 
	if (keysDown () & KEY_TOUCH)
	{
		old_px = px;
		old_py = py;
	}
	else if (keysHeld () & KEY_TOUCH)
	{
		touch_dx += px - old_px;
		touch_dy += py - old_py;
		old_px = px;
		old_py = py;
	}
	else
	{
		touch_dx = 0;
		touch_dy = 0;
	}

	Mouse.x = old_px;
	Mouse.dx = touch_dx;
	Mouse.y = old_py;
	Mouse.dy = touch_dy;
	touch_dx = 0;
	touch_dy = 0;

	Mouse.ctime = timer_get_fixed_seconds();
	if (held & KEY_TOUCH)
	{
		if (!Mouse.pressed[MB_LEFT])
		{
			Mouse.pressed[MB_LEFT] = 1;
			Mouse.time_went_down[MB_LEFT] = Mouse.ctime;
			Mouse.went_down = 1;
			softkey_touch (Mouse.x, Mouse.y);
		}
		Mouse.num_downs[MB_LEFT]++;
	}
	else
	{
		if (Mouse.pressed[MB_LEFT])
		{
			Mouse.pressed[MB_LEFT] = 0;
			Mouse.time_held_down[MB_LEFT] += Mouse.ctime-Mouse.time_went_down[MB_LEFT];
			Mouse.num_ups[MB_LEFT]++;
			Mouse.went_down = 0;
			softkey_release ();
		}
	}
}

void mouse_flush()
{
	int i;
	fix CurTime;
	
	if (!Mouse_installed)
		return;

	CurTime = timer_get_fixed_seconds();
	for (i = 0; i < MOUSE_MAX_BUTTONS; i++) {
		Mouse.pressed[i] = 0;
		Mouse.time_went_down[i] = CurTime;
		Mouse.time_held_down[i] = 0;
		Mouse.num_downs[i] = 0;
		Mouse.num_ups[i] = 0;
	}
	Mouse.went_down = 0;
}
/*
void mouse_close(void)
{
	if (Mouse_installed)
		Mouse_installed = 0;
}
*/
#define CursorDeviceTrap	0xaadb

int mouse_init()
{
	if (Mouse_installed)
		return Mouse.num_buttons;

	Mouse.num_buttons = 1;
	Mouse_installed = 1;
//	atexit(mouse_close);
	mouse_flush();
	
	return Mouse.num_buttons;
}

void mouse_set_limits( int x1, int y1, int x2, int y2 )
{
}

void mouse_get_pos( int *x, int *y)
{
	*x = Mouse.x;
	*y = Mouse.y;
}

void mouse_get_delta( int *dx, int *dy )
{
	*dx = Mouse.dx;
	*dy = Mouse.dy;
}

int mouse_get_btns()
{
	int i;
	uint flag=1;
	int status = 0;
	
	if (!Mouse_installed)
		return 0;

	for (i = 0; i < MOUSE_MAX_BUTTONS; i++) {
		if (Mouse.pressed[i])
			status |= flag;
		flag <<= 1;
	}
	return status;
}

void mouse_set_pos( int x, int y)
{
}

int mouse_went_down(int button)
{
	int count;
	if (!Mouse_installed)
		return 0;
	if ((button < 0) || (button >= MOUSE_MAX_BUTTONS))
		return 0;

	count = (int)Mouse.went_down;
	Mouse.went_down = 0;
	return count;
}

// Returns how many times this button has went down since last call.
int mouse_button_down_count(int button)	
{
	int count;
	
	if (!Mouse_installed)
		return 0;
		
	if ((button < 0) || (button >= MOUSE_MAX_BUTTONS))
		return 0;
		
	count = Mouse.num_downs[button];
	Mouse.num_downs[button] = 0;
	return count;
}

// Returns 1 if this button is currently down
int mouse_button_state(int button)	
{
	int state;

	if (!Mouse_installed)
		return 0;
	
	if ((button < 0) || (button >= MOUSE_MAX_BUTTONS))
		return 0;

	state = Mouse.pressed[button];
	return state;
}

// Returns how long this button has been down since last call.
fix mouse_button_down_time(int button)	
{
	fix time_down, time;
	
	if (!Mouse_installed)
		return 0;
		
	if ((button < 0) || (button >= MOUSE_MAX_BUTTONS))
		return 0;

	if (!Mouse.pressed[button]) {
		time_down = Mouse.time_held_down[button];
		Mouse.time_held_down[button] = 0;
	} else {
		time = timer_get_fixed_seconds();
		time_down = time - Mouse.time_held_down[button];
		Mouse.time_held_down[button] = 0;
	}

	return time_down;
}
