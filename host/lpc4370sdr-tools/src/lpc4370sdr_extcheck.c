/*
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org>
 *
 * This file is part of lpc4370sdr.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * lpc4370sdr_extcheck: streams from one board and checks what the chunk headers carry, chunk by
 * chunk, against what they must say:
 *  - the extension areas: with -T (firmware built with LPC4370SDR_EXT_TEST) every byte of every
 *    chunk's area against lpc4370sdr_ext_test_fill(); else the stream health items
 *    (LPC4370SDR_EXT_HEALTH, every LPC4370SDR_EXT_HEALTH_EVERY chunks);
 *  - the chunk sequence the areas show (with -T every chunk has one but every 89th): each area's
 *    sample index is its chunk index times the chunk's samples, and every chunk missing from the
 *    sequence is one the device or the library counted as lost (lost, unaccounted, corrupt: an
 *    area goes with its chunk's samples);
 *  - the counters the header sends modulo 2^16, as the library extends them: never backwards
 *    within a stream; PPS count steps of 1; SOF count steps of 1 with the frame number moving by
 *    the divider each; with -T they cross their 16-bit wrap within seconds.
 * -l ms:s sleeps ms in the callback every s seconds (the device then loses chunks), -R s switches
 * the sample rate between -r and -x every s seconds (the stream restarts). Exit status 0 when every
 * check held.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <lpc4370sdr.h>

static volatile int stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int test_pattern, stall_ms, stall_every_s;
static uint32_t sof_divider, chunk_samples;
static double t0;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* per stream (a restart starts over) */
static struct {
	uint32_t restarts;
	int have_chunk;
	uint64_t last_chunk;
	int have_counts;
	uint32_t lost, overrun, unacc, corrupt;
	uint32_t pps, sof_count, sof_frame;
	uint32_t lost0, unacc0, corrupt0; /* at the stream's first area */
	uint64_t missing;                 /* chunks missing from the sequence */
	uint64_t either;                  /* empty-pattern chunks inside a gap: there or lost, no telling */
	int loss_since_pps;               /* chunks lost since the last pps step: an edge may be among them */
	/* the losses counted up to the block before the last one with areas, and up to that block:
	   the chunks missing up to the last area lie between the two (later losses show in no gap) */
	uint64_t counted_lo, counted_hi;
	int restarted;                    /* this stream began with a restart: its first block also
	                                     counts the old stream's last losses */
} st;

/* chunks the device and the library counted as lost from the stream's first area on, with the
   counters at lost, unacc, corrupt; with -T the device's counter jumps by the test offset at its
   first loss */
static uint64_t counted_lost_at(uint32_t lost, uint32_t unacc, uint32_t corrupt)
{
	uint64_t l = lost - st.lost0;
	if (test_pattern && st.lost0 == 0 && lost > 0)
		l -= 65530u;
	return l + (unacc - st.unacc0) + (corrupt - st.corrupt0);
}

static struct {
	uint64_t blocks, areas, areas_ok, items, health, empty_expected;
	uint64_t pattern_bad, index_bad, seq_back, missing, either, counted_lo, counted_hi, ext_dropped, ext_malformed;
	uint64_t count_back, pps_steps, pps_bad, sof_steps, sof_bad, restarts;
	uint32_t pps_max, sof_count_max, sof_frame_max, lost_max;
	int pps_wrapped, sof_wrapped, lost_wrapped, frame_wrapped;
	uint64_t stalls;
	uint64_t chunk_min, chunk_max; /* the chunk indexes the areas showed (64-bit, as extended) */
	uint32_t corrupt, ext_len_max;
	int chunk_seen;
} tot;

static double next_stall;

static void fail(const char* what, uint64_t chunk)
{
	static int printed;
	if (printed++ < 20)
		fprintf(stderr, "FAIL at %.1f s, chunk %llu: %s\n", now_s() - t0, (unsigned long long)chunk, what);
}

