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
 * $Source: Smoke:miner:source:misc::RCS:fileutil.c $
 * $Revision: 1.6 $
 * $Author: allender $
 * $Date: 1995/10/30 11:09:51 $
 *
 * utilities for file manipulation
 *
 * $Log: fileutil.c $
 * Revision 1.6  1995/10/30  11:09:51  allender
 * use FILE, not CFILE on the write* routines
 *
 * Revision 1.5  1995/05/11  13:00:34  allender
 * added write functions which swap bytes
 *
 * Revision 1.4  1995/05/04  20:10:38  allender
 * remove include for fcntl
 *
 * Revision 1.3  1995/04/26  10:14:39  allender
 * added byteswap header file
 *
 * Revision 1.2  1995/04/26  10:13:21  allender
 *
 * Revision 1.1  1995/03/30  15:02:34  allender
 * Initial revision
 *
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dir.h>

#include "types.h"
#include "fileutil.h"
#include "cfile.h"
#include "fix.h"
#include "mem.h"

int filelength(FILE *fp)
{
	int cur_pos, size;

	cur_pos = ftell (fp);
	fseek(fp, 0, SEEK_END);
	size = ftell (fp);
	fseek(fp, cur_pos, SEEK_SET);
	return size;
}

byte read_byte(CFILE *fp)
{
	byte b;

	cfread(&b, sizeof(byte), 1, fp);
	return b;
}

short read_short(CFILE *fp)
{
	short s;

	cfread(&s, sizeof(short), 1, fp);
	return (s);
}

int read_int(CFILE *fp)
{
	uint i;

	cfread(&i, sizeof(uint), 1, fp);
	return i;
}

fix read_fix(CFILE *fp)
{
	fix f;

	cfread(&f, sizeof(fix), 1, fp);
	return f;
}

int write_byte(FILE *fp, byte b)
{
	return (fwrite(&b, sizeof(byte), 1, fp));
}

int write_short(FILE *fp, short s)
{
	return (fwrite(&s, sizeof(short), 1, fp));
}

int write_int(FILE *fp, int i)
{
	return (fwrite(&i,sizeof(int), 1, fp));
}

int write_fix(FILE *fp, fix f)
{
	return (fwrite(&f, sizeof(fix), 1, fp));
}

dir_t *dir_find (const char *ext)
{
	dir_t	*dir = malloc (sizeof (dir_t));

  // TODO: wew
	dir->dir = (struct DIR_ITER *)opendir ("sdmc:/3dscent/");
	strncpy (dir->ext, ext, 3);
	dir->ext[3] = 0;

	return dir;
}

const char *dir_findnext (dir_t *dir)
{
	static char	filename[256];
	struct stat	filestat;
	int		len;

	//while (!dirnext ((DIR_ITER *)dir->dir, filename, &filestat))
	//{
	//	if (!S_ISREG(filestat.st_mode))
	//		continue;
	//	len = strlen (filename);
  //  printf("dir_findnext: found file %s\n", filename);
	//	if (!strnicmp (filename + (len - 3), dir->ext, 3))
	//		return filename;
	//}
  //

  struct dirent* dp;

  while (1) {
    if ((dp = readdir((DIR_ITER *)dir->dir)) == 0)
      break;
    if (dp->d_name[0] == 0)
      break;
    if (stat(dp->d_name, &filestat) != 0)
      continue;
    if (!S_ISREG(filestat.st_mode))
      continue;
    len = strlen(dp->d_name);
    // need to fit in filename[14]
    if (len >= 14)
      continue;
    if (!strnicmp(dp->d_name + (len - 3), dir->ext, 3)) {
      return dp->d_name;
    }
  }
  printf("extension %s not found\n", dir->ext);


  printf("dir_findnext: no more files\n");
	return NULL;
}

void dir_close (dir_t *dir)
{
	closedir ((DIR_ITER *)dir->dir);
	free (dir);
}
