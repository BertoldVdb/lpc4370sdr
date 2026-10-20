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
#include <stdbool.h>
#include <getopt.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
	#ifdef _MSC_VER
		#ifdef _WIN64
		typedef int64_t ssize_t;
		#else
		typedef int32_t ssize_t;
		#endif
	#endif

#define sleep(a) Sleep( (a*1000) )
#endif

/* 8 Mbit flash */
#define MAX_LENGTH 0x100000
/* The device's erase command (LPC4370SDR_SPIFLASH_ERASE) erases the first 64 KB sector only: a
   write must stay inside it, flash that was not erased would end up with old AND new bits. The
   sector is read first and written back whole, with the file at its address. The calibration
   (0x20000, lpc4370sdr_calibrate) lies outside it */
#define WRITE_MAX_END 0x10000
/* Tries of the first write after the erase, each up to the library's 500 ms control timeout */
#define ERASE_WAIT_TRIES 8
/* Where a failed write leaves what the sector was to hold */
#define SECTOR_SAVE_PATH "lpc4370sdr_spiflash_sector0.bin"

static struct option long_options[] = {
	{ "address", required_argument, 0, 'a' },
	{ "length", required_argument, 0, 'l' },
	{ "read", required_argument, 0, 'r' },
	{ "write", required_argument, 0, 'w' },
	{ 0, 0, 0, 0 },
};

int parse_u32(char* s, uint32_t* const value)
{
	char* s_end;
	uint_fast8_t base = 10;
	uint32_t u32_value;
	unsigned long long u64_value;

	if (strlen(s) > 2) {
		if (s[0] == '0')  {
			if ((s[1] == 'x') || (s[1] == 'X')) {
				base = 16;
				s += 2;
			} else if ((s[1] == 'b') || (s[1] == 'B')) {
				base = 2;
				s += 2;
			}
		}
	}

	/* strtoull() takes a sign, and wraps a negative value around */
	if (strchr(s, '-') != NULL) {
		return LPC4370SDR_ERROR_INVALID_PARAM;
	}
	s_end = s;
	errno = 0;
	u64_value = strtoull(s, &s_end, base);
	u32_value = (uint32_t)u64_value;
	if ((s != s_end) && (*s_end == 0) && (errno == 0) && u64_value <= UINT32_MAX) {
		*value = u32_value;
		return LPC4370SDR_SUCCESS;
	} else {
		return LPC4370SDR_ERROR_INVALID_PARAM;
	}
}

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

/* The sector is erased but not (all) written: keep what it was to hold, to write it with -w */
static void save_sector(const uint8_t* sector)
{
	FILE* f = fopen(SECTOR_SAVE_PATH, "wb");
	if (f != NULL && fwrite(sector, 1, WRITE_MAX_END, f) == WRITE_MAX_END && fclose(f) == 0) {
		fprintf(stderr, "The 1st 64KB of the flash may be erased: what they were to hold is in %s, write it with -w %s\n",
			SECTOR_SAVE_PATH, SECTOR_SAVE_PATH);
	} else {
		if (f != NULL)
			fclose(f);
		fprintf(stderr, "The 1st 64KB of the flash may be erased, and saving what they were to hold to %s failed\n", SECTOR_SAVE_PATH);
	}
}

static void usage()
{
	printf("Usage:\n");
	printf("\t-a, --address <n>: starting address (default: 0)\n");
	printf("\t-l, --length <n>: number of bytes to read (default: 0)\n");
	printf("\t-r <filename>: Read data into file (SPIFI@0x80000000).\n");
	printf("\t-w <filename>: Write data from file at the address, in the first 64 KB of the flash only (the sector the device erases:\n");
	printf("\t\tthe rest of it is read and written back), then verify the sector.\n");
	printf("\t[-s serial_number_64bits]: Open board with specified 64bits serial number.\n");
}

bool serial_number = false;
uint64_t serial_number_val;

