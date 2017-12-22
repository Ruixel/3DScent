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
 * $Source: BigRed:miner:source:cfile::RCS:cfile.c $
 * $Revision: 1.7 $
 * $Author: allender $
 * $Date: 1995/10/27 15:18:20 $
 *
 * Functions for accessing compressed files.
 *
 * $Log: cfile.c $
 * Revision 1.7  1995/10/27  15:18:20  allender
 * get back to descent directory before trying to read a hog file
 *
 * Revision 1.6  1995/10/21  23:48:24  allender
 * hogfile(s) are now in :Data: folder
 *
 * Revision 1.5  1995/08/14  09:27:31  allender
 * added byteswap header
 *
 * Revision 1.4  1995/05/12  11:54:33  allender
 * changed memory stuff again
 *
 * Revision 1.3  1995/05/04  20:03:38  allender
 * added code that was missing...use NewPtr instead of malloc
 *
 * Revision 1.2  1995/04/03  09:59:49  allender
 * *** empty log message ***
 *
 * Revision 1.1  1995/03/30  10:25:02  allender
 * Initial revision
 *
 *
 * --- PC RCS Information ---
 * Revision 1.24  1995/03/15  14:20:27  john
 * Added critical error checker.
 *
 * Revision 1.23  1995/03/13  15:16:53  john
 * Added alternate directory stuff.
 *
 * Revision 1.22  1995/02/09  23:08:47  matt
 * Increased the max number of files in hogfile to 250
 *
 * Revision 1.21  1995/02/01  20:56:47  john
 * Added cfexist function
 *
 * Revision 1.20  1995/01/21  17:53:48  john
 * Added alternate pig file thing.
 *
 * Revision 1.19  1994/12/29  15:10:02  john
 * Increased hogfile max files to 200.
 *
 * Revision 1.18  1994/12/12  13:20:57  john
 * Made cfile work with fiellentth.
 *
 * Revision 1.17  1994/12/12  13:14:25  john
 * Made cfiles prefer non-hog files.
 *
 * Revision 1.16  1994/12/09  18:53:26  john
 * *** empty log message ***
 *
 * Revision 1.15  1994/12/09  18:52:56  john
 * Took out mem, error checking.
 *
 * Revision 1.14  1994/12/09  18:10:31  john
 * Speed up cfgets, which was slowing down the reading of
 * bitmaps.tbl, which was making POF loading look slow.
 *
 * Revision 1.13  1994/12/09  17:53:51  john
 * Added error checking to number of hogfiles..
 *
 * Revision 1.12  1994/12/08  19:02:55  john
 * Added cfgets.
 *
 * Revision 1.11  1994/12/07  21:57:48  john
 * Took out data dir.
 *
 * Revision 1.10  1994/12/07  21:38:02  john
 * Made cfile not return error..
 *
 * Revision 1.9  1994/12/07  21:35:34  john
 * Made it read from data directory.
 *
 * Revision 1.8  1994/12/07  21:33:55  john
 * Stripped out compression stuff...
 *
 * Revision 1.7  1994/04/13  23:44:59  matt
 * When file cannot be opened, free up the buffer for that file.
 *
 * Revision 1.6  1994/02/18  12:38:20  john
 * Optimized a bit
 *
 * Revision 1.5  1994/02/15  18:13:20  john
 * Fixed more bugs.
 *
 * Revision 1.4  1994/02/15  13:27:58  john
 * Works ok...
 *
 * Revision 1.3  1994/02/15  12:51:57  john
 * Crappy inbetween version
 *
 * Revision 1.2  1994/02/14  20:12:29  john
 * First version working with new cfile stuff.
 *
 * Revision 1.1  1994/02/14  15:51:33  john
 * Initial revision
 *
 * Revision 1.1  1994/02/10  15:45:12  john
 * Initial revision
 *
 *
 */

/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: cfile.c 1.7 1995/10/27 15:18:20 allender Exp allender $";
#pragma on (unreferenced)
*/
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#include "cfile.h"
#include "error.h"
#include "mem.h"
#include "fileutil.h"
#include "misc.h"

typedef struct hogfile {
	char	name[13];
	int	    offset;
	int 	length;
} hogfile;

#define MAX_HOGFILES 250

hogfile HogFiles[MAX_HOGFILES];
char Hogfile_initialized = 0;
int Num_hogfiles = 0;

hogfile AltHogFiles[MAX_HOGFILES];
char AltHogfile_initialized = 0;
int AltNum_hogfiles = 0;
char AltHogFilename[64];

char AltHogDir[64];
char AltHogdir_initialized = 0;

void cfile_use_alternate_hogdir( char * path )
{
	if ( path )	{
		strcpy( AltHogDir, path );
		AltHogdir_initialized = 1;
	} else {
		AltHogdir_initialized = 0;
	}
}

extern int descent_critical_error;

