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
 * $Source: Smoke:miner:source:bios::RCS:key.c $
 * $Revision: 1.19 $
 * $Author: allender $
 * $Date: 1995/11/14 14:23:17 $
 * 
 * Functions for keyboard handler.
 * 
 * $Log: key.c $
 * Revision 1.19  1995/11/14  14:23:17  allender
 * reallocate UPP when initing keyboard
 *
 * Revision 1.18  1995/10/21  23:47:49  allender
 * call GetOSEvent with all events -- screwed up thrustmaster stuff
 *
 * Revision 1.17  1995/10/17  15:35:45  allender
 * only get keyup and keydown events in GetOSEvent
 *
 * Revision 1.16  1995/09/06  13:08:31  allender
 * fixed command modifier for keys
 *
 * Revision 1.15  1995/08/25  11:06:43  allender
 * changed handler to key apprpriate track of time keys held down
 *
 * Revision 1.14  1995/08/25  09:38:36  allender
 * installed handler in key_inkey_time when not installed
 *
 * Revision 1.13  1995/08/18  10:15:52  allender
 * use vbl interrupt for keyboard handler to get keystrokes
 * more often because of high frame rate
 *
 * Revision 1.12  1995/07/26  16:57:35  allender
 * new style keyboard handler using getOSEvent ala DF
 *
 * Revision 1.11  1995/07/13  15:11:25  allender
 * fixed totally bogus key_shifted array
 *
 * Revision 1.10  1995/07/13  11:24:23  allender
 * trap checking for scancodes at 128 and not 256
 *
 * Revision 1.9  1995/05/15  13:55:40  allender
 * change prototypes for keyboard_proc
 *
 * Revision 1.8  1995/05/12  14:54:09  allender
 * make key_to_ascii returned unsigned char instead of char
 *
 * Revision 1.7  1995/05/12  12:19:13  allender
 * added call to mouse handler to get button pressed during
 * interrupt time
 *
 * Revision 1.6  1995/05/11  13:04:44  allender
 * fixed keyboard handling.   Removed gestalt handing -- should be in
 * macinit
 *
 * Revision 1.5  1995/05/04  20:02:43  allender
 * still working
 *
 * Revision 1.4  1995/04/05  13:49:48  allender
 * *** empty log message ***
 *
 * Revision 1.3  1995/03/21  13:39:51  allender
 * *** empty log message ***
 *
 * Revision 1.2  1995/03/21  13:28:11  allender
 * *** empty log message ***
 *
 * Revision 1.1  1995/03/09  09:32:25  allender
 * Initial revision
 *
 *
 * --- PC RCS information ---
 * Revision 1.34  1995/01/14  19:19:31  john
 * Made so when you press Shift+Baskspace, it release keys autmatically.
 * 
 * Revision 1.33  1994/12/13  09:21:48  john
 * Took out keyd_editor_mode, and KEY_DEBUGGED stuff for NDEBUG versions.
 * 
 * Revision 1.32  1994/11/12  13:52:01  john
 * Fixed bug with code that cleared bios buffer.
 * 
 * Revision 1.31  1994/10/24  15:16:16  john
 * Added code to detect KEY_PAUSE.
 * 
 * Revision 1.30  1994/10/24  13:57:53  john
 * Hacked in support for pause key onto code 0x61.
 * 
 * Revision 1.29  1994/10/21  15:18:13  john
 * *** empty log message ***
 * 
 * Revision 1.28  1994/10/21  15:17:24  john
 * Made LSHIFT+BACKSPACE do what PrtScr used to.
 * 
 * Revision 1.27  1994/09/22  16:09:18  john
 * Fixed some virtual memory lockdown problems with timer and
 * joystick.
 * 
 * Revision 1.26  1994/09/15  21:32:47  john
 * Added bounds checking for down_count scancode
 * parameter.
 * 
 * Revision 1.25  1994/08/31  12:22:20  john
 * Added KEY_DEBUGGED
 * 
 * Revision 1.24  1994/08/24  18:53:48  john
 * Made Cyberman read like normal mouse; added dpmi module; moved
 * mouse from assembly to c. Made mouse buttons return time_down.
 * 
 * Revision 1.23  1994/08/18  15:17:51  john
 * *** empty log message ***
 * 
 * Revision 1.22  1994/08/18  15:16:38  john
 * fixed some bugs with clear_key_times and then
 * removed it because i fixed key_flush to do the
 * same.
 * 
 * Revision 1.21  1994/08/17  19:01:25  john
 * Attempted to fix a bug with a key being held down
 * key_flush called, then the key released having too 
 * long of a time.
 * 
 * Revision 1.20  1994/08/08  10:43:48  john
 * Recorded when a key was pressed for key_inkey_time.
 * 
 * Revision 1.19  1994/06/22  15:00:03  john
 * Made keyboard close automatically on exit.
 * 
 * Revision 1.18  1994/06/21  09:16:29  john
 * *** empty log message ***
 * 
 * Revision 1.17  1994/06/21  09:08:23  john
 * *** empty log message ***
 * 
 * Revision 1.16  1994/06/21  09:05:01  john
 * *** empty log message ***
 * 
 * Revision 1.15  1994/06/21  09:04:24  john
 * Made PrtScreen do an int5
 * 
 * Revision 1.14  1994/06/17  17:17:06  john
 * Added keyd_time_last_key_was_pressed or something like that.
 * 
 * Revision 1.13  1994/05/14  13:55:16  matt
 * Added #define to control key passing to bios
 * 
 * Revision 1.12  1994/05/05  18:09:39  john
 * Took out BIOS to prevent stuck keys.
 * 
 * Revision 1.11  1994/05/03  17:39:12  john
 * *** empty log message ***
 * 
 * Revision 1.10  1994/04/29  12:14:20  john
 * Locked all memory used during interrupts so that program
 * won't hang when using virtual memory.
 * 
 * Revision 1.9  1994/04/28  23:49:41  john
 * Made key_flush flush more keys and also did something else but i forget what.
 * 
 * Revision 1.8  1994/04/22  12:52:12  john
 * *** empty log message ***
 * 
 * Revision 1.7  1994/04/01  10:44:59  mike
 * Change key_getch() to call getch() if our interrupt hasn't been installed.
 * 
 * Revision 1.6  1994/03/09  10:45:48  john
 * Neatend code a bit.
 * 
 * Revision 1.5  1994/02/17  17:24:16  john
 * Neatened up a bit.
 * 
 * Revision 1.4  1994/02/17  16:30:29  john
 * Put in code to pass keys when in debugger.
 * 
 * Revision 1.3  1994/02/17  15:57:59  john
 * Made handler not chain to BIOS handler.
 * 
 * Revision 1.2  1994/02/17  15:56:06  john
 * Initial version.
 * 
 * Revision 1.1  1994/02/17  15:54:07  john
 * Initial revision
 * 
 * 
 */
