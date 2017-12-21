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
 * $Source: Smoke:miner:source:mem::RCS:mem.c $
 * $Revision: 1.16 $
 * $Author: allender $
 * $Date: 1995/10/26 19:24:31 $
 * 
 * Files for debugging memory allocator
 * 
 * $Log: mem.c $
 * Revision 1.16  1995/10/26  19:24:31  allender
 * removed stray character
 *
 * Revision 1.15  1995/10/26  14:13:25  allender
 * removed output to file for debugging
 *
 * Revision 1.14  1995/10/21  23:49:58  allender
 * took out memory output to external file #define
 *
 * Revision 1.13  1995/09/13  12:13:35  allender
 * assignments should be = not ==
 *
 * Revision 1.12  1995/09/13  11:42:28  allender
 * free space in heap calcs and warning messages
 *
 * Revision 1.11  1995/09/04  11:39:10  allender
 * took out some debug stuff and fixed the mem_align call
 *
 * Revision 1.10  1995/08/31  15:45:03  allender
 * memory debugging stuff
 *
 * Revision 1.9  1995/07/05  16:16:46  allender
 * use newptrclear instead of just NewPtr
 *
 * Revision 1.8  1995/06/25  21:54:25  allender
 * comment out align malloc for debug purposes
 *
 * Revision 1.7  1995/06/15  09:51:29  allender
 * new malloc to align data on specified memory boundry
 *
 * Revision 1.6  1995/06/07  08:02:54  allender
 * misspelling
 *
 * Revision 1.5  1995/06/07  08:01:34  allender
 * make unfreed bytes only show if show_mem_info is true. (I think
 * the info is not right anyway.)
 *
 * Revision 1.4  1995/05/12  11:24:45  allender
 * removed full memory checking -- Zero out all bytes on NewPtr
 *
 * Revision 1.3  1995/05/04  20:08:57  allender
 * call NewPtr instead of malloc
 *
 * Revision 1.2  1995/03/27  14:51:48  allender
 * *** empty log message ***
 *
 * Revision 1.1  1995/03/09  09:57:42  allender
 * Initial revision
 *
 *
 * --- PC RCS information ---
 * Revision 1.17  1994/11/29  15:40:38  matt
 * Added extra newline after message
 * 
 * Revision 1.16  1994/11/15  18:27:56  john
 * Took away show mem info by default.
 * 
 * Revision 1.15  1994/11/10  10:00:37  john
 * Made it show_mem_info by default.
 * 
 * Revision 1.14  1994/11/10  09:53:06  john
 * Put in more efficient, but less debugging info version.
 * 
 * Revision 1.13  1994/10/27  00:56:45  john
 * Reduced number of blocks; made show mem usage by default.
 * 
 * Revision 1.12  1994/10/06  19:15:17  john
 * Upped the number of blocks.
 * 
 * 
 * Revision 1.11  1994/07/27  20:04:22  john
 * Neatend printed output.
 * 
 * Revision 1.10  1994/07/20  10:22:17  yuan
 * Added overwrite error
 * 
 * Revision 1.9  1994/03/23  12:58:43  john
 * Made message global.
 * 
 * Revision 1.8  1994/03/15  11:12:59  john
 * Made calloc fill block with zeros like it's
 * supposed to.
 * 
 * Revision 1.7  1994/02/18  12:43:21  john
 * Only print mem debugging info if show_mem_info
 * is true. Need to set in debugger, btw.
 * 
 * Revision 1.6  1994/02/17  17:01:34  john
 * Took out MEM_LEAKAGE warning!
 * 
 * Revision 1.5  1994/01/24  16:04:47  john
 * Added mem_print_all function to dump all
 * allocated memory to inferno.mem file.
 * 
 * 
 * Revision 1.4  1994/01/18  11:01:41  john
 * *** empty log message ***
 * 
 * Revision 1.3  1993/12/10  12:20:50  john
 * Speed up by replacing mem_find_unused_id with a table lookup.
 * 
 * Revision 1.2  1993/12/08  12:38:22  mike
 * Change 10000 to MAX_INDEX
 * Use LargestIndex in place of MAX_INDEX as appropriate.
 * 
 * Revision 1.1  1993/11/02  17:45:28  john
 * Initial revision
 * 
 * 
 */

/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: mem.c 1.16 1995/10/26 19:24:31 allender Exp $";
#pragma on (unreferenced)
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "types.h"
#include "mono.h"
#include "error.h"

#include <malloc.h>
#ifdef MEMDEBUG
void * mem_malloc( unsigned int size, char * filename, int line, int fill_zero )
{
	void *ptr;

//	printf ("%d at %s (%d)\n", size, filename, line);

	if (size==0)	{
		Error("Attempt to malloc 0 bytes.\nVar %s, file %s, line %d.\n", filename, line);
	}

	ptr = (void *)malloc( size );		// clear when allocated

	if (ptr==NULL)	{
		Error("Malloc (%d) returned NULL\nVar %s, file %s, line %d.\n", size, filename, line );
		*(unsigned long *)0 = 0;
	}

	return ptr;
}

void mem_free( void * buffer , char * filename, int line)
{
	if (buffer==NULL)
		Error( "MEM: Freeing the NULL pointer!\n file %s, line %d.\n", filename, line);

	free (buffer);
}
#endif
