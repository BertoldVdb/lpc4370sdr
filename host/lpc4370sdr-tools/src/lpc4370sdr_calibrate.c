/*
 * Copyright 2024 Benjamin Vernoux <bvernoux@gmail.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from AirSpy).
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
#include <getopt.h>
#include <sys/types.h>
#include <time.h>

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>

#define LPC4370SDR_FLASH_CALIB_OFFSET (0x20000) /* After 128KB (Reserved for Firmware + 64KB Spare) */
#define LPC4370SDR_FLASH_CALIB_HEADER (0xCA1B0001)

typedef struct
{
	uint32_t header; /* Shall be equal to LPC4370SDR_FLASH_CALIB_HEADER */
	uint32_t timestamp; /* Epoch Unix Time Stamp */
	int32_t correction_ppb;
} lpc4370sdr_calib_t;

static void usage()
{
	printf("Usage:\n");
	printf("\t-r: Read and display calibration data.\n");
	printf("\t-w <calibration in ppb>: Erase and Write calibration in ppb.\n");
}

int main(int argc, char **argv)
{
	int opt;
	struct lpc4370sdr_device* device = NULL;
	int result;
	bool read = false;
	bool write = false;
	int32_t calibration_ppb = 0;
	lpc4370sdr_calib_t calib;

	while ((opt = getopt(argc, argv, "rw:")) != EOF)
	{
		switch (opt) {
		case 'r':
			read = true;
			break;

		case 'w':
			{
				char* end;
				long ppb;
				write = true;
				errno = 0;
				ppb = strtol(optarg, &end, 10);
				if (end == optarg || *end != 0 || errno != 0 || ppb < INT32_MIN || ppb > INT32_MAX) {
					fprintf(stderr, "argument error: -w %s is not a calibration in ppb\n", optarg);
					usage();
					return EXIT_FAILURE;
				}
				calibration_ppb = (int32_t)ppb;
			}
			break;

		default:
			fprintf(stderr, "opt error: %d\n", opt);
			usage();
			return EXIT_FAILURE;
		}
	}

	if (write == read) {
		if (write == true) {
			fprintf(stderr, "Read and write options are mutually exclusive.\n");
		} else {
			fprintf(stderr, "Specify either read or write option.\n");
		}
		usage();
		return EXIT_FAILURE;
	}

	result = lpc4370sdr_init();
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "lpc4370sdr_init() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
		return EXIT_FAILURE;
	}

	result = lpc4370sdr_open(&device);
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "Failed to open lpc4370sdr device.\n");
		return 1;
	}

	if (read) {
		printf("Reading %d bytes from 0x%06x.\n", (int)sizeof(calib), LPC4370SDR_FLASH_CALIB_OFFSET);
		result = lpc4370sdr_spiflash_read(device, LPC4370SDR_FLASH_CALIB_OFFSET, (int)sizeof(calib), (uint8_t *)&calib);
		if (result != LPC4370SDR_SUCCESS) {
			fprintf(stderr, "lpc4370sdr_spiflash_read() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
			return EXIT_FAILURE;
		}
		if (calib.header != LPC4370SDR_FLASH_CALIB_HEADER) {
			printf("No calibration in the flash (header 0x%08x).\n", calib.header);
		} else {
			time_t epoch_time = calib.timestamp;
			struct tm *local_time = localtime(&epoch_time);
			if (local_time != NULL)
				printf("Calibration timestamp: %04d/%02d/%02d %02d:%02d:%02d\n",
					local_time->tm_year + 1900,
					local_time->tm_mon + 1,
					local_time->tm_mday,
					local_time->tm_hour,
					local_time->tm_min,
					local_time->tm_sec);
			printf("Calibration correction in ppb: %d\n", calib.correction_ppb);
		}
	}
	if(write) {
		printf("Erasing sector 2 (calibration) in SPI flash.\n");
		result = lpc4370sdr_spiflash_erase_sector(device, 2);
		if (result != LPC4370SDR_SUCCESS) {
			fprintf(stderr, "Failed to erase sector 2.\n");
			return 1;
		}

		calib.header = LPC4370SDR_FLASH_CALIB_HEADER;
		calib.timestamp = (uint32_t)time(NULL);
		calib.correction_ppb = calibration_ppb;

		printf("Writing calibration %d bytes at 0x%06x.\n", (int)sizeof(calib), LPC4370SDR_FLASH_CALIB_OFFSET);
		time_t epoch_time = calib.timestamp;
		struct tm *local_time = localtime(&epoch_time);
		printf("Calibration timestamp: %04d/%02d/%02d %02d:%02d:%02d\nCalibration correction in ppb: %d\n", 
				local_time->tm_year + 1900,
				local_time->tm_mon + 1,
				local_time->tm_mday,
				local_time->tm_hour,
				local_time->tm_min,
				local_time->tm_sec,
				calib.correction_ppb);
		result = lpc4370sdr_spiflash_write(device, LPC4370SDR_FLASH_CALIB_OFFSET, sizeof(calib), (uint8_t *)&calib);
		if (result != LPC4370SDR_SUCCESS) {
			fprintf(stderr, "Failed to write calibration data.\n");
			return 1;
		}
	}

	result = lpc4370sdr_close(device);
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "lpc4370sdr_close() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
		return EXIT_FAILURE;
	}

	lpc4370sdr_exit();

	return EXIT_SUCCESS;
}