/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: key.c 1.19 1995/11/14 14:23:17 allender Exp $";
#pragma on (unreferenced)
*/
#include <stdlib.h>
#include <stdio.h>

#include "ndsw.h"

//#include "error.h"
#include "types.h"
#include "key.h"
#include "timer.h"

#include "softkey.h"

#define KEY_BUFFER_SIZE 16

//-------- Variable accessed by outside functions ---------
unsigned char	keyd_repeat;

unsigned char	keyd_pressed[256];
int			keyd_time_when_last_pressed;

typedef struct Key_info {
	ubyte		state;			// state of key 1 == down, 0 == up
	ubyte		last_state;		// previous state of key
	int			counter;		// incremented each time key is down in handler
	fix			timewentdown;	// simple counter incremented each time in interrupt and key is down
	fix			timehelddown;	// counter to tell how long key is down -- gets reset to 0 by key routines
	ubyte		downcount;		// number of key counts key was down
	ubyte		upcount;		// number of times key was released
} Key_info;

typedef struct keyboard	{
	unsigned short		keybuffer[KEY_BUFFER_SIZE];
	Key_info			keys[256];
	fix					time_pressed[KEY_BUFFER_SIZE];
	unsigned int 		keyhead, keytail;
} keyboard;

static keyboard key_data;

static unsigned char Installed=0;

unsigned char ascii_table[128] = 
{ 255, 255, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',255,255,
  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 255, 255,
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39, '`',
  255, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 255,'*',
  255, ' ', 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,255,255,
  255, 255, 255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255 };

unsigned char shifted_ascii_table[128] = 
{ 255, 255, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',255,255,
  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 255, 255,
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 
  255, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 255,255,
  255, ' ', 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,255,255,
  255, 255, 255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255 };