FILE * cfile_get_filehandle( const char * filename, char * mode )
{
    // Create variables
	FILE * fp;
	char temp[128];

	// Open file
	descent_critical_error = 0;
	fp = fopen( filename, mode );
	if ( fp && descent_critical_error )	{
        // If there has been a critical error, abort opening
		fclose(fp);
		fp = NULL;
	}

	printf("Hog Dir: %s\n", AltHogDir);

	// If file was not found but the HOG file was initialized
	if ( (fp==NULL) && (AltHogdir_initialized) )	{
		strcpy( temp, AltHogDir );
		strcat( temp, filename );
		descent_critical_error = 0;
		fp = fopen( temp, mode );
		if ( fp && descent_critical_error )	{
			fclose(fp);
			fp = NULL;
		}
	}
	return fp;
}

char* findFile(const char* ds_fname)
{
    // Length of filename
    size_t pl = strlen(ds_fname) + 4;

    // From Prdoom, check 8 directories for the file we want
    /*for (int i = 0; i < 8; i++) {
        char *p;
        const char *directory = NULL;
        const char *s = NULL;

        switch(i)
    }*/

    //char* p = "ew";
    return NULL;
}

void cfile_init_hogfile(char *fname, hogfile * hog_files, int * nfiles )
{
    // Create variables
	char id[4];
	FILE * fp;
	int i, len;

	*nfiles = 0;

	//char romfs_fname[64] = "romfs:/";
	//strcat(romfs_fname, fname);

	fp = cfile_get_filehandle( fname, "rb" );
	if ( fp == NULL ) {
	    // Find the current working directory of the 3DS
	    char cwd[256];
	    if (getcwd(cwd, sizeof(cwd)) != NULL )
            printf("CWD: %s\n", cwd);

        Hogfile_initialized = 1;
        return;
	}

	fseek ( fp, 0, SEEK_CUR);
	fread( id, 3, 1, fp );
	if ( strncmp( id, "DHF", 3 ) )	{
		fclose(fp);
		return;
	}

	while( 1 )
	{
		if ( *nfiles >= MAX_HOGFILES ) {
			Warning( "ERROR: HOGFILE IS LIMITED TO %d FILES\n",  MAX_HOGFILES );
			fclose(fp);
			exit(1);
		}
		fseek (fp, 0, SEEK_CUR);
		i = fread( hog_files[*nfiles].name, 13, 1, fp );
		if ( i != 1 )	{
			fclose(fp);
			return;
		}
		fseek (fp, 0, SEEK_CUR);
		i = fread( &len, 4, 1, fp );
		if ( i != 1 )	{
			fclose(fp);
			return;
		}
		hog_files[*nfiles].length = len;
		if (hog_files[*nfiles].length < 0)
			Warning ("Hogfile length < 0");
		hog_files[*nfiles].offset = ftell( fp );
		*nfiles = (*nfiles) + 1;
		// Skip over
		i = fseek( fp, len, SEEK_CUR );
	}
}

FILE * cfile_find_libfile(const char * name, int * length)
{
	FILE * fp;
	int i;

	// Alternate HOG File
	if ( AltHogfile_initialized )	{
        Warning("ddsds");
		for (i=0; i<AltNum_hogfiles; i++ )	{
            Warning("ddsds2");
			if ( !stricmp( AltHogFiles[i].name, name ))	{
                Warning("ddsds3");
				fp = cfile_get_filehandle( AltHogFilename, "rb" );

				if ( fp == NULL )
                    return NULL;
				fseek( fp,  AltHogFiles[i].offset, SEEK_SET );
				*length = AltHogFiles[i].length;
				return fp;
			}
		}
	}

	// If the HOG file is not initialized then initialize it
	if ( !Hogfile_initialized ) 	{
		cfile_init_hogfile( "DESCENT.HOG", HogFiles, &Num_hogfiles );
		Hogfile_initialized = 1;
	}

    printf("Files: %d\n", Num_hogfiles);

    for (i=50; i<70; i++ )	{
        //printf("File %d: %s\n", i, HogFiles[i].name);
        //printf("Offset: %d\n", HogFiles[i].offset);
    }

    // Search in each hog file for the file we want
	for (i=0; i<10; i++ )	{
		/*if ( stricmp( HogFiles[i].name, name ))	{
            printf("File: %s\n", HogFiles[i].name);

			fp = cfile_get_filehandle( "DESCENT.HOG", "rb" );

			if ( fp == NULL )
                return NULL;

			fseek( fp,  HogFiles[i].offset, SEEK_SET );
			*length = HogFiles[i].length;

			return fp;
		} else {*/
            Warning("%s does not match %s", name, HogFiles[i].name);
            Warning("By %d", stricmp( HogFiles[i].name, name ));
		//}
	}
	Warning("%n could not be found", name);
	return NULL;
}

