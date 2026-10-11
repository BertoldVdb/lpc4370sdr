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

#include "iqconverter_int16.h"
#include <stdlib.h>
#include <string.h>

#if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
  #include <malloc.h>
  #define _aligned_malloc __mingw_aligned_malloc
  #define _aligned_free  __mingw_aligned_free
  #define _inline inline
#elif defined(__APPLE__)
  #include <malloc/malloc.h>
  #define _aligned_malloc(size, alignment) malloc(size)
  #define _aligned_free(mem) free(mem)
  #define _inline inline
#elif defined(__FreeBSD__)
  #define _inline inline
  #define _aligned_free(mem) free(mem)
static void *_aligned_malloc(size_t size, size_t alignment);
#elif defined(__GNUC__) && !defined(__MINGW64_VERSION_MAJOR)
  #include <malloc.h>
  #define _aligned_malloc(size, alignment) memalign(alignment, size)
  #define _aligned_free(mem) free(mem)
  #define _inline inline
#endif

#define DEFAULT_ALIGNMENT 16

/* The FIR and the delay line work on blocks of FIR_BLOCK I/Q pairs */
#define FIR_BLOCK 128
#define FIR_GROUP 16

/* GCC and Clang vector extensions: SSE on x86, NEON on ARM, without intrinsics. On ARM, NEON
   intrinsics also split and merge the I/Q pairs around the FIR, eight pairs at a time. */
#if defined(__GNUC__) || defined(__clang__)
#define HAVE_V4SI
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAVE_NEON
typedef int32x4_t v4si;
#else
typedef int32_t v4si __attribute__((vector_size(16)));
static _inline void store4_int16(int32_t *p, v4si v) { memcpy(p, &v, sizeof(v)); }
#endif
static _inline v4si load4_int16(const int32_t *p) { v4si v; memcpy(&v, p, sizeof(v)); return v; }
#endif

static iqconverter_int16_t *iqconverter_int16_create(const int16_t *hb_kernel, int len)
{
	int i;
	size_t buffer_size;
	iqconverter_int16_t *cnv;

	/* A half-band kernel has an odd length, its centre tap in the middle */
	if (hb_kernel == NULL || len < 3 || (len & 1) == 0)
	{
		return NULL;
	}

	cnv = (iqconverter_int16_t *) _aligned_malloc(sizeof(iqconverter_int16_t), DEFAULT_ALIGNMENT);
	if (cnv == NULL)
	{
		return NULL;
	}

	cnv->len = len / 2 + 1;
	cnv->dc_removal = 1;

	buffer_size = cnv->len * sizeof(int32_t);

	cnv->fir_kernel = (int32_t *) _aligned_malloc(buffer_size, DEFAULT_ALIGNMENT);
	cnv->fir_queue = (int32_t *) _aligned_malloc((cnv->len - 1 + FIR_BLOCK) * sizeof(int32_t), DEFAULT_ALIGNMENT);
	cnv->delay_line = (int16_t *) _aligned_malloc((cnv->len / 2 + FIR_BLOCK) * sizeof(int16_t), DEFAULT_ALIGNMENT);
	if (cnv->fir_kernel == NULL || cnv->fir_queue == NULL || cnv->delay_line == NULL)
	{
		iqconverter_int16_free(cnv);
		return NULL;
	}

	iqconverter_int16_reset(cnv);

	for (i = 0; i < cnv->len; i++)
	{
		cnv->fir_kernel[i] = hb_kernel[i * 2];
	}

	return cnv;
}

static void iqconverter_int16_free(iqconverter_int16_t *cnv)
{
	if (cnv == NULL)
	{
		return;
	}
	if (cnv->fir_kernel != NULL)
		_aligned_free(cnv->fir_kernel);
	if (cnv->fir_queue != NULL)
		_aligned_free(cnv->fir_queue);
	if (cnv->delay_line != NULL)
		_aligned_free(cnv->delay_line);
	_aligned_free(cnv);
}

static void iqconverter_int16_reset(iqconverter_int16_t *cnv)
{
	cnv->fir_index = 0;
	cnv->delay_index = 0;
	cnv->old_x = 0;
	cnv->old_y = 0;
	cnv->old_e = 0;
	cnv->phase = 0;
	memset(cnv->delay_line, 0, (cnv->len / 2 + FIR_BLOCK) * sizeof(int16_t));
	memset(cnv->fir_queue, 0, (cnv->len - 1 + FIR_BLOCK) * sizeof(int32_t));
}

/*
 * The half-band filter: FIR on the even (I) samples, delay line on the odd (Q) samples.
 * fir_queue holds the last len - 1 I inputs in time order followed by the block being
 * filtered, delay_line the last len / 2 Q inputs followed by the block. Outputs are computed
 * FIR_GROUP at a time with every tap applied to the whole group, so the accumulators stay in
 * (vector) registers. Integer sums do not depend on their order: the results are the same as
 * one multiply-add chain per output.
 */
