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
#include <stdlib.h>
#include <stdarg.h>

#include <ctype.h>
#include "types.h"
#include "fix.h"

int min( int a, int b )
{
	if ( a < b )
		return a;
	else
		return b;
}

int max( int a, int b )
{
	if ( a > b )
		return a;
	else
		return b;
}

/*int random (void)
{
	return (rand () & 0x7fff);
}*/

void string_tolower(char s1[])
{
    for (int i = 0; s1[i]; i++)
    {
        s1[i] = tolower(s1[i]);
    }
}

int strnicmp(char *s1, char *s2, int n)
{
    string_tolower(*s1);
    string_tolower(*s1);
    return strncmp(*s1, *s2, n);
}

int stricmp(char *s1, char *s2)
{
    string_tolower(*s1);
    string_tolower(*s1);
    return strcmp(*s1, *s2);
}