static int callback(lpc4370sdr_transfer* t)
{
	lpc4370sdr_transfer_metadata_t m;
	uint32_t i;

	if (lpc4370sdr_transfer_get_metadata(t, &m) != LPC4370SDR_SUCCESS)
		return 0;
	tot.blocks++;
	if (m.stream_restarts != st.restarts)
	{
		/* settle the stream that ended, start over */
		tot.missing += st.missing;
		tot.either += st.either;
		tot.counted_lo += st.counted_lo;
		tot.counted_hi += st.counted_hi;
		memset(&st, 0, sizeof(st));
		st.restarts = m.stream_restarts;
		st.restarted = 1;
		tot.restarts++;
	}
	tot.ext_dropped += m.ext_dropped;
	tot.corrupt = m.corrupt_chunks;
	if (m.ext_len > tot.ext_len_max) tot.ext_len_max = m.ext_len;
	tot.ext_malformed = m.ext_malformed;

	/* the counters as the block before this one left them */
	const uint32_t prev_lost = st.lost, prev_unacc = st.unacc, prev_corrupt = st.corrupt;

	/* counters: never back within a stream */
	if (st.have_counts && (m.lost_chunks != st.lost || m.unaccounted_chunks != st.unacc || m.gap_samples))
		st.loss_since_pps = 1;
	if (st.have_counts)
	{
		if (m.lost_chunks < st.lost || m.overrun_chunks < st.overrun || m.pps_count < st.pps ||
			m.sof_count < st.sof_count || m.sof_frame < st.sof_frame)
		{
			tot.count_back++;
			fail("a counter went backwards", m.sample_index);
		}
		if (m.pps_count != st.pps)
		{
			tot.pps_steps++;
			if (m.pps_count - st.pps != 1 && st.pps != 0 && !st.loss_since_pps)
			{
				tot.pps_bad++;
				fail("pps count step not 1 without a loss", m.pps_count);
			}
			st.loss_since_pps = 0;
		}
		if (m.sof_count != st.sof_count && st.sof_count != 0)
		{
			const uint32_t dc = m.sof_count - st.sof_count, df = m.sof_frame - st.sof_frame;
			tot.sof_steps++;
			if (sof_divider && df != dc * sof_divider)
			{
				tot.sof_bad++;
				fail("sof frame step is not the divider times the count step", m.sof_count);
			}
		}
	}
	st.have_counts = 1;
	st.lost = m.lost_chunks;
	st.overrun = m.overrun_chunks;
	st.unacc = m.unaccounted_chunks;
	st.corrupt = m.corrupt_chunks;
	st.pps = m.pps_count;
	st.sof_count = m.sof_count;
	st.sof_frame = m.sof_frame;
	if (m.pps_count > tot.pps_max) tot.pps_max = m.pps_count;
	if (m.sof_count > tot.sof_count_max) tot.sof_count_max = m.sof_count;
	if (m.sof_frame > tot.sof_frame_max) tot.sof_frame_max = m.sof_frame;
	if (m.lost_chunks > tot.lost_max) tot.lost_max = m.lost_chunks;
	if (m.pps_count > 0xFFFF) tot.pps_wrapped = 1;
	if (m.sof_count > 0xFFFF) tot.sof_wrapped = 1;
	if (m.sof_frame > 0xFFFF) tot.frame_wrapped = 1;
	if (m.lost_chunks > 0xFFFF) tot.lost_wrapped = 1;

	for (i = 0; i < m.ext_len; i++)
	{
		const lpc4370sdr_ext_area_t* e = &m.ext[i];
		uint32_t pos = 0;
		uint8_t type, len;
		const uint8_t* data;
		int r;

		tot.areas++;
		if (!tot.chunk_seen || e->chunk_index < tot.chunk_min) tot.chunk_min = e->chunk_index;
		if (!tot.chunk_seen || e->chunk_index > tot.chunk_max) tot.chunk_max = e->chunk_index;
		tot.chunk_seen = 1;
		if (chunk_samples && e->sample_index != e->chunk_index * chunk_samples)
		{
			tot.index_bad++;
			fail("sample index is not chunk index times chunk samples", e->chunk_index);
		}
		/* the sequence: chunks between two areas are empty ones (-T) or missing */
		if (!st.have_chunk)
		{
			/* from the block before this one: its own losses lie before its first area or between
			   its areas - unless the stream began with a restart in this block, whose counts then
			   hold the old stream's last losses too */
			st.lost0 = st.restarted ? m.lost_chunks : prev_lost;
			st.unacc0 = st.restarted ? m.unaccounted_chunks : prev_unacc;
			st.corrupt0 = st.restarted ? m.corrupt_chunks : prev_corrupt;
		}
		else if (e->chunk_index <= st.last_chunk)
		{
			tot.seq_back++;
			fail("chunk index not after the last area's", e->chunk_index);
		}
		else
		{
			uint64_t c;
			const int alone = e->chunk_index == st.last_chunk + 2; /* one chunk between two areas */
			for (c = st.last_chunk + 1; c < e->chunk_index; c++)
			{
				if (test_pattern && (uint32_t)c % 89u == 0 && alone)
					tot.empty_expected++;   /* alone between two areas: there, without one */
				else if (test_pattern && (uint32_t)c % 89u == 0)
					st.either++;            /* inside a gap: there or lost, no telling */
				else if (test_pattern)
					st.missing++;
				else if ((uint32_t)c % LPC4370SDR_EXT_HEALTH_EVERY == 0)
					st.missing++;
			}
		}
		st.have_chunk = 1;
		st.last_chunk = e->chunk_index;

		if (test_pattern)
		{
			uint8_t want[LPC4370SDR_FRAME_EXT_BYTES];
			lpc4370sdr_ext_test_fill(want, (uint32_t)e->chunk_index);
			if (memcmp(want, e->data, sizeof(want)) != 0)
			{
				tot.pattern_bad++;
				fail("extension area differs from the test pattern", e->chunk_index);
			}
			else
				tot.areas_ok++;
		}
		while ((r = lpc4370sdr_ext_next(e->data, &pos, &type, &data, &len)) > 0)
		{
			tot.items++;
			if (type == LPC4370SDR_EXT_HEALTH)
			{
				tot.health++;
				if (len != 16 || (uint32_t)e->chunk_index % LPC4370SDR_EXT_HEALTH_EVERY != 0)
					fail("health item of the wrong size or on the wrong chunk", e->chunk_index);
			}
			else if (!test_pattern || type < LPC4370SDR_EXT_TEST_FIRST)
				fail("unknown item type", e->chunk_index);
		}
		if (r < 0)
			fail("malformed extension area", e->chunk_index);
		if (!test_pattern)
			tot.areas_ok++;
	}

	if (m.ext_len && st.have_chunk)
	{
		st.counted_lo = counted_lost_at(prev_lost, prev_unacc, prev_corrupt);
		st.counted_hi = counted_lost_at(m.lost_chunks, m.unaccounted_chunks, m.corrupt_chunks);
		if (st.counted_lo > st.counted_hi) st.counted_lo = 0;   /* the first block: none before it */
	}

	if (stall_ms && now_s() >= next_stall)
	{
		next_stall = now_s() + stall_every_s;
		tot.stalls++;
		usleep(stall_ms * 1000);
	}
	return 0;
}

