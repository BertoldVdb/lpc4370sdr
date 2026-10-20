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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <lpc4370sdr.h>

/* A 32-bit number (0x... hex, 0... octal); all of them are checked before the device is opened */
static int parse_num(const char* s, uint32_t* value)
{
	char* end;
	unsigned long long v;
	if (strchr(s, '-') != NULL) /* strtoull() would wrap it around */
		return -1;
	errno = 0;
	v = strtoull(s, &end, 0);
	if (end == s || *end != 0 || errno != 0 || v > UINT32_MAX)
		return -1;
	*value = (uint32_t)v;
	return 0;
}

static uint32_t num(const char* s) { uint32_t v = 0; parse_num(s, &v); return v; }

static int usage(void)
{
	fprintf(stderr, "usage: lpc4370sdr_debug read <addr> [words] | bytes <addr> [count] | write <addr> <word>... | call <core> <addr> [r0 r1 r2 r3]\n");
	return 1;
}

int main(int argc, char** argv)
{
	struct lpc4370sdr_device* device = NULL;
	int result, ret = 0;
	int a;

	if (argc < 3)
		return usage();
	for (a = 2; a < argc; a++)
	{
		uint32_t v;
		if (parse_num(argv[a], &v) != 0) { fprintf(stderr, "not a 32-bit number: %s\n", argv[a]); return usage(); }
	}
	if ((strcmp(argv[1], "read") == 0 || strcmp(argv[1], "bytes") == 0) && argc > 3)
	{
		/* the bytes from addr on, inside the 32-bit address space */
		uint64_t bytes = (uint64_t)num(argv[3]) * (strcmp(argv[1], "read") == 0 ? 4 : 1);
		if (bytes > UINT32_MAX - num(argv[2])) { fprintf(stderr, "%s runs past the end of the address space\n", argv[3]); return usage(); }
	}
	result = lpc4370sdr_init();
	if (result != LPC4370SDR_SUCCESS) { fprintf(stderr, "lpc4370sdr_init() failed: %s\n", lpc4370sdr_error_name(result)); return 1; }
	result = lpc4370sdr_open(&device);
	if (result != LPC4370SDR_SUCCESS) { fprintf(stderr, "lpc4370sdr_open() failed: %s\n", lpc4370sdr_error_name(result)); return 1; }

	if (strcmp(argv[1], "read") == 0 || strcmp(argv[1], "bytes") == 0)
	{
		int words = strcmp(argv[1], "read") == 0;
		uint32_t addr = num(argv[2]);
		uint32_t count = argc > 3 ? num(argv[3]) : 1;
		uint32_t total = words ? count * 4 : count;
		uint32_t done = 0;
		uint8_t buf[64];
		while (done < total)
		{
			uint32_t n = total - done > 64 ? 64 : total - done;
			result = lpc4370sdr_mem_read(device, addr + done, buf, (uint16_t)n);
			if (result != LPC4370SDR_SUCCESS) { fprintf(stderr, "read at 0x%08x failed: %s\n", addr + done, lpc4370sdr_error_name(result)); ret = 1; break; }
			if (words)
			{
				uint32_t i;
				for (i = 0; i < n; i += 4)
				{
					uint32_t w; memcpy(&w, &buf[i], 4);
					if (((done + i) % 16) == 0) printf("%s%08x:", (done + i) ? "\n" : "", addr + done + i);
					printf(" %08x", w);
				}
			}
			else
			{
				uint32_t i;
				for (i = 0; i < n; i++)
				{
					if (((done + i) % 16) == 0) printf("%s%08x:", (done + i) ? "\n" : "", addr + done + i);
					printf(" %02x", buf[i]);
				}
			}
			done += n;
		}
		printf("\n");
	}
	else if (strcmp(argv[1], "write") == 0)
	{
		uint32_t addr = num(argv[2]);
		int i;
		for (i = 3; i < argc; i++)
		{
			uint32_t w = num(argv[i]);
			result = lpc4370sdr_mem_write(device, addr, &w, 4);
			if (result != LPC4370SDR_SUCCESS) { fprintf(stderr, "write at 0x%08x failed: %s\n", addr, lpc4370sdr_error_name(result)); ret = 1; break; }
			addr += 4;
		}
	}
	else if (strcmp(argv[1], "call") == 0 && argc >= 4)
	{
		uint32_t args[4] = { 0, 0, 0, 0 };
		uint32_t r0 = 0;
		int i;
		for (i = 0; i < 4 && argc > 4 + i; i++)
			args[i] = num(argv[4 + i]);
		if (num(argv[3]) < 0x1000) { fprintf(stderr, "refusing to call address 0x%08x\n", num(argv[3])); lpc4370sdr_close(device); lpc4370sdr_exit(); return 1; }
		result = lpc4370sdr_call(device, num(argv[2]), num(argv[3]), args, &r0, 2000);
		if (result != LPC4370SDR_SUCCESS) { fprintf(stderr, "call failed: %s\n", lpc4370sdr_error_name(result)); ret = 1; }
		else printf("r0 = 0x%08x (%u)\n", r0, r0);
	}
	else
		ret = usage();

	lpc4370sdr_close(device);
	lpc4370sdr_exit();
	return ret;
}