/* Every int16 the converter writes saturates. The high-pass after a full-scale input, the fs/4
   translation's negation of -32768 and the half-band filter's overshoot all exceed int16, and a
   plain cast wrapped them to the opposite rail: driven to its rails, the ADC's signal came out
   3 dB weaker, and 10 dB weaker 12 dB past them (a real sine near fs/4: -6.1 dBFS just below
   full scale, -9.4 at it, -15.8 at four times it), so nothing downstream could see the overload. */
static _inline int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

static void fir_delay_interleaved_int16(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	const int taps = cnv->len;
	const int half = cnv->len / 2;
	const int32_t *kernel = cnv->fir_kernel;
	int32_t *x = cnv->fir_queue;
	int16_t *q = cnv->delay_line;
	int n = len / 2;
	int done, i, k;

	for (done = 0; done < n; done += FIR_BLOCK)
	{
		int m = (n - done < FIR_BLOCK) ? n - done : FIR_BLOCK;
		int16_t *out = samples + 2 * done;

		i = 0;
#ifdef HAVE_NEON
		for (; i + 8 <= m; i += 8)
		{
			const int16x8x2_t iq = vld2q_s16(out + 2 * i);
			vst1q_s32(x + taps - 1 + i, vmovl_s16(vget_low_s16(iq.val[0])));
			vst1q_s32(x + taps + 3 + i, vmovl_s16(vget_high_s16(iq.val[0])));
			vst1q_s16(q + half + i, iq.val[1]);
		}
#endif
		for (; i < m; i++)
		{
			x[taps - 1 + i] = out[2 * i];
			q[half + i] = out[2 * i + 1];
		}
		/* The outputs: the FIR's in the I positions, the delayed Q samples in the Q positions */
		for (i = 0; i + FIR_GROUP <= m; i += FIR_GROUP)
		{
#ifdef HAVE_V4SI
			v4si a0 = { 0 }, a1 = { 0 }, a2 = { 0 }, a3 = { 0 };
			for (k = 0; k < taps; k++)
			{
				const int32_t h = kernel[k];
				const int32_t *xk = x + taps - 1 - k + i;
				a0 += h * load4_int16(xk);
				a1 += h * load4_int16(xk + 4);
				a2 += h * load4_int16(xk + 8);
				a3 += h * load4_int16(xk + 12);
			}
#endif
#ifdef HAVE_NEON
			/* vqshrn: sat16(acc >> 15) */
			int16x8x2_t iq;
			iq.val[0] = vcombine_s16(vqshrn_n_s32(a0, 15), vqshrn_n_s32(a1, 15));
			iq.val[1] = vld1q_s16(q + i);
			vst2q_s16(out + 2 * i, iq);
			iq.val[0] = vcombine_s16(vqshrn_n_s32(a2, 15), vqshrn_n_s32(a3, 15));
			iq.val[1] = vld1q_s16(q + i + 8);
			vst2q_s16(out + 2 * i + 16, iq);
#else
			int32_t acc[FIR_GROUP];
			int j;
#ifdef HAVE_V4SI
			store4_int16(acc, a0);
			store4_int16(acc + 4, a1);
			store4_int16(acc + 8, a2);
			store4_int16(acc + 12, a3);
#else
			memset(acc, 0, sizeof(acc));
			for (k = 0; k < taps; k++)
			{
				const int32_t h = kernel[k];
				const int32_t *xk = x + taps - 1 - k + i;
				for (j = 0; j < FIR_GROUP; j++)
				{
					acc[j] += h * xk[j];
				}
			}
#endif
			for (j = 0; j < FIR_GROUP; j++)
			{
				out[2 * (i + j)] = sat16(acc[j] >> 15);
				out[2 * (i + j) + 1] = q[i + j];
			}
#endif
		}
		for (; i < m; i++)
		{
			int32_t acc = 0;
			for (k = 0; k < taps; k++)
			{
				acc += kernel[k] * x[taps - 1 - k + i];
			}
			out[2 * i] = sat16(acc >> 15);
			out[2 * i + 1] = q[i];
		}
		memmove(x, x + m, (taps - 1) * sizeof(int32_t));
		memmove(q, q + m, half * sizeof(int16_t));
	}
}

/*
 * DC removal (a first-order high-pass with error feedback) and the fs/4 translation, which
 * multiplies the samples by -1, -1/2, 1, 1/2 in turn, in one pass. cnv->phase is where the
 * next block starts in that cycle, so blocks of any length join up.
 */
static _inline int16_t translate_fs_4(int32_t y, int phase)
{
	switch (phase & 3)
	{
	case 0: return sat16(-y);
	case 1: return sat16(-y >> 1);
	case 2: return sat16(y);
	default: return sat16(y >> 1);
	}
}

