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
 * $Source: Smoke:miner:source:fix::RCS:fix.c $
 * $Revision: 1.7 $
 * $Author: allender $
 * $Date: 1995/09/22 14:08:16 $
 * 
 * C version of fixed point library
 * 
 * $Log: fix.c $
 * Revision 1.7  1995/09/22  14:08:16  allender
 * fixed fix_atan2 to work correctly with doubles
 *
 * Revision 1.6  1995/08/31  15:43:49  allender
 * *** empty log message ***
 *
 * Revision 1.5  1995/07/05  16:15:15  allender
 * make fixmuldiv use doubles for PPC implementation
 *
 * Revision 1.4  1995/05/15  13:57:36  allender
 * make fixmuldiv compile when compiling under 68k
 *
 * Revision 1.3  1995/05/11  13:02:59  allender
 * some routines are now in assembly
 *
 * Revision 1.2  1995/05/04  20:04:45  allender
 * use MPW fixdiv if compiling with MPW (why did I do this?)
 *
 * Revision 1.1  1995/04/17  11:37:54  allender
 * Initial revision
 *
 *
 * --- PC RCS Info ---
 * Revision 1.1  1995/03/08  18:55:09  matt
 * Initial revision
 * 
 * 
 */

/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: fix.c 1.7 1995/09/22 14:08:16 allender Exp $";
#pragma on (unreferenced)
*/
#include <stdlib.h>
#include <math.h>

#include "error.h"
#include "fix.h"

#include "ndsw.h"

//extern ubyte guess_table[];
extern short sincos_table[];
extern ushort asin_table[];
extern ushort acos_table[];
//extern fix isqrt_guess_table[];

//negate a quad
/*ITCM_CODE void fixquadnegate(quad *q)
{
	q->low  = 0 - q->low;
	q->high = 0 - q->high - (q->low != 0);
}
*/
//multiply two ints & add 64-bit result to 64-bit sum
/*
ITCM_CODE quad fixmulaccum(fix a,fix b)
{
	return (quad)(a * (quad)b);
}
*/
//extract a fix from a quad product
/*
ITCM_CODE fix fixquadadjust(quad q)
{
	return (fix)(q >> 16);
}
*/
/*
fix fixmul(fix a, fix b)
{
	return (fix)((a * (long long int)b) >> 16);
}
*/
//divide a quad by a fix, returning a fix
ITCM_CODE long fixdivquadlong(quad q,ulong d)
{
#if 1
	REG_DIVCNT = DIV_64_32;

	while(REG_DIVCNT & DIV_BUSY);

	REG_DIV_NUMER = q;
	REG_DIV_DENOM_L = d;

	while(REG_DIVCNT & DIV_BUSY);

	return (REG_DIV_RESULT_L);
#else
	int i;
	ulong tmp0;
	ubyte tmp1;
	ulong r;
	ubyte T,Q,M;

	r = 0;

	Q = ((nh&0x80000000)!=0);
	M = ((d&0x80000000)!=0);
	T = (M!=Q);

	if (M == 0)
	{
		for (i=0; i<32; i++ )   {
	
			r <<= 1;
			r |= T;
			T = ((nl&0x80000000L)!=0);
			nl <<= 1;
	
			switch( Q ) {
		
			case 0:
				Q = (unsigned char)((0x80000000L & nh) != 0 );
				nh = (nh << 1) | (unsigned long)T;

				tmp0 = nh;
				nh -= d;
				tmp1 = (nh>tmp0);
				if (Q == 0)
					Q = tmp1;
				else
					Q = (unsigned char)(tmp1 == 0);
				break;
			case 1:
				Q = (unsigned char)((0x80000000L & nh) != 0 );
				nh = (nh << 1) | (unsigned long)T;

				tmp0 = nh;
				nh += d;
				tmp1 = (nh<tmp0);
				if (Q == 0)
					Q = tmp1;
				else
					Q = (unsigned char)(tmp1 == 0);
				break;
			}
			T = (Q==M);
		}
	}
	else
	{
		for (i=0; i<32; i++ )   {
	
			r <<= 1;
			r |= T;
			T = ((nl&0x80000000L)!=0);
			nl <<= 1;
	
			switch( Q ) {
		
			case 0:
				Q = (unsigned char)((0x80000000L & nh) != 0 );
				nh = (nh << 1) | (unsigned long)T;

				tmp0 = nh;
				nh += d;
				tmp1 = (nh<tmp0);
				if (Q == 1)
					Q = tmp1;
				else
					Q = (unsigned char)(tmp1 == 0);
				break;
			case 1: 
				Q = (unsigned char)((0x80000000L & nh) != 0 );
				nh = (nh << 1) | (unsigned long)T;

				tmp0 = nh;
				nh = nh - d;
				tmp1 = (nh>tmp0);
				if (Q == 1)
					Q = tmp1;
				else
					Q = (unsigned char)(tmp1 == 0);
				break;
			}
			T = (Q==M);
		}
	}

	r = (r << 1) | T;

	return r;
#endif
}

