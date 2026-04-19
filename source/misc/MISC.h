#ifndef __MISC_H__
#define __MISC_H__

#define random() (rand() & 0x7FFF)

int min( int a, int b );
int max( int a, int b );

//int random (void);

void strrev( char *s1 );

void string_tolower( char *s1 );
int strnicmp(char *s1, char *s2, int n);
int stricmp(char *s1, char *s2);

#endif
