/*
This program is distributed under the terms of the 'MIT license'. The text
of this licence follows...

Copyright (c) 2005 J.D.Medhurst (a.k.a. Tixy)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

static const unsigned short IMA_ADPCMStepTable[89] =
{
		7,	  8,	9,	 10,   11,	 12,   13,	 14,
	   16,	 17,   19,	 21,   23,	 25,   28,	 31,
	   34,	 37,   41,	 45,   50,	 55,   60,	 66,
	   73,	 80,   88,	 97,  107,	118,  130,	143,
	  157,	173,  190,	209,  230,	253,  279,	307,
	  337,	371,  408,	449,  494,	544,  598,	658,
	  724,	796,  876,	963, 1060, 1166, 1282, 1411,
	 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	 7132, 7845, 8630, 9493,10442,11487,12635,13899,
	15289,16818,18500,20350,22385,24623,27086,29794,
	32767
};

static const short IMA_ADPCMIndexTable[8] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
};

void adpcm_coder (char *src, unsigned char *dst, int srcSize)
{
	unsigned bitOffset = 0;
	// calculate end of input buffer
	const char *end = ((const char *)src + srcSize);
	int delta, stepIndex = 0;
	int predictedValue;

	// make sure srcSize represents a whole number of samples
	srcSize &= ~1;

	predictedValue = (((*src++) - 0x80) << 8);
	delta = (((*src++) - 0x80) << 8) - predictedValue;
	if (delta < 0)
		delta = - delta;
	if (delta > 32767)
		delta = 32767;
	while (IMA_ADPCMStepTable[stepIndex] < (unsigned)delta)
		stepIndex++;

	*((short *)dst) = predictedValue; dst += 2;
	*dst++ = stepIndex;
	*dst++ = 0;

	while (src < end)
	{
		// encode a pcm value from input buffer
		int delta = (((*src++) - 0x80) << 8) - predictedValue;
		unsigned value;
		if (delta >= 0)
			value = 0;
		else
		{
			value = 8;
			delta = -delta;
		}

		int step = IMA_ADPCMStepTable[stepIndex];
		int diff = step >> 3;
		if (delta > step)
		{
			value |= 4;
			delta -= step;
			diff += step;
		}
		step >>= 1;
		if (delta > step)
		{
			value |= 2;
			delta -= step;
			diff += step;
		}
		step >>= 1;
		if (delta > step)
		{
			value |= 1;
			diff += step;
		}

		if (value & 8)
			predictedValue -= diff;
		else
			predictedValue += diff;
		if (predictedValue < -0x8000)
			predictedValue = -0x8000;
		else if (predictedValue > 0x7fff)
			predictedValue = 0x7fff;
//		PredictedValue = predictedValue;

		stepIndex += IMA_ADPCMIndexTable[value & 7];
		if (stepIndex < 0)
			stepIndex = 0;
		else if (stepIndex > 88)
			stepIndex = 88;
//		StepIndex = stepIndex;

		// pick which nibble to write adpcm value to...
		if (!bitOffset)
			*dst = value;		// write adpcm value to low nibble
		else
		{
			unsigned char b = *dst;		// get byte from ouput
			b &= 0x0f;			// clear bits of high nibble
			b |= value << 4;		// or adpcm value into the high nibble
			*dst++ = (unsigned char)b;	// write value back to output and move on to next byte
		}

		// toggle which nibble in byte to write to next
		bitOffset ^= 4;
	}
}