int main(int argc, char** argv)
{
	int opt;
	uint32_t address = 0;
	uint32_t length = 0;
	uint32_t tmp_length;
	uint16_t xfer_len = 0;
	const char* path = NULL;
	struct lpc4370sdr_device* device = NULL;
	int result = LPC4370SDR_SUCCESS;
	int option_index = 0;
	static uint8_t data[MAX_LENGTH];
	uint8_t* pdata = &data[0];
	FILE* fd = NULL;
	bool read = false;
	bool write = false;
	uint32_t serial_number_msb_val;
	uint32_t serial_number_lsb_val;

	while ((opt = getopt_long(argc, argv, "a:l:r:w:s:", long_options, &option_index)) != EOF)
	{
		switch (opt) {
		case 'a':
			result = parse_u32(optarg, &address);
			break;

		case 'l':
			result = parse_u32(optarg, &length);
			break;

		case 'r':
			read = true;
			path = optarg;
			break;

		case 'w':
			write = true;
			path = optarg;
			break;

		case 's':
			serial_number = true;
			result = parse_u64(optarg, &serial_number_val);
			if (result != LPC4370SDR_SUCCESS) {
				break;
			}
			serial_number_msb_val = (uint32_t)(serial_number_val >> 32);
			serial_number_lsb_val = (uint32_t)(serial_number_val & 0xFFFFFFFF);
			printf("Board serial number to open: 0x%08X%08X\n", serial_number_msb_val, serial_number_lsb_val);
			break;

		default:
			fprintf(stderr, "opt error: %d\n", opt);
			usage();
			return EXIT_FAILURE;
		}

		if (result != LPC4370SDR_SUCCESS) {
			fprintf(stderr, "argument error: %s (%d)\n", lpc4370sdr_error_name(result), result);
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
	
	if (path == NULL) {
		fprintf(stderr, "Specify a path to a file.\n");
		usage();
		return EXIT_FAILURE;
	} 
	
	if( write )
	{
		fd = fopen(path, "rb");
		if(fd == NULL)
		{
			printf("Error to open file %s\n", path);
			return EXIT_FAILURE;
		}
		/* Get size of the file  */
		{
			long size;
			fseek(fd, 0, SEEK_END); /* Not really portable but work on major OS Linux/Win32 */
			size = ftell(fd);
			/* Move to start */
			rewind(fd);
			if (size < 0 || size > MAX_LENGTH)
			{
				fprintf(stderr, "File %s is too large for the flash.\n", path);
				fclose(fd);
				return EXIT_FAILURE;
			}
			length = (uint32_t)size;
		}
		printf("File size %u bytes.\n", length);
	}

	if (length == 0) {
		fprintf(stderr, "Requested transfer of zero bytes.\n");
		if(fd != NULL)
			fclose(fd);
		usage();
		return EXIT_FAILURE;
	}

	if(length > MAX_LENGTH || address > MAX_LENGTH - length)
	{
		fprintf(stderr, "Request exceeds size of flash memory.\n");
		fprintf(stderr, "address=0x%08X size=%d Bytes.\n", address, length);
		if(fd != NULL)
			fclose(fd);
		usage();
		return EXIT_FAILURE;
	}

	if (write && address + length > WRITE_MAX_END)
	{
		fprintf(stderr, "Write exceeds the first 64 KB of the flash, the only sector the device erases.\n");
		fprintf(stderr, "address=0x%08X size=%u Bytes.\n", address, length);
		fclose(fd);
		return EXIT_FAILURE;
	}

	if (read) {
		fd = fopen(path, "wb");
		if(fd == NULL)
		{
			printf("Error to open file %s\n", path);
			return EXIT_FAILURE;
		}
	}

	if (fd == NULL) {
		fprintf(stderr, "Failed to open file: %s\n", path);
		return EXIT_FAILURE;
	}

	result = lpc4370sdr_init();
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "lpc4370sdr_init() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
		fclose(fd);
		return EXIT_FAILURE;
	}

	if(serial_number == true)
	{
		result = lpc4370sdr_open_sn(&device, serial_number_val);
		if( result != LPC4370SDR_SUCCESS ) {
			printf("lpc4370sdr_open_sn() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
			usage();
			fclose(fd);
			return EXIT_FAILURE;
		}
	}else
	{
		result = lpc4370sdr_open(&device);
		if( result != LPC4370SDR_SUCCESS ) {
			printf("lpc4370sdr_open() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
			usage();
			fclose(fd);
			return EXIT_FAILURE;
		}
	}

	if (read) 
	{
		ssize_t bytes_written;
		tmp_length = length;
		while (tmp_length) 
		{
			xfer_len = (tmp_length > 256) ? 256 : tmp_length;
			printf("Reading %d bytes from 0x%06x.\n", xfer_len, address);
			result = lpc4370sdr_spiflash_read(device, address, xfer_len, pdata);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_spiflash_read() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
				fclose(fd);
				return EXIT_FAILURE;
			}     
			address += xfer_len;
			pdata += xfer_len;
			tmp_length -= xfer_len;
		}
		bytes_written = fwrite(data, 1, length, fd);
		if (bytes_written != (ssize_t)length) {
			fprintf(stderr, "Failed write to file (wrote %d bytes).\n", (int)bytes_written);
			fclose(fd);
			return EXIT_FAILURE;
		}
	} else
	{
		/* Write Firmware: the whole sector, the file merged into what the flash holds */
		const uint32_t start = address;
		static uint8_t sector[WRITE_MAX_END];
		static uint8_t check[WRITE_MAX_END];
		ssize_t bytes_read = fread(data, 1, length, fd);
		if (bytes_read != (ssize_t)length) {
			fprintf(stderr, "Failed read file (read %d bytes).\n", (int)bytes_read);
			fclose(fd);
			return EXIT_FAILURE;
		}
		printf("Reading the 1st 64KB in SPI flash (the sector the device erases).\n");
		for (address = 0; address < WRITE_MAX_END; address += 256)
		{
			result = lpc4370sdr_spiflash_read(device, address, 256, &sector[address]);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_spiflash_read() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
				fclose(fd);
				return EXIT_FAILURE;
			}
		}
		memcpy(&sector[start], data, length);
		printf("Erasing 1st 64KB in SPI flash.\n");
		result = lpc4370sdr_spiflash_erase(device);
		if (result != LPC4370SDR_SUCCESS) {
			fprintf(stderr, "lpc4370sdr_spiflash_erase() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
			save_sector(sector);
			fclose(fd);
			return EXIT_FAILURE;
		}
		printf("Writing 64KB (the file's %u bytes at 0x%06x).\n", length, start);
		for (address = 0; address < WRITE_MAX_END; address += 256)
		{
			/* The device acknowledges the erase before the flash has done it (typically 150 ms,
			   1 s and more at worst): the first write waits for it and may time out. Writing a
			   page again with the same data does no harm */
			int tries = (address == 0) ? ERASE_WAIT_TRIES : 1;
			do {
				result = lpc4370sdr_spiflash_write(device, address, 256, &sector[address]);
			} while (result != LPC4370SDR_SUCCESS && --tries > 0);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_spiflash_write() at 0x%06x failed: %s (%d)\n", address, lpc4370sdr_error_name(result), result);
				save_sector(sector);
				fclose(fd);
				return EXIT_FAILURE;
			}
		}
		/* Read it back */
		printf("Verifying 64KB.\n");
		for (address = 0; address < WRITE_MAX_END; address += 256)
		{
			result = lpc4370sdr_spiflash_read(device, address, 256, &check[address]);
			if (result != LPC4370SDR_SUCCESS) {
				fprintf(stderr, "lpc4370sdr_spiflash_read() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
				save_sector(sector);
				fclose(fd);
				return EXIT_FAILURE;
			}
		}
		if (memcmp(check, sector, WRITE_MAX_END) != 0) {
			uint32_t i;
			for (i = 0; i < WRITE_MAX_END && check[i] == sector[i]; i++);
			fprintf(stderr, "Verify failed: the flash differs from what was written at 0x%06x.\n", i);
			save_sector(sector);
			fclose(fd);
			return EXIT_FAILURE;
		}
		printf("Verify OK.\n");
	}

	result = lpc4370sdr_close(device);
	if (result != LPC4370SDR_SUCCESS) {
		fprintf(stderr, "lpc4370sdr_close() failed: %s (%d)\n", lpc4370sdr_error_name(result), result);
		fclose(fd);
		return EXIT_FAILURE;
	}

	lpc4370sdr_exit();

	if (fd != NULL) {
		fclose(fd);
	}

	return EXIT_SUCCESS;
}
