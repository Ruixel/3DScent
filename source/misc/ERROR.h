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
 * $Source: Smoke:miner:source:misc::RCS:error.h $
 * $Revision: 1.6 $
 * $Author: allender $
 * $Date: 1995/09/04 11:42:21 $
 * 
 * prototypes for error and warning dialogs
 * 
 * $Log: error.h $
 * Revision 1.6  1995/09/04  11:42:21  allender
 * made call to debug_video_mode on error and warning
 * in case screen is faded
 *
 * Revision 1.5  1995/08/23  21:28:44  allender
 * fix mcc compiler warning
 *
 * Revision 1.4  1995/08/16  10:00:59  allender
 * call key_close on Int3 -- made bios code restart key handler
 * if not installed
 *
 * Revision 1.3  1995/07/05  16:18:21  allender
 * new macro for Int3 to restore colors for debugger
 *
 * Revision 1.2  1995/05/11  12:57:11  allender
 * changed assert macro to do something useful
 *
 * Revision 1.1  1995/05/04  20:12:10  allender
 * Initial revision
 *
 * Revision 1.2  1995/04/18  16:03:06  allender
 * *** empty log message ***
 *
 * Revision 1.1  1995/03/09  09:31:23  allender
 * Initial revision
 *
 */
 
#ifndef _ERROR_H
#define _ERROR_H

void Error(char *format, ...);
void Warning(char *format, ...);

void set_exit_message(char *fmt,...);

#ifndef NDEBUG

#define BreakToLowLevelDebugger_()		 Debugger()
#define BreakStrToLowLevelDebugger_(s) DebugStr(s)
#define BreakToSourceDebugger_()		 SysBreak()
#define BreakStrToSourceDebugger_(s) 	SysBreakStr(s)

extern void debug_video_mode();
void MyAssert(int expr,char *expr_text,char *filename,int linenum);
#include <assert.h>
#define Assert(expr) assert ((expr))
//MyAssert(expr,#expr,__FILE__,__LINE__)
#define Int3() Assert(0) //do { debug_video_mode(); key_close(); BreakToSourceDebugger_(); } while(0)

#else

#define Assert(expr) ((void)0)
#define Int3() ((void)0)

#endif		// NDEBUG

#endif