int main(int argc, char** argv)
{
	struct lpc4370sdr_device* d = NULL;
	uint64_t serial = 0;
	uint32_t rate = 0, rate2 = 0, baud = 0;
	int packing = 1, seconds = 30, restart_every = 0, opt, failed;
	double next_restart = 0, next_print;
	int which = 0;

	while ((opt = getopt(argc, argv, "s:r:x:p:t:TS:B:l:R:")) != -1)
	{
		switch (opt)
		{
		case 's': serial = strtoull(optarg, NULL, 16); break;
		case 'r': rate = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'x': rate2 = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': packing = atoi(optarg); break;
		case 't': seconds = atoi(optarg); break;
		case 'T': test_pattern = 1; break;
		case 'S': sof_divider = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'B': baud = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'l': stall_ms = atoi(optarg); stall_every_s = strchr(optarg, ':') ? atoi(strchr(optarg, ':') + 1) : 5; break;
		case 'R': restart_every = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: lpc4370sdr_extcheck -s serial [-r rate] [-x rate2 -R every_s] [-p packing] [-t s] [-T] "
				"[-S sof_divider] [-B uart_baud] [-l stall_ms:every_s]\n");
			return 2;
		}
	}
	if (lpc4370sdr_open_sn(&d, serial) != LPC4370SDR_SUCCESS) { fprintf(stderr, "open failed\n"); return 2; }
	if (lpc4370sdr_set_sample_type(d, LPC4370SDR_SAMPLE_RAW) != LPC4370SDR_SUCCESS ||
		lpc4370sdr_set_packing(d, (uint8_t)packing) != LPC4370SDR_SUCCESS ||
		(rate && lpc4370sdr_set_samplerate(d, rate) != LPC4370SDR_SUCCESS) ||
		lpc4370sdr_set_freq(d, 159450000) != LPC4370SDR_SUCCESS ||
		(sof_divider && lpc4370sdr_set_sof_divider(d, sof_divider) != LPC4370SDR_SUCCESS) ||
		(baud && lpc4370sdr_set_uart_baud(d, baud) != LPC4370SDR_SUCCESS))
	{
		fprintf(stderr, "setup failed\n");
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	t0 = now_s();
	next_stall = t0 + stall_every_s;
	next_restart = t0 + restart_every;
	next_print = t0 + 10;
	if (lpc4370sdr_start_rx(d, callback, NULL) != LPC4370SDR_SUCCESS) { fprintf(stderr, "start failed\n"); return 2; }
	{
		lpc4370sdr_stream_status_t s;
		usleep(200000);
		if (lpc4370sdr_get_stream_status(d, &s) == LPC4370SDR_SUCCESS)
			chunk_samples = s.chunk_samples;
	}
	while (!stop && now_s() - t0 < seconds && lpc4370sdr_is_streaming(d) == LPC4370SDR_TRUE)
	{
		usleep(100000);
		if (restart_every && rate2 && now_s() >= next_restart)
		{
			next_restart = now_s() + restart_every;
			which = !which;
			if (lpc4370sdr_set_samplerate(d, which ? rate2 : rate) != LPC4370SDR_SUCCESS)
				fprintf(stderr, "samplerate switch failed\n");
		}
		if (now_s() >= next_print)
		{
			next_print += 10;
			fprintf(stderr, "%.0f s: %llu blocks, %llu areas (%llu bad), %llu items, %llu health, pps %u, sof %u frame %u, "
				"lost %u, restarts %llu, corrupt %u, areas dropped %llu, per block max %u\n", now_s() - t0, (unsigned long long)tot.blocks,
				(unsigned long long)tot.areas, (unsigned long long)tot.pattern_bad, (unsigned long long)tot.items,
				(unsigned long long)tot.health, tot.pps_max, tot.sof_count_max, tot.sof_frame_max, tot.lost_max,
				(unsigned long long)tot.restarts, tot.corrupt, (unsigned long long)tot.ext_dropped, tot.ext_len_max);
		}
	}
	lpc4370sdr_stop_rx(d);
	tot.missing += st.missing;
	tot.either += st.either;
	tot.counted_lo += st.counted_lo;
	tot.counted_hi += st.counted_hi;
	lpc4370sdr_close(d);

	failed = tot.pattern_bad || tot.index_bad || tot.seq_back || tot.count_back || tot.pps_bad || tot.sof_bad ||
		tot.ext_dropped || tot.ext_malformed || tot.areas == 0 ||
		(test_pattern && (tot.counted_hi < tot.missing || tot.counted_lo > tot.missing + tot.either));
	printf("%s: %llu blocks, %llu areas (%llu as expected, %llu not, %llu empty ones skipped), %llu items, %llu health; "
		"chunks missing %llu (+%llu either), counted lost %llu..%llu; index errors %llu, sequence back %llu, counters back %llu; "
		"pps %llu steps (%llu bad, max %u%s), sof %llu steps (%llu bad, count max %u%s, frame max %u%s); "
		"lost max %u%s; corrupt %u; areas per block max %u, dropped %llu, malformed %llu; restarts %llu, stalls %llu; chunks %#llx..%#llx%s\n",
		failed ? "FAIL" : "PASS", (unsigned long long)tot.blocks, (unsigned long long)tot.areas,
		(unsigned long long)tot.areas_ok, (unsigned long long)tot.pattern_bad, (unsigned long long)tot.empty_expected,
		(unsigned long long)tot.items, (unsigned long long)tot.health, (unsigned long long)tot.missing,
		(unsigned long long)tot.either, (unsigned long long)tot.counted_lo, (unsigned long long)tot.counted_hi, (unsigned long long)tot.index_bad, (unsigned long long)tot.seq_back,
		(unsigned long long)tot.count_back, (unsigned long long)tot.pps_steps, (unsigned long long)tot.pps_bad,
		tot.pps_max, tot.pps_wrapped ? " wrapped" : "", (unsigned long long)tot.sof_steps,
		(unsigned long long)tot.sof_bad, tot.sof_count_max, tot.sof_wrapped ? " wrapped" : "", tot.sof_frame_max,
		tot.frame_wrapped ? " wrapped" : "", tot.lost_max, tot.lost_wrapped ? " wrapped" : "", tot.corrupt, tot.ext_len_max,
		(unsigned long long)tot.ext_dropped, (unsigned long long)tot.ext_malformed,
		(unsigned long long)tot.restarts, (unsigned long long)tot.stalls,
		(unsigned long long)tot.chunk_min, (unsigned long long)tot.chunk_max,
		tot.chunk_max > 0xFFFFFFFFull ? " (past the 32-bit wrap)" : "");
	return failed ? 1 : 0;
}
