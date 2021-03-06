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
 * $Source: Smoke:miner:source:mem::RCS:mem.h $
 * $Revision: 1.6 $
 * $Author: allender $
 * $Date: 1995/09/04 11:39:39 $
 * 
 * Headers for safe malloc stuff.
 * 
 * $Log: mem.h $
 * Revision 1.6  1995/09/04  11:39:39  allender
 * change mem_align macro to call normal malloc
 *
 * Revision 1.5  1995/06/15  09:51:22  allender
 * new malloc prototype
 *
 * Revision 1.4  1995/05/12  11:28:34  allender
 * have two sets of memory routines which do the same thing to cover all bases
 *
 * Revision 1.3  1995/05/12  11:24:04  allender
 * changed #defines so that it is more clear which memory functions we are using
 *
 * Revision 1.2  1995/05/04  20:09:25  allender
 * removed some ifdefs which should probably be added back
 *
 * Revision 1.1  1995/03/09  09:58:08  allender
 * Initial revision
 *
 *
 * -- PC RCS information --
 * Revision 1.4  1994/11/27  21:10:58  matt
 * Now supports NDEBUG to turn off all special mem checking
 * 
 * Revision 1.3  1994/03/15  11:12:40  john
 * Made calloc fill block with zeros like it's
 * supposed to.
 * 
 * Revision 1.2  1993/11/04  14:02:39  matt
 * Added calloc() macro
 * 
 * Revision 1.1  1993/11/02  17:45:33  john
 * Initial revision
 * 
 * 
 */

#ifdef MEMDEBUG

void * mem_malloc( unsigned int size, char * file, int line );
void mem_free( void * buffer, char * file, int line );

#define malloc(size)    mem_malloc((size),__FILE__,__LINE__ )
#define free(ptr)       do{ mem_free(ptr,__FILE__,__LINE__); ptr=NULL; } while(0)

#endif