void cfile_use_alternate_hogfile( const char * name )
{
	if ( name )	{
		strcpy( AltHogFilename, name );
		cfile_init_hogfile( AltHogFilename, AltHogFiles, &AltNum_hogfiles );
		AltHogfile_initialized = 1;
	} else {
		AltHogfile_initialized = 0;
	}
}

int cfexist( const char * filename )
{
	int length;
	FILE *fp;

	fp = cfile_get_filehandle( filename, "rb" );		// Check for non-hog file first...
	if ( fp )	{
		fclose(fp);
		return 1;
	}

	fp = cfile_find_libfile(filename, &length );
	if ( fp )	{
		fclose(fp);
		return 2;		// file found in hog
	}

	return 0;		// Couldn't find it.
}

// Open a specific file
// Return a pointer to said file
CFILE * cfopen(const char * filename, char * mode )
{
    // Create some variables to be used
	int length;
	FILE * fp;
	CFILE *cfile;
//	char new_filename[256], *p;

    // Make sure that the given mode is RB
    // RB means read-only for non-text files
	if (stricmp( mode, "rb"))	{
		Warning( "CFILES CAN ONLY BE OPENED WITH RB\n" );
		exit(1);
	}
/*
	strcpy(new_filename, filename);
	while ( (p = strchr(new_filename, 13) ) )	*p = '\0';
	while ( (p = strchr(new_filename, 10) ) )	*p = '\0';
*/
    // Check for non-hog file first.
	fp = cfile_get_filehandle( filename, mode );
	if ( !fp ) {
        // If there's none found, then use the hog file to find it
		fp = cfile_find_libfile(filename, &length );
		if ( !fp ) {
            Warning("File %s not found in HOG", filename);
			return NULL;		// No file found
		}

        // Check the file isn't empty
		cfile = (CFILE *)malloc ( sizeof(CFILE) );
		if ( cfile == NULL ) {
            Warning("File %s empty", filename);
			fclose(fp);
			return NULL;
		}

		printf("Found HOGFile: %s\n", filename);

		// Initialize the data for the cfile
		cfile->file = fp;
		cfile->size = length;
		// Offset in the .hog file
		cfile->lib_offset = ftell( fp );
		cfile->raw_position = 0;
		return cfile;
	} else {
	    // Check the file isn't empty
		cfile = (CFILE *)malloc ( sizeof(CFILE) );
		if ( cfile == NULL ) {
            Warning("File %s in HOG is empty", filename);
			fclose(fp);
			return NULL;
		}

		printf("Found file: %s\n", filename);

		// Initialize the data for the cfile
		cfile->file = fp;
		cfile->size = filelength( fp );
		cfile->lib_offset = 0;
		cfile->raw_position = 0;
		return cfile;
	}
}

// Getter for the length of a file
int cfilelength( CFILE *fp )
{
	return fp->size;
}

int cfgetc( CFILE * fp )
{
	int c;

	if (fp->raw_position >= fp->size ) return EOF;

	fseek (fp->file, fp->lib_offset + fp->raw_position, SEEK_SET);
	c = getc( fp->file );
	if (c!=EOF)
		fp->raw_position++;

//	Assert( fp->raw_position==(ftell(fp->file)-fp->lib_offset) );

	return c;
}

char * cfgets( char * buf, size_t n, CFILE * fp )
{
	char * t = buf;
	size_t i;
	int c;

	for (i=0; i<n-1; i++ )	{
		do {
			if (fp->raw_position >= fp->size ) {
				*buf = 0;
				return NULL;
			}
			fseek (fp->file, fp->lib_offset + fp->raw_position, SEEK_SET);
			c = fgetc( fp->file );
			fp->raw_position++;
		} while ( c == 13 );
		*buf++ = c;
		if ( c=='\n' ) break;
	}
	*buf++ = 0;
	return  t;
}

size_t cfread( void * buf, size_t elsize, size_t nelem, CFILE * fp )
{
	int i;
	if ((fp->raw_position+(elsize*nelem)) > (size_t)fp->size ) return EOF;
	fseek (fp->file, fp->lib_offset + fp->raw_position, SEEK_SET);
	i = fread( buf, elsize, nelem, fp->file );
	fp->raw_position += i*elsize;
	return i;
}

int cftell( CFILE *fp )
{
	return fp->raw_position;
}

int cfseek( CFILE *fp, long int offset, int where )
{
	int c, goal_position;

	switch( where )	{
	case SEEK_SET:
		goal_position = offset;
		break;
	case SEEK_CUR:
		goal_position = fp->raw_position+offset;
		break;
	case SEEK_END:
		goal_position = fp->size+offset;
		break;
	default:
		return 1;
	}
	c = fseek( fp->file, fp->lib_offset + goal_position, SEEK_SET );
	fp->raw_position = ftell(fp->file)-fp->lib_offset;
	return c;
}

void cfclose( CFILE * fp )
{
	fclose(fp->file);
	free(fp);
	return;
}
