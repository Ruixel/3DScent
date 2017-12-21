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
 * $Source: Smoke:miner:source:misc::RCS:fileutil.h $
 * $Revision: 1.4 $
 * $Author: allender $
 * $Date: 1995/10/25 14:06:34 $
 *
 * header file for file utilities
 *
 * $Log: fileutil.h $
 * Revision 1.4  1995/10/25  14:06:34  allender
 * don't write with CFILE -- use FILE
 *
 * Revision 1.3  1995/05/11  13:00:45  allender
 * added functions which write swapped bytes
 *
 * Revision 1.2  1995/04/26  10:13:12  allender
 * added routines to read basic data types
 *
 * Revision 1.1  1995/03/30  15:02:51  allender
 * Initial revision
 *
*/

#ifndef _FILEUTIL_
#define _FILEUTIL_

#include "types.h"
#include "cfile.h"
#include "fix.h"

extern int filelength(FILE *fp);

// routines which read basic data types
extern byte read_byte(CFILE *fp);
extern short read_short(CFILE *fp);
extern int read_int(CFILE *fp);
extern fix read_fix(CFILE *fp);
// routines which write basic data types
extern int write_byte(FILE *fp, byte b);
extern int write_short(FILE *fp, short s);
extern int write_int(FILE *fp, int i);
extern int write_fix(FILE *fp, fix f);

struct DIR_ITER;
typedef struct
{
	char	ext[4];
	struct DIR_ITER	*dir;
} dir_t;

dir_t *dir_find (const char *ext);
const char *dir_findnext (dir_t *dir);
void dir_close (dir_t *dir);

#endif