ITCM_CODE fix fixdiv(fix a, fix b)
{
	REG_DIVCNT = DIV_64_32;

	while(REG_DIVCNT & DIV_BUSY);

	REG_DIV_NUMER = ((long long int)a) << 16;
	REG_DIV_DENOM_L = b;

	while(REG_DIVCNT & DIV_BUSY);

	return (REG_DIV_RESULT_L);
//	return fixdivquadlong(a<<16,a>>16,b);
}

ITCM_CODE fix fixmuldiv(fix a,fix b,fix c)
{
	REG_DIVCNT = DIV_64_32;

	while(REG_DIVCNT & DIV_BUSY);

	REG_DIV_NUMER = ((long long int)a * b);
	REG_DIV_DENOM_L = c;

	while(REG_DIVCNT & DIV_BUSY);

	return (REG_DIV_RESULT_L);
}
/*
//multiply two fixes, then divide by a third, return a fix
fix fixmuldiv(fix a,fix b,fix c)
{
	quad q;
	ulong t,old;
	int neg;
	ulong aa,bb;
	ulong ah,al,bh,bl;
 
	neg = ((a^b) < 0);

	aa = labs(a); bb = labs(b);

	ah = aa>>16;  al = aa&0xffff;
	bh = bb>>16;  bl = bb&0xffff;

	t = ah*bl + bh*al;

	q.high = 0;
	old = q.low = al*bl;
	q.low += (t<<16);
	if (q.low < old) q.high++;
	
	q.high += ah*bh + (t>>16);
	
	if (neg)
		fixquadnegate(&q);

	return fixdivquadlong(q.low,q.high,c);
}
//*/
ITCM_CODE fixang fix_atan2(fix cos,fix sin)
{
	quad q;
	fix m;
	fixang t;

	//Assert(!(cos==0 && sin==0));

	//find smaller of two

	q = fixmulaccum(sin,sin);
	q += fixmulaccum(cos,cos);

	m = quad_sqrt(q);

	if (m==0)
		return 0;

	if (labs(sin) < labs(cos)) {				//sin is smaller, use arcsin
		t = fix_asin(fixdiv(sin,m));
		if (cos<0)
			t = 0x8000 - t;
		return t;
	}
	else {
		t = fix_acos(fixdiv(cos,m));
		if (sin<0)
			t = -t;
		return t;
	}

}

//computes the square root of a quad, returning a long 
ITCM_CODE ulong quad_sqrt(quad q)
{
#if 1
	REG_SQRTCNT = SQRT_64;

	while(REG_SQRTCNT & SQRT_BUSY);

	REG_SQRT_PARAM = q;//((long long int)q->high << 32) | (unsigned)q->low;

	while(REG_SQRTCNT & SQRT_BUSY);

	return REG_SQRT_RESULT;
#else
	long cnt,r,old_r,t;
	quad tq;

	if (high & 0xff000000)
		cnt=12+16;
	else if (high & 0xff0000)
		cnt=8+16;
	else if (high & 0xff00)
		cnt=4+16;
	else
		cnt=0+16;
	
	r = guess_table[(high>>cnt)&0xff]<<cnt;

	//quad loop usually executed 4 times

	r = (fixdivquadlong(low,high,r)+r)/2;
	r = (fixdivquadlong(low,high,r)+r)/2;
	r = (fixdivquadlong(low,high,r)+r)/2;

	do {

		old_r = r;
		t = fixdivquadlong(low,high,r);

		if (t==r)	//got it!
			return r;
 
		r = (t+r)/2;

	} while (!(r==t || r==old_r));

	t = fixdivquadlong(low,high,r);
	tq.low=tq.high = 0;
	fixmulaccum(&tq,r,t);
	if (tq.low!=low || tq.high!=high)
		r++;
#endif
}