/*
 * The fs/4 translation alone, for a consumer that does not need the DC removal: the ADC's DC
 * lands at the edge of the I/Q band (-fs/4 of the I/Q rate), where nothing that only uses the
 * middle of the band looks. Bit for bit what the translation does after the high-pass, and
 * vectorizes: eight samples a step with NEON against one through the serial high-pass.
 */
static void translate_fs_4_only(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	int i;
	const int ph = cnv->phase;

	for (i = 0; i < len && ((ph + i) & 3) != 0; i++)
	{
		samples[i] = translate_fs_4(samples[i], ph + i);
	}
#ifdef HAVE_NEON
	{
		/* lanes 0 and 4: sat16(-x); 1 and 5: (-x) >> 1, exact through the halving subtract;
		   2 and 6: x; 3 and 7: x >> 1 */
		static const uint16_t k[4][8] = {
			{ 0xFFFF, 0, 0, 0, 0xFFFF, 0, 0, 0 },
			{ 0, 0xFFFF, 0, 0, 0, 0xFFFF, 0, 0 },
			{ 0, 0, 0xFFFF, 0, 0, 0, 0xFFFF, 0 },
			{ 0, 0, 0, 0xFFFF, 0, 0, 0, 0xFFFF } };
		const int16x8_t k0 = vreinterpretq_s16_u16(vld1q_u16(k[0]));
		const int16x8_t k1 = vreinterpretq_s16_u16(vld1q_u16(k[1]));
		const int16x8_t k2 = vreinterpretq_s16_u16(vld1q_u16(k[2]));
		const int16x8_t k3 = vreinterpretq_s16_u16(vld1q_u16(k[3]));
		const int16x8_t zero = vdupq_n_s16(0);
		for (; i + 8 <= len; i += 8)
		{
			const int16x8_t x = vld1q_s16(samples + i);
			int16x8_t r = vandq_s16(vqnegq_s16(x), k0);
			r = vorrq_s16(r, vandq_s16(vhsubq_s16(zero, x), k1));
			r = vorrq_s16(r, vandq_s16(x, k2));
			r = vorrq_s16(r, vandq_s16(vshrq_n_s16(x, 1), k3));
			vst1q_s16(samples + i, r);
		}
	}
#endif
	for (; i + 4 <= len; i += 4)
	{
		samples[i] = sat16(-(int32_t)samples[i]);
		samples[i + 1] = (int16_t)(-(int32_t)samples[i + 1] >> 1);
		samples[i + 3] = (int16_t)(samples[i + 3] >> 1);
	}
	for (; i < len; i++)
	{
		samples[i] = translate_fs_4(samples[i], ph + i);
	}
	cnv->phase = (ph + (len & 3)) & 3;
}

static void remove_dc_translate_fs_4_int16(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	int i;
	/* y reaches +-65536 (twice full scale), so y * 32100 needs 64 bits */
	int64_t u;
	int32_t x, y, w, s, old_x, old_y, old_e;
	const int ph = cnv->phase;

	if (!cnv->dc_removal)
	{
		translate_fs_4_only(cnv, samples, len);
		return;
	}

	old_x = cnv->old_x;
	old_y = cnv->old_y;
	old_e = cnv->old_e;

#define DC_STEP(k, out) \
		x = samples[i + (k)]; \
		w = x - old_x; \
		u = old_e + (int64_t) old_y * 32100; \
		s = (int32_t)(u >> 15); \
		y = w + s; \
		old_e = (int32_t)(u - (int64_t) s * 32768); \
		old_x = x; \
		old_y = y; \
		samples[i + (k)] = (out);

	/* One at a time up to the start of a cycle, then four at a time, then the tail */
	for (i = 0; i < len && ((ph + i) & 3) != 0; i++)
	{
		DC_STEP(0, translate_fs_4(y, ph + i))
	}
	for (; i + 4 <= len; i += 4)
	{
		DC_STEP(0, sat16(-y))
		DC_STEP(1, sat16(-y >> 1))
		DC_STEP(2, sat16(y))
		DC_STEP(3, sat16(y >> 1))
	}
	for (; i < len; i++)
	{
		DC_STEP(0, translate_fs_4(y, ph + i))
	}
#undef DC_STEP

	cnv->old_x = old_x;
	cnv->old_y = old_y;
	cnv->old_e = old_e;
	cnv->phase = (ph + (len & 3)) & 3;
}

/* The two halves of iqconverter_int16_process(), for running them on two threads: they use
   separate parts of the state */
static void iqconverter_int16_process_first(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	remove_dc_translate_fs_4_int16(cnv, samples, len);
}

static void iqconverter_int16_process_second(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	fir_delay_interleaved_int16(cnv, samples, len);
}

static void iqconverter_int16_process(iqconverter_int16_t *cnv, int16_t *samples, int len)
{
	remove_dc_translate_fs_4_int16(cnv, samples, len);
	fir_delay_interleaved_int16(cnv, samples, len);
}