unsigned char key_to_ascii_table[128] =
{ 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  57, 255, 255, 255, 255, 255, 255, 40, 255, 255, 55, 255, 51, 12, 52, 53,
  11, 2, 3, 4, 5, 6, 7, 8, 9, 10, 255, 39, 255, 13, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 26, 43, 27, 255, 255,
  41, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24,
  25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 255, 255, 255, 255, 255 };

void keyboard_updatekey (int keycode, int state)
{
	Key_info *key;
	unsigned char temp;
	key = &(key_data.keys[keycode]);
	if ( key->last_state == state )	{
		if (state) {
			key->counter++;
			keyd_time_when_last_pressed = timer_get_fixed_seconds();
		}
	} else {
		if (state)	{
			keyd_pressed[keycode] = 1;
			key->downcount += state;
			key->state = 1;
			key->timewentdown = keyd_time_when_last_pressed = timer_get_fixed_seconds();
			key->counter++;
		} else {	
			keyd_pressed[keycode] = 0;
			key->upcount += key->state;
			key->state = 0;
			key->counter = 0;
			key->timehelddown += timer_get_fixed_seconds() - key->timewentdown;
		}
	}
	if ( (state && !key->last_state) || (state && key->last_state && (key->counter > 30) && (key->counter & 0x01)) ) {
		if (keycode != KEY_ESC &&
			keycode != KEY_ENTER &&
			keycode != KEY_BACKSP)
		{
			if ( keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT])
				keycode |= KEY_SHIFTED;
			if ( keyd_pressed[KEY_LALT] || keyd_pressed[KEY_RALT])
				keycode |= KEY_ALTED;
			if ( keyd_pressed[KEY_LCTRL] || keyd_pressed[KEY_RCTRL])
				keycode |= KEY_CTRLED;
		}
#ifndef NDEBUG
		if ( keyd_pressed[KEY_DELETE] )
			keycode |= KEY_DEBUGGED;
#endif
		temp = key_data.keytail+1;
		if ( temp >= KEY_BUFFER_SIZE ) temp=0;
		if (temp!=key_data.keyhead)	{
			key_data.keybuffer[key_data.keytail] = keycode;
			key_data.time_pressed[key_data.keytail] = keyd_time_when_last_pressed;
			key_data.keytail = temp;
		}
	}
	key->last_state = state;
}

softkey_list_t	*old_sk;

void keyboard_handler()
{
	int keys, pressed;

	pressed = keysDown ();

	if (!keyd_repeat)
		keys = keysDownRepeat ();
	else
		keys = keysHeld ();

	keyboard_updatekey (KEY_RIGHT, keys & nds_KEY_RIGHT ? 1 : 0);
	keyboard_updatekey (KEY_LEFT, keys & nds_KEY_LEFT ? 1 : 0);
	keyboard_updatekey (KEY_UP, keys & nds_KEY_UP ? 1 : 0);
	keyboard_updatekey (KEY_DOWN, keys & nds_KEY_DOWN ? 1 : 0);
	keyboard_updatekey (KEY_R, keys & nds_KEY_R ? 1 : 0);
	keyboard_updatekey (KEY_L, keys & nds_KEY_L ? 1 : 0);
	keyboard_updatekey (KEY_A, keys & nds_KEY_A ? 1 : 0);
	keyboard_updatekey (KEY_B, keys & nds_KEY_B ? 1 : 0);
	keyboard_updatekey (KEY_X, keys & nds_KEY_X ? 1 : 0);
	keyboard_updatekey (KEY_Y, keys & nds_KEY_Y ? 1 : 0);
	if (keys & nds_KEY_START && keys & nds_KEY_SELECT)
	{
		if (!old_sk)
			old_sk = softkey_set_list (NULL);
		else
		{
			softkey_set_list (old_sk);
			old_sk = NULL;
		}
	}
	else
	{
		keyboard_updatekey (KEY_ESC, pressed & nds_KEY_START ? 1 : 0);
		keyboard_updatekey (KEY_TAB, keys & nds_KEY_SELECT ? 1 : 0);
	}
}
/*
void key_close(void)
{
	if (!Installed)
		return;
	Installed = 0;
}
*/
void key_init()
{
	keyd_time_when_last_pressed = timer_get_fixed_seconds();
//	keyd_repeat = 1;

	if (Installed) return;
	Installed = 1;
		
	key_flush();

//	atexit(key_close);
}

