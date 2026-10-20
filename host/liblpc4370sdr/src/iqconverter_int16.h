/*
Copyright (C) 2014, Youssef Touil <youssef@airspy.com>

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef IQCONVERTER_INT16_H
#define IQCONVERTER_INT16_H

#include <stdint.h>

typedef struct {
	int len;
	int fir_index;
	int delay_index;
	/* int32: the high-pass output of a full-scale input reaches twice full scale, and int16
	   state wrapped there - an ADC driven to its rails came out weaker, not clipped */
	int32_t old_x;
	int32_t old_y;
	int32_t old_e;
	int phase; /* position of the next sample in the fs/4 translation's cycle of four */
	int dc_removal; /* 1 (the default): high-pass before the translation; 0: translation alone */
	int odd; /* the FIR and delay line's next sample is the Q sample of a pair */
	int32_t *fir_kernel;
	int32_t *fir_queue;
	int16_t *delay_line;
} iqconverter_int16_t;

/* Built into lpc4370sdr.c, static. The libairspy compatibility library builds them in once more
   with these four exported (IQCONVERTER_API empty), as libairspy exports them. */
#ifndef IQCONVERTER_API
#define IQCONVERTER_API static
#endif

IQCONVERTER_API iqconverter_int16_t *iqconverter_int16_create(const int16_t *hb_kernel, int len);
IQCONVERTER_API void iqconverter_int16_free(iqconverter_int16_t *cnv);
IQCONVERTER_API void iqconverter_int16_reset(iqconverter_int16_t *cnv);
IQCONVERTER_API void iqconverter_int16_process(iqconverter_int16_t *cnv, int16_t *samples, int len);
static void iqconverter_int16_process_first(iqconverter_int16_t *cnv, int16_t *samples, int len);
static void iqconverter_int16_process_second(iqconverter_int16_t *cnv, int16_t *samples, int len);

#endif // IQCONVERTER_INT16_H
