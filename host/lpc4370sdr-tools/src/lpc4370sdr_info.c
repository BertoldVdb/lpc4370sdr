/*
 * Copyright 2012 Jared Boone <jared@sharebrained.com>
 * Copyright 2013 Michael Ossmann <mike@ossmann.com>
 * Copyright 2013/2014 Benjamin Vernoux <bvernoux@airspy.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from AirSpy, based on the HackRF project).
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

#include <lpc4370sdr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <stdbool.h>

#define LPC4370SDR_MAX_DEVICE (32)
char version[255 + 1];
lpc4370sdr_read_partid_serialno_t read_partid_serialno;
struct lpc4370sdr_device* devices[LPC4370SDR_MAX_DEVICE+1] = { NULL };

int parse_u64(char* s, uint64_t* const value) {
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t u64_value;

	if( strlen(s) > 2 ) {
		if( s[0] == '0' ) {
			if( (s[1] == 'x') || (s[1] == 'X') ) {
				base = 16;
				s += 2;
			} else if( (s[1] == 'b') || (s[1] == 'B') ) {
				base = 2;
				s += 2;
			}
		}
	}

	/* strtoull() takes a sign, and wraps a negative value around */
	if( strchr(s, '-') != NULL ) {
		return LPC4370SDR_ERROR_INVALID_PARAM;
	}
	s_end = s;
	errno = 0;
	u64_value = strtoull(s, &s_end, base);
	if( (s != s_end) && (*s_end == 0) && (errno == 0) ) {
		*value = u64_value;
		return LPC4370SDR_SUCCESS;
	} else {
		return LPC4370SDR_ERROR_INVALID_PARAM;
	}
}

static void usage(void)
{
	printf("Usage:\n");
	printf("\t[-s serial_number_64bits]: Open board with specified 64bits serial number.\n");
}

bool serial_number = false;
uint64_t serial_number_val;

int main(int argc, char** argv)
{
	int i;
	uint32_t j;
	int result;
	int opt;
	uint32_t count;
	uint32_t *samplerates;
	uint32_t serial_number_msb_val;
	uint32_t serial_number_lsb_val;
	lpc4370sdr_lib_version_t lib_version;
	uint8_t board_id = LPC4370SDR_BOARD_ID_INVALID;

	while( (opt = getopt(argc, argv, "s:")) != EOF )
	{
		result = LPC4370SDR_SUCCESS;
		switch( opt ) 
		{
		case 's':
			serial_number = true;
			result = parse_u64(optarg, &serial_number_val);
			if( result != LPC4370SDR_SUCCESS ) {
				break;
			}
			serial_number_msb_val = (uint32_t)(serial_number_val >> 32);
			serial_number_lsb_val = (uint32_t)(serial_number_val & 0xFFFFFFFF);
			printf("Board serial number to open: 0x%08X%08X\n", serial_number_msb_val, serial_number_lsb_val);
			break;

		default:
			printf("unknown argument '-%c %s'\n", opt, optarg ? optarg : "");
			usage();
			return EXIT_FAILURE;
		}
		
		if( result != LPC4370SDR_SUCCESS ) {
			printf("argument error: '-%c %s' %s (%d)\n", opt, optarg, lpc4370sdr_error_name(result), result);
			usage();
			return EXIT_FAILURE;
		}		
	}

	result = lpc4370sdr_init();
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "lpc4370sdr_init() failed: %s (%d)\n",
				lpc4370sdr_error_name(result), result);
		return EXIT_FAILURE;
	}

	lpc4370sdr_lib_version(&lib_version);
	printf("lpc4370sdr_lib_version: %d.%d.%d\n", 
					lib_version.major_version, lib_version.minor_version, lib_version.revision); 

	for (i = 0; i < LPC4370SDR_MAX_DEVICE; i++)
	{
		if(serial_number == true)
		{
			result = lpc4370sdr_open_sn(&devices[i], serial_number_val);
		}else
		{
			result = lpc4370sdr_open(&devices[i]);
		}
		if (result != LPC4370SDR_SUCCESS)
		{
			if(i == 0)
			{
				fprintf(stderr, "lpc4370sdr_open() board %d failed: %s (%d)\n",
						i+1, lpc4370sdr_error_name(result), result);
			}
			break;
		}
	}

	for(i = 0; i < LPC4370SDR_MAX_DEVICE; i++)
	{
		if(devices[i] != NULL)
		{
			printf("\nFound board %d\n", i + 1);
			fflush(stdout);
			result = lpc4370sdr_board_id_read(devices[i], &board_id);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_board_id_read() failed: %s (%d)\n",
					lpc4370sdr_error_name(result), result);
				continue;
			}
			printf("Board ID Number: %d (%s)\n", board_id,
				lpc4370sdr_board_id_name(board_id));

			result = lpc4370sdr_version_string_read(devices[i], &version[0], 255);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_version_string_read() failed: %s (%d)\n",
					lpc4370sdr_error_name(result), result);
				continue;
			}
			printf("Firmware Version: %s\n", version);
			{
				lpc4370sdr_calibration_t cal;
				if (lpc4370sdr_get_calibration(devices[i], &cal) == LPC4370SDR_SUCCESS)
				{
					const char* src = cal.source == LPC4370SDR_CALIBRATION_SOURCE_FLASH ? "flash" : cal.source == LPC4370SDR_CALIBRATION_SOURCE_HOST ? "host" : "none";
					printf("Calibration: %d ppb (%s)\n", cal.correction_ppb, src);
				}
			}

			result = lpc4370sdr_board_partid_serialno_read(devices[i], &read_partid_serialno);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_board_partid_serialno_read() failed: %s (%d)\n",
					lpc4370sdr_error_name(result), result);
				continue;
			}
			printf("Part ID Number: 0x%08X 0x%08X\n",
				read_partid_serialno.part_id[0],
				read_partid_serialno.part_id[1]);
			printf("Serial Number: 0x%08X%08X\n",
				read_partid_serialno.serial_no[2],
				read_partid_serialno.serial_no[3]);
			{
				uint8_t busnum, devnum;
				char port[32];
				if (lpc4370sdr_usb_position(devices[i], &busnum, &devnum, port, sizeof(port)) == LPC4370SDR_SUCCESS)
					printf("USB: bus %03u device %03u, port %s\n", busnum, devnum, port);
			}

			printf("Supported sample rates:\n");
			lpc4370sdr_get_samplerates(devices[i], &count, 0);
			samplerates = (uint32_t *) malloc(count * sizeof(uint32_t));
			lpc4370sdr_get_samplerates(devices[i], samplerates, count);
			for (j = 0; j < count; j++)
			{
				printf("\t%f MSPS\n", samplerates[j] * 0.000001f);
			}
			free(samplerates);

			printf("Close board %d\n", i+1);
			result = lpc4370sdr_close(devices[i]);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_close() board %d failed: %s (%d)\n",
						i+1, lpc4370sdr_error_name(result), result);
				continue;
			}
		}
	}

	lpc4370sdr_exit();
	return EXIT_SUCCESS;
}