//computes the square root of a long, returning a short
ITCM_CODE ushort long_sqrt(long a)
{
#if 1
	REG_SQRTCNT = SQRT_32;

	while(REG_SQRTCNT & SQRT_BUSY);

	REG_SQRT_PARAM_L = a << 16;

	while(REG_SQRTCNT & SQRT_BUSY);

	return REG_SQRT_RESULT;
#else
	int cnt,r,old_r,t;

	if (a<=0)
		return 0;

	if (a & 0xff000000)
		cnt=12;
	else if (a & 0xff0000)
		cnt=8;
	else if (a & 0xff00)
		cnt=4;
	else
		cnt=0;
	
	r = guess_table[(a>>cnt)&0xff]<<cnt;

	//the loop nearly always executes 3 times, so we'll unroll it 2 times and
	//not do any checking until after the third time.  By my calcutations, the
	//loop is executed 2 times in 99.97% of cases, 3 times in 93.65% of cases, 
	//four times in 16.18% of cases, and five times in 0.44% of cases.  It never
	//executes more than five times.  By timing, I determined that is is faster
	//to always execute three times and not check for termination the first two
	//times through.  This means that in 93.65% of cases, we save 6 cmp/jcc pairs,
	//and in 6.35% of cases we do an extra divide.  In real life, these numbers
	//might not be the same.

	r = ((a/r)+r)/2;
	r = ((a/r)+r)/2;

	do {

		old_r = r;
		t = a/r;

		if (t==r)	//got it!
			return r;
 
		r = (t+r)/2;

	} while (!(r==t || r==old_r));

	if (a % r)
		r++;

	return r;
#endif
}

//computes the square root of a fix, returning a fix
/*
ITCM_CODE fix fix_sqrt(fix a)
{
	return ((fix) long_sqrt (a)) << 8;
}
*/
//compute sine and cosine of an angle, filling in the variables
//either of the pointers can be NULL
//with interpolation
ITCM_CODE void fix_sincos(fix a,fix *s,fix *c)
{
	int i,f;
	fix ss,cc;

	i = (a>>8)&0xff;
	f = a&0xff;

	ss = sincos_table[i];
	*s = (ss + (((sincos_table[i+1] - ss) * f)>>8))<<2;

	cc = sincos_table[i+64];
	*c = (cc + (((sincos_table[i+64+1] - cc) * f)>>8))<<2;

}

//compute sine and cosine of an angle, filling in the variables
//either of the pointers can be NULL
//no interpolation
ITCM_CODE void fix_fastsincos(fix a,fix *s,fix *c)
{
	int i;

	i = (a>>8)&0xff;

	*s = sincos_table[i] << 2;
	*c = sincos_table[i+64] << 2;
}

//compute inverse sine
ITCM_CODE fixang fix_asin(fix v)
{
	fix vv;
	int i,f,aa;

	vv = labs(v);

	if (vv >= f1_0)		//check for out of range
		return 0x4000;

	i = (vv>>8)&0xff;
	f = vv&0xff;

	aa = asin_table[i];
	aa = aa + (((asin_table[i+1] - aa) * f)>>8);

	if (v < 0)
		aa = -aa;

	return aa;
}

//compute inverse cosine
ITCM_CODE fixang fix_acos(fix v)
{
	fix vv;
	int i,f,aa;

	vv = labs(v);

	if (vv >= f1_0)		//check for out of range
		return 0;

	i = (vv>>8)&0xff;
	f = vv&0xff;

	aa = acos_table[i];
	aa = aa + (((acos_table[i+1] - aa) * f)>>8);

	if (v < 0)
		aa = 0x8000 - aa;

	return aa;
}
/*
#define TABLE_SIZE 1024

//for passed value a, returns 1/sqrt(a) 
fix fix_isqrt( fix a )
{
	int i, b = a;
	int cnt = 0;
	int r;

	if ( a == 0 ) return 0;

	while( b >= TABLE_SIZE )	{
		b >>= 1;
		cnt++;
	}

	//printf( "Count = %d (%d>>%d)\n", cnt, b, (cnt+1)/2 );
	r = isqrt_guess_table[b] >> ((cnt+1)/2);

	//printf( "Initial r = %d\n", r );

	for (i=0; i<3; i++ )	{
		int old_r = r;
		r = fixmul( ( (3*65536) - fixmul(fixmul(r,r),a) ), r) / 2;
		//printf( "r %d  = %d\n", i, r );
		if ( old_r >= r ) return (r+old_r)/2;
	}

	return r;	
}
*/