unsigned char key_to_ascii(int keycode)
{
	int shifted;

	shifted = keycode & KEY_SHIFTED;
	keycode &= 0xFF;

	if ( keycode>=127 )
		return 255;

	if (shifted)
		return shifted_ascii_table[keycode];
	else
		return ascii_table[keycode];
}

unsigned char ascii_to_key(int keycode)
{
	keycode &= 0xFF;

	if ( keycode>=127 )
		return 255;

	return key_to_ascii_table[keycode];
}

void key_flush()
{
	int i;
	fix curtime;

	if (!Installed)
		key_init();
		
	key_data.keyhead = key_data.keytail = 0;

	//Clear the keyboard buffer
	for (i=0; i<KEY_BUFFER_SIZE; i++ )	{
		key_data.keybuffer[i] = 0;
		key_data.time_pressed[i] = 0;
	}
	
	curtime = timer_get_fixed_seconds();
	
	for (i=0; i<256; i++ )	{
		keyd_pressed[i] = 0;
		key_data.keys[i].state = 1;
		key_data.keys[i].last_state = 0;
		key_data.keys[i].timewentdown = curtime;
		key_data.keys[i].downcount=0;
		key_data.keys[i].upcount=0;
		key_data.keys[i].timehelddown = 0;
		key_data.keys[i].counter = 0;
	}

}

int add_one( int n )
{
	n++;
	if ( n >= KEY_BUFFER_SIZE ) n=0;
	return n;
}

// Returns 1 if character waiting... 0 otherwise
int key_checkch()
{
	int is_one_waiting = 0;

	if (key_data.keytail!=key_data.keyhead)
		is_one_waiting = 1;

	return is_one_waiting;
}

int key_inkey()
{
	int key = 0;

	if (!Installed)
		key_init();
		
	if (key_data.keytail!=key_data.keyhead) {
		key = key_data.keybuffer[key_data.keyhead];
		key_data.keyhead = add_one(key_data.keyhead);
	}

	return key;
}

int key_inkey_time(fix * time)
{
	int key = 0;

	if (!Installed)
		key_init();

	if (key_data.keytail!=key_data.keyhead)	{
		key = key_data.keybuffer[key_data.keyhead];
		*time = key_data.time_pressed[key_data.keyhead];
		key_data.keyhead = add_one(key_data.keyhead);
	}
	return key;
}



int key_peekkey()
{
	int key = 0;

	if (key_data.keytail!=key_data.keyhead)
		key = key_data.keybuffer[key_data.keyhead];

	return key;
}

// If not installed, uses BIOS and returns getch();
//	Else returns pending key (or waits for one if none waiting).
int key_getch()
{
	int dummy=0;

	if (!Installed)
		return 0;
//		return getch();

	while (!key_checkch())
		dummy++;
	return key_inkey();
}

unsigned int key_get_shift_status()
{
	unsigned int shift_status = 0;

	if ( keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT] )
		shift_status |= KEY_SHIFTED;

	if ( keyd_pressed[KEY_LALT] || keyd_pressed[KEY_RALT] )
		shift_status |= KEY_ALTED;

	if ( keyd_pressed[KEY_LCTRL] || keyd_pressed[KEY_RCTRL] )
		shift_status |= KEY_CTRLED;

#ifndef NDEBUG
	if (keyd_pressed[KEY_DELETE])
		shift_status |=KEY_DEBUGGED;
#endif

	return shift_status;
}

// Returns the number of seconds this key has been down since last call.
fix key_down_time(int scancode)
{
	fix time_down, time;

	if ((scancode<0)|| (scancode>255)) return 0;

	if (!keyd_pressed[scancode]) {
		time_down = key_data.keys[scancode].timehelddown;
		key_data.keys[scancode].timehelddown = 0;
	} else {
		time = timer_get_fixed_seconds();
		time_down = time - key_data.keys[scancode].timewentdown;
		key_data.keys[scancode].timewentdown = time;
	}

	return time_down;
}

// Returns number of times key has went from up to down since last call.
unsigned int key_down_count(int scancode)	{
	int n;

	if ((scancode<0)|| (scancode>255)) return 0;

	n = key_data.keys[scancode].downcount;
	key_data.keys[scancode].downcount = 0;

	return n;
}


// Returns number of times key has went from down to up since last call.
unsigned int key_up_count(int scancode)	{
	int n;

	if ((scancode<0)|| (scancode>255)) return 0;

	n = key_data.keys[scancode].upcount;
	key_data.keys[scancode].upcount = 0;

	return n;
}
