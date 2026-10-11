/*
Copyright (c) 2013, Michael Ossmann <mike@ossmann.com>
Copyright (c) 2012, Jared Boone <jared@sharebrained.com>
Copyright (c) 2014, Youssef Touil <youssef@airspy.com>
Copyright (c) 2014, Benjamin Vernoux <bvernoux@airspy.com>
Copyright (c) 2015, Ian Gilmour <ian@sdrsharp.com>
Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

		Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
		Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the
		documentation and/or other materials provided with the distribution.
		Neither the name of AirSpy nor the names of its contributors may be used to endorse or promote products derived from this software
		without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifndef _WIN32
#include <unistd.h>
#include <time.h>
#endif
#include <libusb.h>

#if _MSC_VER > 1700  // To avoid error with Visual Studio 2017/2019 or more define which define timespec as it is already defined in pthread.h
#define HAVE_STRUCT_TIMESPEC
#endif

#include <pthread.h>

#include "lpc4370sdr.h"
/* The I/Q converters are compiled into this file so that none of their
   symbols leave the library. */
#include "iqconverter_float.c"
#include "iqconverter_int16.c"
#include "filters.h"

#define PACKET_SIZE (12)
#define UNPACKED_SIZE (16)
#define RAW_BUFFER_COUNT (8)

#ifdef LPC4370SDR_BIG_ENDIAN
#define TO_LE(x) __builtin_bswap32(x)
#define TO_LE16(x) __builtin_bswap16(x)
#else
#define TO_LE(x) (x)
#define TO_LE16(x) (x)
#endif

#define SAMPLE_RESOLUTION 12
#define SAMPLE_ENCAPSULATION 16

#define SAMPLE_SHIFT (SAMPLE_ENCAPSULATION - SAMPLE_RESOLUTION)
#define SAMPLE_SCALE (1.0f / (1 << (15 - SAMPLE_SHIFT)))

#define SERIAL_NUMBER_UNUSED (0ULL)
#define FILE_DESCRIPTOR_UNUSED (-1)

#define USB_PRODUCT_ID (2)
#define STR_DESCRIPTOR_SIZE (250)

#define MIN_SAMPLERATE_BY_VALUE (1000000)
#define SAMPLE_TYPE_IS_IQ(x) ((x) == LPC4370SDR_SAMPLE_FLOAT32_IQ || (x) == LPC4370SDR_SAMPLE_INT16_IQ)

#define LIBUSB_CTRL_TIMEOUT_MS (500)
/* A 64 KB sector erase takes up to a few hundred ms (2 s worst case) before the device answers */
#define SPIFLASH_ERASE_TIMEOUT_MS (3000)

/* Chunk sizes of the previous buffer's end the deframer keeps: a chunk held back for the next
   buffer can start more than one chunk size before the end */
#define FRAME_PREV_CHUNKS (2)

typedef struct {
	uint32_t freq_hz;
} set_freq_params_t;


/*
 * DSP thread: with IQ output the consumer thread does the first half of the IQ conversion
 * and hands each block over in a slot; the DSP thread does the second half and calls the
 * application. All blocks go through the slots while it runs, so they keep their order.
 */
#define DSP_SLOTS (4)

typedef struct
{
	void *buf;
	int sample_count; /* real samples, as converted */
	uint32_t dropped_buffers;
	enum lpc4370sdr_sample_type sample_type;
	bool meta_valid;
	lpc4370sdr_transfer_metadata_t meta;
} dsp_slot_t;

typedef struct lpc4370sdr_device
{
	libusb_context* usb_context;
	libusb_device_handle* usb_device;
	struct libusb_transfer** transfers;
	lpc4370sdr_sample_block_cb_fn callback;
	volatile bool streaming;
	volatile bool stop_requested;
	pthread_t transfer_thread;
	pthread_t consumer_thread;
	bool transfer_thread_running;
	bool consumer_thread_running;
	long transfers_in_flight; /* submitted transfers libusb has not given back: in_flight_*() */
	pthread_cond_t consumer_cv;
	pthread_mutex_t consumer_mp;
	bool dsp_thread_enabled; /* lpc4370sdr_set_dsp_thread(), on unless LPC4370SDR_NO_DSP_THREAD is set */
	bool dsp_active; /* this stream runs the DSP thread */
	pthread_t dsp_thread;
	bool dsp_thread_running;
	pthread_cond_t dsp_cv;
	pthread_mutex_t dsp_mp;
	dsp_slot_t dsp_slots[DSP_SLOTS];
	int dsp_head;
	int dsp_tail;
	int dsp_count;
	const lpc4370sdr_transfer_metadata_t *cb_meta; /* metadata of the block in the callback */
	pthread_t deferred_join; /* the IO thread that stopped its own stream: joined by the next call from another thread */
	pthread_mutex_t state_mp; /* starting and stopping the stream, see lock_state() */
	bool deferred_join_pending;
	bool cb_meta_valid;
	uint32_t supported_samplerate_count;
	uint32_t *supported_samplerates;
	uint32_t transfer_count;
	uint32_t buffer_size;
	uint32_t dropped_buffers;
	uint32_t dropped_buffers_queue[RAW_BUFFER_COUNT];
	uint16_t *received_samples_queue[RAW_BUFFER_COUNT];
	volatile int received_samples_queue_head;
	volatile int received_samples_queue_tail;
	volatile int received_buffer_count;
	void *output_buffer;
	uint16_t *unpacked_samples;
	bool packing_enabled;
	uint8_t packing_mode; /* 0 = 16-bit, 1 = 12-bit packed, 3 = 8-bit */
	bool dev_mem_buffers;
	bool usb_buf_slow; /* the zero-copy buffers were slow to read (the stream then uses ordinary ones) */
	bool dc_removal; /* the I/Q converters' DC removal (lpc4370sdr_set_dc_removal) */
	bool framing_requested;
	bool framing_active;
	volatile bool gap_fill; /* lpc4370sdr_set_gap_fill() */
	uint32_t fill_max; /* samples gap fill may add to a block: the buffers have room for them */
	uint32_t sof_count_seen; /* last SOF tag count deframed: a change means a new tag */
	uint32_t pps_count_seen; /* last PPS count deframed: a change means a new edge */
	/* the header's wrapping fields, extended (valid: a value seen since the stream started) */
	uint64_t chunk_ext; bool chunk_ext_valid;
	uint32_t lost_ext, overrun_ext, pps_ext, sof_count_ext, sof_frame_ext;
	bool counts_ext_valid, sof_frame_ext_valid;
	bool version_noted; /* a header of another version was reported */
	uint64_t uart_total; /* UART bytes delivered since lpc4370sdr_start_rx() */
	uint32_t sof_divider; /* requested SOF tagging divider, sent at every lpc4370sdr_start_rx() */
	uint8_t *framed_samples; /* deframed samples (uint16_t, 12-bit scale), or payloads for LPC4370SDR_SAMPLE_RAW */
	uint32_t block_capacity; /* samples framed_samples, output_buffer and the DSP slots have room for */
	uint8_t *frame_prev; /* framed: the last FRAME_PREV_CHUNKS chunk sizes of bytes of the previous buffer, then room to put together a chunk that starts in them */
	uint32_t frame_prev_len; /* 0 when the previous buffer was not kept */
	uint32_t frame_carry_len; /* bytes at the end of frame_prev that start a chunk (after a resync) */
	uint32_t last_chunk_index; /* of the last valid chunk header, with the device's lost count then */
	uint32_t last_lost_chunks;
	bool chunk_index_valid;
	uint32_t stream_start_count; /* of the last valid chunk header: a change means the device restarted the stream */
	bool stream_start_valid;
	bool meta_pending; /* the UART bytes and SOF tags in meta have not reached the application yet */
	bool frame_tail; /* the last valid chunk header had LPC4370SDR_FRAME_FLAG_TAIL */
	bool freq_64bit; /* the firmware takes a 64-bit frequency (HydraSDR firmware) */
	bool rf_port_select; /* the board has antenna inputs to select (HydraSDR RFOne) */
	lpc4370sdr_transfer_metadata_t meta;
	bool meta_valid;
	uint64_t expected_sample_index;
	bool expected_sample_index_valid;
	iqconverter_float_t *cnv_f;
	iqconverter_int16_t *cnv_i;
	void* ctx;
	enum lpc4370sdr_sample_type sample_type;
} lpc4370sdr_device_t;

/* USB IDs: Airspy (the Airspy firmware, and lpc4370sdr on every board) and the HydraSDR RFOne's own */
static const struct {
	uint16_t vid;
	uint16_t pid;
} usb_ids[] = {
	{ 0x1d50, 0x60a1 },
	{ 0x38af, 0x0001 },
};

/* Serial number strings: one of these prefixes (Airspy boards, the HydraSDR RFOne), then the
   64-bit serial number as 16 hex digits */
static const char* const serial_prefixes[] = { "AIRSPY SN:", "HYDRASDR SN:" };
#define SERIAL_HEX_DIGITS (16)
#define SERIAL_STRING_MAX (32)

static bool usb_id_known(uint16_t vid, uint16_t pid)
{
	size_t i;
	for (i = 0; i < sizeof(usb_ids) / sizeof(usb_ids[0]); i++)
	{
		if (usb_ids[i].vid == vid && usb_ids[i].pid == pid)
			return true;
	}
	return false;
}

/* The serial number in a serial number string; false when it has none of the known forms */
static bool parse_serial(const unsigned char* str, int len, uint64_t* serial)
{
	size_t i;
	for (i = 0; i < sizeof(serial_prefixes) / sizeof(serial_prefixes[0]); i++)
	{
		size_t plen = strlen(serial_prefixes[i]);
		if (len == (int)(plen + SERIAL_HEX_DIGITS) && memcmp(str, serial_prefixes[i], plen) == 0)
		{
			char hex[SERIAL_HEX_DIGITS + 1];
			char* end;
			memcpy(hex, str + plen, SERIAL_HEX_DIGITS);
			hex[SERIAL_HEX_DIGITS] = 0;
			*serial = strtoull(hex, &end, 16);
			return end == hex + SERIAL_HEX_DIGITS;
		}
	}
	return false;
}

#define GAIN_COUNT (22)

static const uint8_t lpc4370sdr_linearity_vga_gains[GAIN_COUNT] = { 13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4 };
static const uint8_t lpc4370sdr_linearity_mixer_gains[GAIN_COUNT] = { 12, 12, 11, 9, 8, 7, 6, 6, 5, 0, 0, 1, 0, 0, 2, 2, 1, 1, 1, 1, 0, 0 };
static const uint8_t lpc4370sdr_linearity_lna_gains[GAIN_COUNT] = { 14, 14, 14, 13, 12, 10, 9, 9, 8, 9, 8, 6, 5, 3, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t lpc4370sdr_sensitivity_vga_gains[GAIN_COUNT] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4 };
static const uint8_t lpc4370sdr_sensitivity_mixer_gains[GAIN_COUNT] = { 12, 12, 12, 12, 11, 10, 10, 9, 9, 8, 7, 4, 4, 4, 3, 2, 2, 1, 0, 0, 0, 0 };
static const uint8_t lpc4370sdr_sensitivity_lna_gains[GAIN_COUNT] = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12, 9, 9, 8, 7, 6, 5, 3, 2, 1, 0 };

static int cancel_transfers(lpc4370sdr_device_t* device)
{
	uint32_t transfer_index;

	if (device->transfers != NULL)
	{
		for (transfer_index = 0; transfer_index<device->transfer_count; transfer_index++)
		{
			if (device->transfers[transfer_index] != NULL)
			{
				libusb_cancel_transfer(device->transfers[transfer_index]);
			}
		}
		return LPC4370SDR_SUCCESS;
	}
	else {
		return LPC4370SDR_ERROR_OTHER;
	}
}

/* The transfers in flight: counted on the thread that submits them and on the libusb event thread */
static long in_flight(lpc4370sdr_device_t* device)
{
#if defined(_MSC_VER)
	return InterlockedCompareExchange((volatile LONG*)&device->transfers_in_flight, 0, 0);
#else
	return __atomic_load_n(&device->transfers_in_flight, __ATOMIC_SEQ_CST);
#endif
}

static void in_flight_add(lpc4370sdr_device_t* device, long n)
{
#if defined(_MSC_VER)
	InterlockedExchangeAdd((volatile LONG*)&device->transfers_in_flight, n);
#else
	__atomic_fetch_add(&device->transfers_in_flight, n, __ATOMIC_SEQ_CST);
#endif
}

static void* alloc_usb_buffer(lpc4370sdr_device_t* device)
{
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000105)
	if (device->dev_mem_buffers)
	{
		return libusb_dev_mem_alloc(device->usb_device, device->buffer_size);
	}
#endif
	return malloc(device->buffer_size);
}

/*
 * Byte reads from a zero-copy (usbfs) buffer cost under a nanosecond where the USB controller
 * is IO coherent and hundreds where it is not, and nothing tells the application which. So
 * measure: when the buffers are much slower to read than ordinary memory, the stream gives
 * zero-copy up and takes ordinary buffers, which the kernel fills with its own copy from
 * cached memory. That beats the consumer copying each uncached buffer out with one wide
 * memcpy: on a Raspberry Pi CM0 (dwc_otg) with three devices streaming, 0.19 of a core less,
 * and the consumer no longer falls behind and drops buffers.
 */
#define PROBE_BYTES (2048)
#define PROBE_ROUNDS (5)
#define PROBE_RATIO (8)

#if !defined(_WIN32) && defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000105)
static double read_cost_ns(const volatile uint8_t* buf, int len)
{
	volatile uint32_t sum = 0;
	double best = 0;
	int i, r;

	for (i = 0; i < len; i++)
		sum += buf[i]; /* map the pages first */
	for (r = 0; r < PROBE_ROUNDS; r++)
	{
		struct timespec a, b;
		double ns;

		clock_gettime(CLOCK_MONOTONIC, &a);
		for (i = 0; i < len; i++)
			sum += buf[i];
		clock_gettime(CLOCK_MONOTONIC, &b);
		ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / len;
		if (r == 0 || ns < best)
			best = ns;
	}
	return best;
}
#endif

static void probe_usb_buffers(lpc4370sdr_device_t* device)
{
	device->usb_buf_slow = false;
#if !defined(_WIN32) && defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000105)
	{
		uint8_t* cached;
		double slow, fast;

		if (!device->dev_mem_buffers || device->received_samples_queue[0] == NULL)
			return;
		cached = (uint8_t*)calloc(1, PROBE_BYTES);
		if (cached == NULL)
			return;
		fast = read_cost_ns(cached, PROBE_BYTES);
		slow = read_cost_ns((const volatile uint8_t*)device->received_samples_queue[0], PROBE_BYTES);
		free(cached);
		device->usb_buf_slow = slow > fast * PROBE_RATIO;
	}
#else
	(void)device;
#endif
}

static void free_usb_buffer(lpc4370sdr_device_t* device, void* buffer)
{
	if (buffer == NULL)
	{
		return;
	}
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000105)
	if (device->dev_mem_buffers)
	{
		libusb_dev_mem_free(device->usb_device, (unsigned char*)buffer, device->buffer_size);
		return;
	}
#endif
	free(buffer);
}

static int allocate_usb_buffers(lpc4370sdr_device_t* device, unsigned char** transfer_buffers)
{
	int i;
	uint32_t t;

	for (i = 0; i < RAW_BUFFER_COUNT; i++)
	{
		device->received_samples_queue[i] = (uint16_t*)alloc_usb_buffer(device);
		if (device->received_samples_queue[i] == NULL)
		{
			return -1;
		}
		memset(device->received_samples_queue[i], 0, device->buffer_size);
	}
	for (t = 0; t < device->transfer_count; t++)
	{
		transfer_buffers[t] = (unsigned char*)alloc_usb_buffer(device);
		if (transfer_buffers[t] == NULL)
		{
			return -1;
		}
	}
	return 0;
}

static void release_usb_buffers(lpc4370sdr_device_t* device, unsigned char** transfer_buffers)
{
	int i;
	uint32_t t;

	for (i = 0; i < RAW_BUFFER_COUNT; i++)
	{
		free_usb_buffer(device, device->received_samples_queue[i]);
		device->received_samples_queue[i] = NULL;
	}
	for (t = 0; t < device->transfer_count; t++)
	{
		free_usb_buffer(device, transfer_buffers[t]);
		transfer_buffers[t] = NULL;
	}
}

/* Frees the transfers and every buffer of the stream; only while no transfer is in flight */
static int free_transfers(lpc4370sdr_device_t* device)
{
	int i;
	uint32_t transfer_index;

	if (in_flight(device) > 0)
	{
		return LPC4370SDR_ERROR_BUSY;
	}

	if (device->transfers != NULL)
	{
		// libusb_close() should free all transfers referenced from this array.
		for (transfer_index = 0; transfer_index < device->transfer_count; transfer_index++)
		{
			if (device->transfers[transfer_index] != NULL)
			{
				free_usb_buffer(device, device->transfers[transfer_index]->buffer);
				libusb_free_transfer(device->transfers[transfer_index]);
				device->transfers[transfer_index] = NULL;
			}
		}
		free(device->transfers);
		device->transfers = NULL;
	}

	free(device->output_buffer);
	device->output_buffer = NULL;

	for (i = 0; i < DSP_SLOTS; i++)
	{
		free(device->dsp_slots[i].buf);
		device->dsp_slots[i].buf = NULL;
	}

	free(device->unpacked_samples);
	device->unpacked_samples = NULL;
	free(device->framed_samples);
	device->framed_samples = NULL;
	free(device->frame_prev);
	device->frame_prev = NULL;
	device->usb_buf_slow = false;
	device->block_capacity = 0;

	for (i = 0; i < RAW_BUFFER_COUNT; i++)
	{
		free_usb_buffer(device, device->received_samples_queue[i]);
		device->received_samples_queue[i] = NULL;
	}

	return LPC4370SDR_SUCCESS;
}

static int allocate_transfers(lpc4370sdr_device_t* const device)
{
	int i;
	size_t sample_count, alloc_count;
	uint32_t transfer_index;
	unsigned char** transfer_buffers;
	int result = LPC4370SDR_ERROR_NO_MEM;

	if (device->transfers != NULL)
	{
		return LPC4370SDR_ERROR_BUSY;
	}

	transfer_buffers = (unsigned char**)calloc(device->transfer_count, sizeof(unsigned char*));
	if (transfer_buffers == NULL)
	{
		return LPC4370SDR_ERROR_NO_MEM;
	}

	device->dev_mem_buffers = (getenv("LPC4370SDR_NO_ZEROCOPY") == NULL);
	if (allocate_usb_buffers(device, transfer_buffers) != 0)
	{
		release_usb_buffers(device, transfer_buffers);
		device->dev_mem_buffers = false;
		if (allocate_usb_buffers(device, transfer_buffers) != 0)
		{
			goto fail;
		}
	}

	probe_usb_buffers(device);
	if (device->usb_buf_slow)
	{
		release_usb_buffers(device, transfer_buffers);
		device->dev_mem_buffers = false;
		if (allocate_usb_buffers(device, transfer_buffers) != 0)
		{
			goto fail;
		}
	}

	if (device->packing_mode == 3)
	{
		sample_count = device->buffer_size;
	}
	else if (device->packing_enabled)
	{
		sample_count = ((device->buffer_size / 2) * 4) / 3;
	}
	else
	{
		sample_count = device->buffer_size / 2;
	}
	/* Gap fill may add up to a buffer's worth of samples to a block */
	device->fill_max = (uint32_t)sample_count & ~3u;
	/* After a resync a framed buffer can complete one chunk more, one that started in the previous buffer */
	sample_count += LPC4370SDR_FRAME_WIRE_UNPACKED / 2;
	sample_count += device->fill_max;
	/* The sample conversions work in groups of four */
	alloc_count = sample_count + 4;

	for (i = 0; i < DSP_SLOTS; i++)
	{
		device->dsp_slots[i].buf = malloc(alloc_count * sizeof(float));
		if (device->dsp_slots[i].buf == NULL)
		{
			goto fail;
		}
	}

	device->output_buffer = (float *)malloc(alloc_count * sizeof(float));
	if (device->output_buffer == NULL)
	{
		goto fail;
	}

	if (device->packing_enabled)
	{
		device->unpacked_samples = (uint16_t*)malloc(alloc_count * sizeof(uint16_t));
		if (device->unpacked_samples == NULL)
		{
			goto fail;
		}
	}

	device->framed_samples = (uint8_t*)malloc(alloc_count * sizeof(uint16_t));
	device->frame_prev = (uint8_t*)malloc((FRAME_PREV_CHUNKS + 1) * LPC4370SDR_FRAME_WIRE_UNPACKED);
	if (device->framed_samples == NULL || device->frame_prev == NULL)
	{
		goto fail;
	}
	device->block_capacity = (uint32_t)sample_count;

	device->transfers = (struct libusb_transfer**) calloc(device->transfer_count, sizeof(struct libusb_transfer*));
	if (device->transfers == NULL)
	{
		goto fail;
	}

	for (transfer_index = 0; transfer_index<device->transfer_count; transfer_index++)
	{
		device->transfers[transfer_index] = libusb_alloc_transfer(0);
		if (device->transfers[transfer_index] == NULL)
		{
			result = LPC4370SDR_ERROR_LIBUSB;
			goto fail;
		}

		libusb_fill_bulk_transfer(
			device->transfers[transfer_index],
			device->usb_device,
			0,
			transfer_buffers[transfer_index],
			device->buffer_size,
			NULL,
			device,
			0
			);
		transfer_buffers[transfer_index] = NULL; /* the transfer owns it now */
	}

	free(transfer_buffers);
	return LPC4370SDR_SUCCESS;

fail:
	release_usb_buffers(device, transfer_buffers);
	free(transfer_buffers);
	free_transfers(device);
	return result;
}

static int prepare_transfers(lpc4370sdr_device_t* device, const uint_fast8_t endpoint_address, libusb_transfer_cb_fn callback)
{
	int error;
	uint32_t transfer_index;
	if (device->transfers != NULL)
	{
		for (transfer_index = 0; transfer_index<device->transfer_count; transfer_index++)
		{
			device->transfers[transfer_index]->endpoint = endpoint_address;
			device->transfers[transfer_index]->callback = callback;

			/* Counted first: its callback may run before libusb_submit_transfer() returns */
			in_flight_add(device, 1);
			error = libusb_submit_transfer(device->transfers[transfer_index]);
			if (error != 0)
			{
				in_flight_add(device, -1);
				return LPC4370SDR_ERROR_LIBUSB;
			}
		}
		return LPC4370SDR_SUCCESS;
	}
	else {
		// This shouldn't happen.
		return LPC4370SDR_ERROR_OTHER;
	}
}

/*
 * Cancel the transfers and let libusb give them all back (it calls their callbacks). Cancelled
 * again on every round: one that completed and was resubmitted meanwhile would miss a single
 * cancel. Should libusb still hold some after 10 s, they stay allocated (free_transfers() refuses).
 */
static void reap_transfers(lpc4370sdr_device_t* device)
{
	struct timeval timeout = { 0, 100000 };
	int tries;

	for (tries = 0; in_flight(device) > 0 && tries < 100; tries++)
	{
		cancel_transfers(device);
		libusb_handle_events_timeout_completed(device->usb_context, &timeout, NULL);
	}
}

static void convert_samples_int16(uint16_t *src, int16_t *dest, int count)
{
	int i;
	for (i = 0; i < count; i += 4)
	{
		dest[i + 0] = (int16_t)((src[i + 0] - 2048) * (1 << SAMPLE_SHIFT));
		dest[i + 1] = (int16_t)((src[i + 1] - 2048) * (1 << SAMPLE_SHIFT));
		dest[i + 2] = (int16_t)((src[i + 2] - 2048) * (1 << SAMPLE_SHIFT));
		dest[i + 3] = (int16_t)((src[i + 3] - 2048) * (1 << SAMPLE_SHIFT));
	}
}

static void convert_samples_float(uint16_t *src, float *dest, int count)
{
	int i;
	for (i = 0; i < count; i += 4)
	{
		dest[i + 0] = (src[i + 0] - 2048) * SAMPLE_SCALE;
		dest[i + 1] = (src[i + 1] - 2048) * SAMPLE_SCALE;
		dest[i + 2] = (src[i + 2] - 2048) * SAMPLE_SCALE;
		dest[i + 3] = (src[i + 3] - 2048) * SAMPLE_SCALE;
	}
}

static inline void unpack_samples(uint32_t *input, uint16_t *output, int length)
{
	int i, j;

	for (i = 0, j = 0; j < length; i += 3, j += 8)
	{
		const uint32_t a = TO_LE(input[i]), b = TO_LE(input[i + 1]), c = TO_LE(input[i + 2]);
		output[j + 0] = (a >> 20) & 0xfff;
		output[j + 1] = (a >> 8) & 0xfff;
		output[j + 2] = ((a & 0xff) << 4) | ((b >> 28) & 0xf);
		output[j + 3] = ((b & 0xfff0000) >> 16);
		output[j + 4] = ((b & 0xfff0) >> 4);
		output[j + 5] = ((b & 0xf) << 8) | ((c & 0xff000000) >> 24);
		output[j + 6] = ((c >> 12) & 0xfff);
		output[j + 7] = ((c & 0xfff));
	}
}

static int send_framing(lpc4370sdr_device_t* device, uint8_t value)
{
int result;
uint8_t retval;

result = libusb_control_transfer(
	device->usb_device,
	LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
	LPC4370SDR_SET_FRAMING,
	0,
	value ? 1 : 0,
	&retval,
	1,
	LIBUSB_CTRL_TIMEOUT_MS);

if (result < 1)
{
	return LPC4370SDR_ERROR_LIBUSB;
}
return LPC4370SDR_SUCCESS;
}

static int send_sof_divider(lpc4370sdr_device_t* device, uint32_t divider)
{
	int result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_SET_SOF_DIVIDER,
		divider & 0xFFFF,
		divider >> 16,
		NULL,
		0,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result != 0)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

static inline void unpack_samples_8bit(const uint8_t *input, uint16_t *output, int length)
{
	/* Word reads: the USB buffers may be uncached (zero-copy), where byte reads are slow */
	const uint32_t *in = (const uint32_t *)input;
	int i;
	for (i = 0; i + 4 <= length; i += 4)
	{
		uint32_t w = TO_LE(in[i / 4]);
		output[i + 0] = (uint16_t)((w & 0xff) << 4);
		output[i + 1] = (uint16_t)(((w >> 8) & 0xff) << 4);
		output[i + 2] = (uint16_t)(((w >> 16) & 0xff) << 4);
		output[i + 3] = (uint16_t)((w >> 24) << 4);
	}
	for (; i < length; i++)
		output[i] = (uint16_t)input[i] << 4;
}

/*
 * Framed chunks and packing 2
 *
 * A buffer holds whole device chunks of LPC4370SDR_FRAME_WIRE_* bytes. With framing
 * each chunk starts with an lpc4370sdr_frame_header_t; in packing 2 the first two
 * 12-bit values of each chunk are the chunk counter and the ADC status, not
 * samples; with the tail flag the payload ends with LPC4370SDR_FRAME_TAIL_SAMPLES
 * values that are no samples either.
 */
static uint32_t chunk_wire_bytes(const lpc4370sdr_device_t* device)
{
	if (device->packing_mode == 3)
		return LPC4370SDR_FRAME_WIRE_8BIT;
	return device->packing_enabled ? LPC4370SDR_FRAME_WIRE_PACKED : LPC4370SDR_FRAME_WIRE_UNPACKED;
}

/* Values (samples, or packing 2 counter and status) in a chunk payload of 'bytes' */
static uint32_t payload_values(const lpc4370sdr_device_t* device, uint32_t bytes)
{
	if (device->packing_mode == 3)
		return bytes;
	if (device->packing_enabled)
		return (bytes / 3) * 2;
	return bytes / 2;
}

/* Bytes that 'values' payload values take */
static size_t payload_bytes(const lpc4370sdr_device_t* device, uint32_t values)
{
	if (device->packing_mode == 3)
		return values;
	if (device->packing_enabled)
		return (size_t)values / 2 * 3;
	return (size_t)values * 2;
}

static uint32_t expected_frame_flags(const lpc4370sdr_device_t* device)
{
	if (device->packing_mode == 3)
		return LPC4370SDR_FRAME_FLAG_8BIT;
	if (device->packing_mode == 2)
		return LPC4370SDR_FRAME_FLAG_PACKED | LPC4370SDR_FRAME_FLAG_TIMESTAMP;
	return device->packing_enabled ? LPC4370SDR_FRAME_FLAG_PACKED : 0;
}

/* Samples in a chunk of 'values' payload values: less the packing 2 counter and status and the tail */
static uint32_t frame_samples(uint32_t values, uint32_t skip, uint32_t flags)
{
	return values - skip - ((flags & LPC4370SDR_FRAME_FLAG_TAIL) ? LPC4370SDR_FRAME_TAIL_SAMPLES : 0);
}

/* Header fields, little-endian on the wire, at their places in lpc4370sdr_frame_header_t */
#define HDR(field) offsetof(lpc4370sdr_frame_header_t, field)
static uint16_t hdr16(const uint8_t* chunk, size_t at)
{
	return (uint16_t)(chunk[at] | (chunk[at + 1] << 8));
}
static uint32_t hdr32(const uint8_t* chunk, size_t at)
{
	return (uint32_t)chunk[at] | ((uint32_t)chunk[at + 1] << 8) | ((uint32_t)chunk[at + 2] << 16) | ((uint32_t)chunk[at + 3] << 24);
}

/* A valid chunk header: magic, version, check word, size and flags as expected */
static bool frame_header_valid(const lpc4370sdr_device_t* device, const uint8_t* chunk, uint32_t values, uint32_t skip)
{
	const uint32_t flags = chunk[HDR(flags)];
	const uint32_t samples = frame_samples(values, skip, flags);
	const uint32_t format = LPC4370SDR_FRAME_FLAG_PACKED | LPC4370SDR_FRAME_FLAG_8BIT | LPC4370SDR_FRAME_FLAG_TIMESTAMP;
	uint32_t sum = 0;
	size_t i;

	if (hdr32(chunk, HDR(magic)) != LPC4370SDR_FRAME_MAGIC)
		return false;
	for (i = 0; i < HDR(check); i += 4)
		sum += hdr32(chunk, i);
	if (hdr32(chunk, HDR(check)) != ~sum)
		return false;
	return chunk[HDR(version)] == LPC4370SDR_FRAME_VERSION && hdr16(chunk, HDR(sample_count)) == samples && samples > 0 &&
		(flags & format) == expected_frame_flags(device);
}

/* The tail marker, when the chunk has one: the device wrote ~chunk_index there once the chunk was complete */
static bool frame_tail_valid(const uint8_t* chunk, uint32_t wire)
{
	uint32_t marker;

	if (!(chunk[HDR(flags)] & LPC4370SDR_FRAME_FLAG_TAIL))
		return true;
	memcpy(&marker, chunk + wire - sizeof(marker), sizeof(marker));
	return TO_LE(marker) == ~hdr32(chunk, HDR(chunk_index));
}

/* A wrapping counter extended: it only grows, by less than 2^bits between two headers */
static uint32_t extend16(uint32_t ext, uint16_t low)
{
	return ext + (uint16_t)(low - (uint16_t)ext);
}

/* Output state while deframing one buffer */
typedef struct {
	uint16_t* out;       /* next sample in framed_samples */
	size_t raw_bytes;    /* bytes in framed_samples, LPC4370SDR_SAMPLE_RAW */
	uint32_t raw_values; /* values in framed_samples, LPC4370SDR_SAMPLE_RAW */
	bool keep_raw;
	uint32_t samples;    /* samples in the block so far, filled ones included */
	uint32_t fill_room;  /* samples gap fill may still add to the block */
} deframe_ctx_t;

/*
 * The device restarted the stream (a setting changed while streaming): its chunk and sample
 * indices start again. The samples of the old stream in this block are dropped, since one
 * block's timeline cannot hold both streams.
 */
static void deframe_restart(lpc4370sdr_device_t* device, deframe_ctx_t* ctx)
{
	lpc4370sdr_transfer_metadata_t* m = &device->meta;

	device->expected_sample_index_valid = false;
	device->chunk_index_valid = false;
	device->sof_count_seen = 0;
	device->pps_count_seen = 0;
	device->chunk_ext_valid = false;
	device->counts_ext_valid = false; /* the device counts from 0 again; its SOF frame number goes on */
	m->stream_restarts++;
	m->ext_len = 0;

	ctx->out = (uint16_t*)device->framed_samples;
	ctx->raw_bytes = 0;
	ctx->raw_values = 0;
	ctx->samples = 0;
	ctx->fill_room = device->fill_max;
	m->chunks = 0;
	m->gap_samples = 0;
	m->gaps_len = 0;
	m->filled_samples = 0;
	m->duplicate_chunks = 0;
	m->sof_tags_len = 0;
	m->adc_status = 0;
}

/* A backward jump of the sample index by more than this many chunks is a restart, for firmware
   that does not count stream starts (the resync takes back at most one chunk) */
#define RESTART_CHUNKS (32)

/* What a chunk header says about the chunk */
enum deframe_result {
	DEFRAME_BAD,     /* no valid header here: out of step with the chunk boundaries */
	DEFRAME_SKIP,    /* its samples were delivered already */
	DEFRAME_CORRUPT, /* the device sent it while overwriting it: metadata only */
	DEFRAME_OK
};

/* A sample index from its low word: the one nearest the chunk's own sample index */
static uint64_t widen_index(uint64_t chunk_index, uint32_t lo)
{
	uint64_t v = (chunk_index & ~(uint64_t)0xFFFFFFFF) | lo;
	if (v > chunk_index && v - chunk_index > 0x80000000ull && v >= 0x100000000ull)
		v -= 0x100000000ull;
	else if (v < chunk_index && chunk_index - v > 0x80000000ull)
		v += 0x100000000ull;
	return v;
}

/* Take a chunk header into the block metadata */
static enum deframe_result deframe_header(lpc4370sdr_device_t* device, deframe_ctx_t* ctx, const uint8_t* chunk,
	uint32_t wire, uint32_t values, uint32_t skip, uint64_t* index, uint32_t* samples, uint64_t* gap)
{
	lpc4370sdr_transfer_metadata_t* m = &device->meta;
	uint32_t chunk_index, lost, start_count, overrun_back;

	if (!frame_header_valid(device, chunk, values, skip))
	{
		/* firmware of another header version: said once, its chunks are all bad */
		if (hdr32(chunk, HDR(magic)) == LPC4370SDR_FRAME_MAGIC && chunk[HDR(version)] != LPC4370SDR_FRAME_VERSION &&
			!device->version_noted)
		{
			device->version_noted = true;
			fprintf(stderr, "lpc4370sdr: the device sends chunk header version %u, this library reads %u: "
				"load the matching firmware\n", chunk[HDR(version)], LPC4370SDR_FRAME_VERSION);
		}
		m->sync_errors++;
		return DEFRAME_BAD;
	}
	*samples = frame_samples(values, skip, chunk[HDR(flags)]);

	/* A new stream start count: the device restarted the stream, its indices start again */
	start_count = chunk[HDR(stream_start_count)];
	if (device->stream_start_valid && start_count != device->stream_start_count)
		deframe_restart(device, ctx);
	device->stream_start_count = start_count;
	device->stream_start_valid = true;
	device->frame_tail = (chunk[HDR(flags)] & LPC4370SDR_FRAME_FLAG_TAIL) != 0;

	/* The chunk index, extended past its 32-bit wrap: the nearest to the last one's (kept only
	   once the chunk turns out new: a duplicate is older) */
	chunk_index = hdr32(chunk, HDR(chunk_index));
	{
		uint64_t v = chunk_index;
		if (device->chunk_ext_valid)
		{
			v = (device->chunk_ext & ~(uint64_t)0xFFFFFFFF) | chunk_index;
			if (v > device->chunk_ext && v - device->chunk_ext > 0x80000000ull && v >= 0x100000000ull)
				v -= 0x100000000ull;
			else if (v < device->chunk_ext && device->chunk_ext - v > 0x80000000ull)
				v += 0x100000000ull;
		}
		*index = v * *samples;
		/* firmware that does not count stream starts: a big step back is a restart */
		if (device->expected_sample_index_valid && *index + (uint64_t)RESTART_CHUNKS * values < device->expected_sample_index)
		{
			deframe_restart(device, ctx);
			v = chunk_index;
			*index = v * *samples;
		}
		if (device->expected_sample_index_valid && *index < device->expected_sample_index)
		{
			m->duplicate_chunks++;
			return DEFRAME_SKIP;
		}
		device->chunk_ext = v;
		device->chunk_ext_valid = true;
	}

	/* The counters sent modulo 2^16, extended */
	if (device->counts_ext_valid)
	{
		device->lost_ext = extend16(device->lost_ext, hdr16(chunk, HDR(lost_chunks)));
		device->overrun_ext = extend16(device->overrun_ext, hdr16(chunk, HDR(overrun_chunks)));
		device->pps_ext = extend16(device->pps_ext, hdr16(chunk, HDR(pps_count)));
		device->sof_count_ext = extend16(device->sof_count_ext, hdr16(chunk, HDR(sof_count)));
	}
	else
	{
		device->lost_ext = hdr16(chunk, HDR(lost_chunks));
		device->overrun_ext = hdr16(chunk, HDR(overrun_chunks));
		device->pps_ext = hdr16(chunk, HDR(pps_count));
		device->sof_count_ext = hdr16(chunk, HDR(sof_count));
		device->counts_ext_valid = true;
	}
	if (device->sof_frame_ext_valid)
		device->sof_frame_ext = extend16(device->sof_frame_ext, hdr16(chunk, HDR(sof_frame)));
	else
	{
		device->sof_frame_ext = hdr16(chunk, HDR(sof_frame));
		device->sof_frame_ext_valid = true;
	}

	/* Chunks missing from the sequence that the device did not count as lost went missing on the way */
	lost = device->lost_ext;
	if (device->chunk_index_valid)
	{
		uint32_t gap = chunk_index - device->last_chunk_index - 1;
		uint32_t counted = lost - device->last_lost_chunks;
		if ((int32_t)gap > 0 && gap > counted)
			m->unaccounted_chunks += gap - counted;
	}
	device->last_chunk_index = chunk_index;
	device->last_lost_chunks = lost;
	device->chunk_index_valid = true;

	if (m->chunks == 0)
	{
		m->flags = chunk[HDR(flags)];
		m->freq_hz = hdr32(chunk, HDR(freq_hz));
	}
	m->lost_chunks = lost;
	m->overrun_chunks = device->overrun_ext;
	/* The last overrun chunk, counted back from this one */
	overrun_back = hdr16(chunk, HDR(overrun_back));
	{
		const uint64_t back = (uint64_t)overrun_back * *samples;
		m->overrun_sample_index = back <= *index ? *index - back : 0;
	}
	m->pps_sample_index = widen_index(*index, hdr32(chunk, HDR(pps_sample_index_lo)));
	m->pps_fraction = (uint32_t)hdr16(chunk, HDR(pps_fraction)) << 16;
	m->pps_count = device->pps_ext;
	m->sof_sample_index = widen_index(*index, hdr32(chunk, HDR(sof_sample_index_lo)));
	m->sof_fraction = (uint32_t)hdr16(chunk, HDR(sof_fraction)) << 16;
	m->sof_frame = device->sof_frame_ext;
	m->sof_count = device->sof_count_ext;
	if (m->sof_count != 0 && m->sof_count != device->sof_count_seen && m->sof_tags_len < LPC4370SDR_METADATA_SOF_MAX)
	{
		lpc4370sdr_sof_tag_t* tag = &m->sof_tags[m->sof_tags_len++];
		tag->sample_index = m->sof_sample_index;
		tag->fraction = m->sof_fraction;
		tag->frame = m->sof_frame;
		tag->count = m->sof_count;
	}
	device->sof_count_seen = m->sof_count;
	{
		uint32_t n = chunk[HDR(uart_len)];
		if (n > LPC4370SDR_FRAME_UART_BYTES)
			n = LPC4370SDR_FRAME_UART_BYTES;
		if (m->uart_len + n <= LPC4370SDR_METADATA_UART_MAX)
		{
			memcpy(&m->uart_data[m->uart_len], chunk + HDR(uart_data), n);
			m->uart_len += n;
			device->uart_total += n;
		}
		/* A new edge: its place among the bytes, from the end of this chunk's (bytes lost with
		   a chunk before it shift only those older ones) */
		if (m->pps_count != device->pps_count_seen)
		{
			const int64_t at = (int64_t)device->uart_total + (int16_t)hdr16(chunk, HDR(pps_uart_rel));
			m->pps_uart_count = at > 0 ? (uint64_t)at : 0;
			m->pps_uart_quiet_us = hdr16(chunk, HDR(pps_uart_quiet_us));
			device->pps_count_seen = m->pps_count;
		}
	}
	/* The samples of an overwritten chunk count as missing, like a lost chunk */
	if (!frame_tail_valid(chunk, wire))
	{
		m->corrupt_chunks++;
		return DEFRAME_CORRUPT;
	}

	/* The extension area, when it carries items: it goes with the chunk's samples (an
	   overwritten chunk's goes with them; a block holds no more areas than chunks) */
	{
		const uint8_t* area = chunk + HDR(ext);
		if (area[0] != LPC4370SDR_EXT_END)
		{
			uint32_t pos = 0;
			uint8_t type, len;
			const uint8_t* data;
			int r;
			while ((r = lpc4370sdr_ext_next(area, &pos, &type, &data, &len)) > 0)
				;
			if (r < 0)
				m->ext_malformed++;
			if (m->ext_len < LPC4370SDR_METADATA_EXT_MAX)
			{
				lpc4370sdr_ext_area_t* e = &m->ext[m->ext_len++];
				e->sample_index = *index;
				e->chunk_index = device->chunk_ext;
				memcpy(e->data, area, LPC4370SDR_FRAME_EXT_BYTES);
			}
			else
				m->ext_dropped++;
		}
	}

	*gap = 0;
	if (device->expected_sample_index_valid && *index > device->expected_sample_index)
	{
		*gap = *index - device->expected_sample_index;
		m->gap_samples += *gap;
	}
	device->expected_sample_index = *index + *samples;
	device->expected_sample_index_valid = true;
	return DEFRAME_OK;
}

/* Mid-scale on the 12-bit scale of the unpacked samples: zero once converted */
#define FILL_VALUE (2048)

/*
 * Record a gap in front of the chunk being taken and, with gap fill, put mid-scale samples in its
 * place: the last ones of the gap when it is longer than the room left. A partial fill leaves a
 * multiple of 4 samples missing where it can, which keeps the fs/4 translation of the IQ
 * conversion in phase.
 */
static uint32_t deframe_gap(lpc4370sdr_device_t* device, deframe_ctx_t* ctx, uint64_t gap)
{
	lpc4370sdr_transfer_metadata_t* m = &device->meta;
	uint32_t fill = 0, i;

	if (device->gap_fill && !ctx->keep_raw)
	{
		fill = (gap < ctx->fill_room) ? (uint32_t)gap : ctx->fill_room & ~3u;
		if (fill < gap)
		{
			uint32_t odd = (uint32_t)(gap - fill) & 3u;
			fill = (fill >= 4) ? fill - 4 + odd : 0;
		}
		for (i = 0; i < fill; i++)
			ctx->out[i] = FILL_VALUE;
		ctx->out += fill;
		ctx->fill_room -= fill;
		m->filled_samples += fill;
	}
	if (m->gaps_len < LPC4370SDR_METADATA_GAPS_MAX)
	{
		lpc4370sdr_gap_t* g = &m->gaps[m->gaps_len++];
		g->offset = ctx->samples;
		g->filled = fill;
		g->samples = gap;
	}
	ctx->samples += fill;
	return fill;
}

/* Take one chunk: its header into the metadata, its samples (or raw payload) to the output */
static enum deframe_result deframe_chunk(lpc4370sdr_device_t* device, deframe_ctx_t* ctx, const uint8_t* chunk,
	uint32_t wire, uint32_t header, uint32_t values, uint32_t skip)
{
	lpc4370sdr_transfer_metadata_t* m = &device->meta;
	const uint8_t* payload = chunk + header;
	uint64_t index = 0, gap = 0;
	uint32_t samples = values - skip;
	uint32_t n, fill = 0;
	size_t used = ctx->keep_raw ? ctx->raw_values : (size_t)(ctx->out - (uint16_t*)device->framed_samples);

	/* Room for this chunk and whatever gap fill may still add: the buffers are sized for the chunks
	   a USB buffer can complete, so this only guards against the unforeseen */
	if (used + values + ctx->fill_room > device->block_capacity)
	{
		m->corrupt_chunks++;
		return DEFRAME_CORRUPT;
	}

	if (header)
	{
		enum deframe_result r = deframe_header(device, ctx, chunk, wire, values, skip, &index, &samples, &gap);
		if (r != DEFRAME_OK)
			return r;
		if (gap)
			fill = deframe_gap(device, ctx, gap);
		if (m->chunks == 0)
			m->sample_index = index - fill;
	}
	m->chunks++;
	ctx->samples += samples;
	n = samples + skip; /* payload values to take: the tail stays behind */

	if (ctx->keep_raw)
	{
		size_t bytes = payload_bytes(device, n);
		memcpy(device->framed_samples + ctx->raw_bytes, payload, bytes);
		ctx->raw_bytes += bytes;
		ctx->raw_values += n;
		return DEFRAME_OK;
	}

	if (device->packing_mode == 3)
		unpack_samples_8bit(payload, ctx->out, (int)n);
	else if (device->packing_enabled)
		unpack_samples((uint32_t*)payload, ctx->out, (int)n);
	else
		memcpy(ctx->out, payload, n * sizeof(uint16_t));
#ifdef LPC4370SDR_BIG_ENDIAN
	if (device->packing_mode == 0)
	{
		uint32_t i;
		for (i = 0; i < n; i++)
			ctx->out[i] = TO_LE16(ctx->out[i]);
	}
#endif
	if (skip)
	{
		m->adc_status |= ctx->out[1];
		memmove(ctx->out, ctx->out + skip, samples * sizeof(uint16_t));
	}
	ctx->out += samples;
	return DEFRAME_OK;
}

/*
 * n bytes at position 'pos' of a framed buffer; negative positions are the end of the previous
 * buffer (frame_prev). Bytes that span both are put together in 'tmp'.
 */
static const uint8_t* frame_at(const lpc4370sdr_device_t* device, const uint8_t* raw, int32_t pos, uint32_t n,
	uint32_t wire, uint8_t* tmp)
{
	if (pos >= 0)
		return raw + pos;
	if (pos + (int32_t)n <= 0)
		return device->frame_prev + FRAME_PREV_CHUNKS * wire + pos;
	memcpy(tmp, device->frame_prev + FRAME_PREV_CHUNKS * wire + pos, (size_t)-pos);
	memcpy(tmp + (-pos), raw, (size_t)(n + pos));
	return tmp;
}

/*
 * After data went missing on USB (always whole 512-byte packets) the chunk boundaries sit
 * elsewhere: the next valid header at a packet boundary from 'from' on (negative: in the end of
 * the previous buffer), confirmed by the one a chunk later when it is in the buffer. Returns
 * 'size' when there is none.
 */
static int32_t frame_resync(const lpc4370sdr_device_t* device, const uint8_t* raw, int32_t from, uint32_t size,
	uint32_t wire, uint32_t values, uint32_t skip)
{
	const int32_t packet = 512, hdr = LPC4370SDR_FRAME_HEADER_SIZE;
	uint32_t tmp[LPC4370SDR_FRAME_HEADER_SIZE / 4];
	int32_t q;

	for (q = from; q + hdr <= (int32_t)size; q += packet)
	{
		if (!frame_header_valid(device, frame_at(device, raw, q, hdr, wire, (uint8_t*)tmp), values, skip))
			continue;
		if (q + (int32_t)wire + hdr > (int32_t)size ||
			frame_header_valid(device, frame_at(device, raw, q + (int32_t)wire, hdr, wire, (uint8_t*)tmp), values, skip))
			return q;
	}
	return (int32_t)size;
}

/*
 * Gather the samples of a framed or packing 2 buffer into framed_samples, unpacked to uint16_t
 * on the 12-bit scale, or the chunk payloads as they are for LPC4370SDR_SAMPLE_RAW. Returns the
 * number of samples (values for LPC4370SDR_SAMPLE_RAW).
 *
 * Framed buffers normally hold whole chunks. When USB lost a packet the chunks no longer start
 * at the buffer start: the first invalid header makes the deframer look for the boundaries
 * again, back into the end of the previous buffer (frame_prev), and chunks that span two
 * buffers are put together.
 */
static int deframe_buffer(lpc4370sdr_device_t* device, const uint8_t* raw, enum lpc4370sdr_sample_type sample_type,
	uint32_t dropped_buffers)
{
	const uint32_t wire = chunk_wire_bytes(device);
	const uint32_t header = device->framing_active ? LPC4370SDR_FRAME_HEADER_SIZE : 0;
	const uint32_t skip = (device->packing_mode == 2) ? 2 : 0;
	const uint32_t values = payload_values(device, wire - header);
	const uint32_t size = device->buffer_size - device->buffer_size % wire;
	lpc4370sdr_transfer_metadata_t* m = &device->meta;
	deframe_ctx_t ctx;
	int32_t pos;
	bool clean_end = false; /* the last chunk was whole, valid and ended at the end of the buffer */
	/* The state before the last chunk taken, to take it back when it turns out to be cut short */
	struct {
		bool valid;
		int32_t pos;
		deframe_ctx_t ctx;
		uint32_t chunks, gaps_len, ext_len;
		uint64_t gap_samples, filled_samples;
		uint64_t expected_sample_index;
		bool expected_sample_index_valid;
	} undo;
	int result;

	/* A chunk that started in the previous buffer does not go on in this one when buffers were dropped between them */
	if (dropped_buffers)
	{
		device->frame_carry_len = 0;
		device->frame_prev_len = 0;
	}
	pos = header ? -(int32_t)device->frame_carry_len : 0;

	ctx.out = (uint16_t*)device->framed_samples;
	ctx.raw_bytes = 0;
	ctx.raw_values = 0;
	ctx.keep_raw = sample_type == LPC4370SDR_SAMPLE_RAW;
	ctx.samples = 0;
	ctx.fill_room = device->fill_max;

	m->chunks = 0;
	m->gap_samples = 0;
	m->gaps_len = 0;
	m->filled_samples = 0;
	m->duplicate_chunks = 0;
	m->adc_status = 0;
	if (!device->meta_pending)
	{
		m->uart_len = 0;
		m->uart_offset = device->uart_total;
		m->sof_tags_len = 0;
		m->ext_len = 0;
		m->ext_dropped = 0;
	}
	memset(&undo, 0, sizeof(undo));

	/* Without a tail marker (older firmware, 16-bit) a chunk cut short on USB passes every check of
	   its own: only the header after it shows it. So every chunk taken needs the next header seen
	   too, in this buffer: the loop goes on to a header whose chunk is not complete here, and a valid
	   chunk whose next header is not in this buffer waits for the next buffer. */
	const bool untailed = header && !device->frame_tail;

	while (pos + (int32_t)wire <= (int32_t)size || (untailed && pos + (int32_t)header <= (int32_t)size))
	{
		const uint8_t* chunk;
		deframe_ctx_t before;
		uint32_t chunks, gaps_len, ext_len;
		uint64_t gap_samples, filled_samples, expected;
		bool expected_valid;
		uint32_t restarts;
		enum deframe_result r;

		before = ctx;
		chunks = m->chunks;
		gaps_len = m->gaps_len;
		ext_len = m->ext_len;
		gap_samples = m->gap_samples;
		filled_samples = m->filled_samples;
		expected = device->expected_sample_index;
		expected_valid = device->expected_sample_index_valid;
		restarts = m->stream_restarts;
		if (untailed && pos >= 0 && pos + (int32_t)(wire + header) > (int32_t)size)
		{
			if (frame_header_valid(device, raw + pos, values, skip))
				break;
			m->sync_errors++;
			r = DEFRAME_BAD;
		}
		else
		{
			chunk = frame_at(device, raw, pos, wire, wire, device->frame_prev + FRAME_PREV_CHUNKS * wire);
			r = deframe_chunk(device, &ctx, chunk, wire, header, values, skip);
		}

		clean_end = (r == DEFRAME_OK || r == DEFRAME_SKIP) && pos + (int32_t)wire == (int32_t)size;
		if (r == DEFRAME_BAD)
		{
			/* Missing data moves the following chunks earlier: search from just after the previous header */
			int32_t from = pos - (int32_t)wire + 512;
			int32_t next;

			if (from < -(int32_t)device->frame_prev_len)
				from = -(int32_t)device->frame_prev_len;
			next = frame_resync(device, raw, from, size, wire, values, skip);
			if (next != pos + (int32_t)wire)
				m->resyncs++;
			/* Without a tail marker a chunk cut short on USB passes every check, and only the
			   chunk boundaries after it show that bytes went missing. Take back the chunk taken
			   just before when they did (the next header is not a whole number of chunks after it,
			   or none follows): the missing bytes may be its own. */
			if (undo.valid && undo.pos + (int32_t)wire == pos && !device->frame_tail &&
				(next >= (int32_t)size || (next - undo.pos) % (int32_t)wire != 0))
			{
				ctx = undo.ctx;
				m->chunks = undo.chunks;
				m->gaps_len = undo.gaps_len;
				m->ext_len = undo.ext_len;
				m->gap_samples = undo.gap_samples;
				m->filled_samples = undo.filled_samples;
				device->expected_sample_index = undo.expected_sample_index;
				device->expected_sample_index_valid = undo.expected_sample_index_valid;
				m->corrupt_chunks++;
			}
			undo.valid = false;
			pos = next;
			continue;
		}
		undo.valid = (r == DEFRAME_OK && m->stream_restarts == restarts);
		if (undo.valid)
		{
			undo.pos = pos;
			undo.ctx = before;
			undo.chunks = chunks;
			undo.gaps_len = gaps_len;
			undo.ext_len = ext_len;
			undo.gap_samples = gap_samples;
			undo.filled_samples = filled_samples;
			undo.expected_sample_index = expected;
			undo.expected_sample_index_valid = expected_valid;
		}
		pos += wire;
	}

	/*
	 * Keep the end of the buffer only when the next buffer may need it: a chunk starts in it, or
	 * its last chunk failed a check (the start of the next chunk may then sit in its last packets).
	 */
	if (header)
	{
		device->frame_carry_len = (uint32_t)((int32_t)size - pos);
		device->frame_prev_len = 0;
		if (device->frame_carry_len > 0 || !clean_end)
		{
			memcpy(device->frame_prev, raw + size - FRAME_PREV_CHUNKS * wire, FRAME_PREV_CHUNKS * wire);
			device->frame_prev_len = FRAME_PREV_CHUNKS * wire;
		}
	}

	device->meta_valid = device->framing_active && m->chunks > 0;
	result = ctx.keep_raw ? (int)ctx.raw_values : (int)(ctx.out - (uint16_t*)device->framed_samples);
	/* A buffer without samples reaches no callback: its UART bytes and SOF tags go with the next block */
	device->meta_pending = (result == 0);
	return result;
}

/* Consumer side: convert a block, do the first half of the IQ conversion and queue it for the
   DSP thread; false when the stream stops meanwhile */
static bool dsp_queue_block(lpc4370sdr_device_t* device, enum lpc4370sdr_sample_type sample_type, uint16_t* input_samples,
	int sample_count, uint32_t dropped_buffers)
{
	dsp_slot_t* slot;

	pthread_mutex_lock(&device->dsp_mp);
	while (device->dsp_count == DSP_SLOTS && device->streaming && !device->stop_requested)
	{
		pthread_cond_wait(&device->dsp_cv, &device->dsp_mp);
	}
	if (!device->streaming || device->stop_requested)
	{
		pthread_mutex_unlock(&device->dsp_mp);
		return false;
	}
	slot = &device->dsp_slots[device->dsp_head];
	pthread_mutex_unlock(&device->dsp_mp);

	slot->sample_type = sample_type;
	switch (slot->sample_type)
	{
	case LPC4370SDR_SAMPLE_FLOAT32_IQ:
		convert_samples_float(input_samples, (float *)slot->buf, sample_count);
		iqconverter_float_process_first(device->cnv_f, (float *)slot->buf, sample_count);
		break;

	case LPC4370SDR_SAMPLE_FLOAT32_REAL:
		convert_samples_float(input_samples, (float *)slot->buf, sample_count);
		break;

	case LPC4370SDR_SAMPLE_INT16_IQ:
		convert_samples_int16(input_samples, (int16_t *)slot->buf, sample_count);
		iqconverter_int16_process_first(device->cnv_i, (int16_t *)slot->buf, sample_count);
		break;

	case LPC4370SDR_SAMPLE_INT16_REAL:
		convert_samples_int16(input_samples, (int16_t *)slot->buf, sample_count);
		break;

	case LPC4370SDR_SAMPLE_UINT16_REAL:
		memcpy(slot->buf, input_samples, (size_t)sample_count * sizeof(uint16_t));
		break;

	case LPC4370SDR_SAMPLE_RAW:
		memcpy(slot->buf, input_samples, payload_bytes(device, (uint32_t)sample_count));
		break;

	case LPC4370SDR_SAMPLE_END:
		break;
	}
	slot->sample_count = sample_count;
	slot->dropped_buffers = dropped_buffers;
	slot->meta_valid = device->meta_valid;
	if (slot->meta_valid)
	{
		slot->meta = device->meta;
	}

	pthread_mutex_lock(&device->dsp_mp);
	device->dsp_head = (device->dsp_head + 1) % DSP_SLOTS;
	device->dsp_count++;
	pthread_cond_broadcast(&device->dsp_cv);
	pthread_mutex_unlock(&device->dsp_mp);
	return true;
}

/* transfer.dropped_samples: from the gaps when the block has metadata, otherwise from the
   dropped USB buffers; 'sample_count' is the block's, in output samples */
static uint64_t dropped_samples(const lpc4370sdr_transfer_metadata_t* meta, bool meta_valid, bool iq,
	uint32_t dropped_buffers, int sample_count)
{
	if (meta_valid)
	{
		uint64_t missing = meta->gap_samples - meta->filled_samples;
		return iq ? missing / 2 : missing;
	}
	return (uint64_t)dropped_buffers * (uint64_t)sample_count;
}

/* The device whose sample callback this thread is running, if any */
#if defined(_MSC_VER)
static __declspec(thread) lpc4370sdr_device_t* callback_device;
#else
static __thread lpc4370sdr_device_t* callback_device;
#endif

/* The application's callback, marked so that lpc4370sdr_close() can refuse to run inside it */
static int run_callback(lpc4370sdr_device_t* device, lpc4370sdr_transfer_t* transfer)
{
	int result;

	callback_device = device;
	result = device->callback(transfer);
	callback_device = NULL;
	return result;
}

/* DSP thread: the second half of the IQ conversion, then the application's callback */
static void* dsp_threadproc(void *arg)
{
	lpc4370sdr_device_t* device = (lpc4370sdr_device_t*)arg;
	lpc4370sdr_transfer_t transfer;
	bool iq;

	pthread_mutex_lock(&device->dsp_mp);
	while (device->streaming && !device->stop_requested)
	{
		dsp_slot_t* slot;
		int sample_count;

		if (device->dsp_count == 0)
		{
			pthread_cond_wait(&device->dsp_cv, &device->dsp_mp);
			continue;
		}
		slot = &device->dsp_slots[device->dsp_tail];
		pthread_mutex_unlock(&device->dsp_mp);

		sample_count = slot->sample_count;
		iq = slot->sample_type == LPC4370SDR_SAMPLE_FLOAT32_IQ || slot->sample_type == LPC4370SDR_SAMPLE_INT16_IQ;
		if (slot->sample_type == LPC4370SDR_SAMPLE_FLOAT32_IQ)
		{
			iqconverter_float_process_second(device->cnv_f, (float *)slot->buf, sample_count);
			sample_count /= 2;
		}
		else if (slot->sample_type == LPC4370SDR_SAMPLE_INT16_IQ)
		{
			iqconverter_int16_process_second(device->cnv_i, (int16_t *)slot->buf, sample_count);
			sample_count /= 2;
		}

		transfer.device = device;
		transfer.ctx = device->ctx;
		transfer.samples = slot->buf;
		transfer.sample_count = sample_count;
		transfer.sample_type = slot->sample_type;
		transfer.dropped_samples = dropped_samples(&slot->meta, slot->meta_valid, iq, slot->dropped_buffers, sample_count);
		device->cb_meta = &slot->meta;
		device->cb_meta_valid = slot->meta_valid;

		if (run_callback(device, &transfer) != 0)
		{
			device->streaming = false;
		}

		pthread_mutex_lock(&device->dsp_mp);
		device->dsp_tail = (device->dsp_tail + 1) % DSP_SLOTS;
		device->dsp_count--;
		pthread_cond_broadcast(&device->dsp_cv);
	}
	device->streaming = false;
	pthread_cond_broadcast(&device->dsp_cv);
	pthread_mutex_unlock(&device->dsp_mp);

	return NULL;
}

static void* consumer_threadproc(void *arg)
{
	int sample_count;
	uint16_t* input_samples;
	uint32_t dropped_buffers;
	bool deframed;
	enum lpc4370sdr_sample_type sample_type;
	lpc4370sdr_device_t* device = (lpc4370sdr_device_t*)arg;
	lpc4370sdr_transfer_t transfer;

#ifdef _WIN32

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

#endif

	pthread_mutex_lock(&device->consumer_mp);

	while (device->streaming && !device->stop_requested)
	{
		while (device->received_buffer_count == 0 && device->streaming && !device->stop_requested)
		{
			pthread_cond_wait(&device->consumer_cv, &device->consumer_mp);
		}
		if (!device->streaming || device->stop_requested)
		{
			break;
		}

		input_samples = device->received_samples_queue[device->received_samples_queue_tail];
		dropped_buffers = device->dropped_buffers_queue[device->received_samples_queue_tail];
		device->received_samples_queue_tail = (device->received_samples_queue_tail + 1) & (RAW_BUFFER_COUNT - 1);

		pthread_mutex_unlock(&device->consumer_mp);

		/* One sample type for the whole buffer */
		sample_type = device->sample_type;

		deframed = device->framing_active || device->packing_mode == 2;
		if (deframed)
		{
			sample_count = deframe_buffer(device, (const uint8_t*)input_samples, sample_type, dropped_buffers);
			if (sample_count == 0)
			{
				pthread_mutex_lock(&device->consumer_mp);
				device->received_buffer_count--;
				continue;
			}
			input_samples = (uint16_t*)device->framed_samples;
		}
		else if (device->packing_mode == 3)
		{
			sample_count = device->buffer_size;
		}
		else if (device->packing_enabled)
		{
			sample_count = ((device->buffer_size / 2) * 4) / 3;
		}
		else
		{
			sample_count = device->buffer_size / 2;
		}

		if (deframed)
		{
			/* deframe_buffer() unpacked them already */
		}
		else if (device->packing_mode == 3 && sample_type != LPC4370SDR_SAMPLE_RAW)
		{
			unpack_samples_8bit((const uint8_t*)input_samples, device->unpacked_samples, sample_count);
			input_samples = device->unpacked_samples;
		}
		else if (device->packing_enabled && sample_type != LPC4370SDR_SAMPLE_RAW)
		{
			unpack_samples((uint32_t*)input_samples, device->unpacked_samples, sample_count);
			input_samples = device->unpacked_samples;
		}
#ifdef LPC4370SDR_BIG_ENDIAN
		else if (!device->packing_enabled && sample_type != LPC4370SDR_SAMPLE_RAW)
		{
			int i;
			for (i = 0; i < sample_count; i++)
				input_samples[i] = TO_LE16(input_samples[i]);
		}
#endif

		if (device->dsp_active)
		{
			if (!dsp_queue_block(device, sample_type, input_samples, sample_count, dropped_buffers))
			{
				pthread_mutex_lock(&device->consumer_mp);
				device->received_buffer_count--;
				break;
			}
			pthread_mutex_lock(&device->consumer_mp);
			device->received_buffer_count--;
			continue;
		}

		switch (sample_type)
		{
		case LPC4370SDR_SAMPLE_FLOAT32_IQ:
			convert_samples_float(input_samples, (float *)device->output_buffer, sample_count);
			iqconverter_float_process(device->cnv_f, (float *) device->output_buffer, sample_count);
			sample_count /= 2;
			transfer.samples = device->output_buffer;
			break;

		case LPC4370SDR_SAMPLE_FLOAT32_REAL:
			convert_samples_float(input_samples, (float *)device->output_buffer, sample_count);
			transfer.samples = device->output_buffer;
			break;

		case LPC4370SDR_SAMPLE_INT16_IQ:
			convert_samples_int16(input_samples, (int16_t *)device->output_buffer, sample_count);
			iqconverter_int16_process(device->cnv_i, (int16_t *) device->output_buffer, sample_count);
			sample_count /= 2;
			transfer.samples = device->output_buffer;
			break;

		case LPC4370SDR_SAMPLE_INT16_REAL:
			convert_samples_int16(input_samples, (int16_t *)device->output_buffer, sample_count);
			transfer.samples = device->output_buffer;
			break;

		case LPC4370SDR_SAMPLE_UINT16_REAL:
		case LPC4370SDR_SAMPLE_RAW:
			transfer.samples = input_samples;
			break;

		case LPC4370SDR_SAMPLE_END:
			// Just to shut GCC's moaning
			break;
		}

		transfer.device = device;
		transfer.ctx = device->ctx;
		transfer.sample_count = sample_count;
		transfer.sample_type = sample_type;
		transfer.dropped_samples = dropped_samples(&device->meta, device->meta_valid, SAMPLE_TYPE_IS_IQ(sample_type),
			dropped_buffers, sample_count);
		device->cb_meta = &device->meta;
		device->cb_meta_valid = device->meta_valid;

		if (run_callback(device, &transfer) != 0)
		{
			device->streaming = false;
		}

		pthread_mutex_lock(&device->consumer_mp);
		device->received_buffer_count--;
	}

	device->streaming = false;

	pthread_mutex_unlock(&device->consumer_mp);

	/* wake the DSP thread, it stops too */
	pthread_mutex_lock(&device->dsp_mp);
	pthread_cond_broadcast(&device->dsp_cv);
	pthread_mutex_unlock(&device->dsp_mp);

	return NULL;
}

static void lpc4370sdr_libusb_transfer_callback(struct libusb_transfer* usb_transfer)
{
	uint16_t *temp;
	lpc4370sdr_device_t* device = (lpc4370sdr_device_t*)usb_transfer->user_data;

	if (!device->streaming || device->stop_requested)
	{
		in_flight_add(device, -1);
		return;
	}

	if (usb_transfer->status == LIBUSB_TRANSFER_COMPLETED && usb_transfer->actual_length == usb_transfer->length)
	{
		pthread_mutex_lock(&device->consumer_mp);

		if (device->received_buffer_count < RAW_BUFFER_COUNT)
		{
			temp = device->received_samples_queue[device->received_samples_queue_head];
			device->received_samples_queue[device->received_samples_queue_head] = (uint16_t *)usb_transfer->buffer;
			usb_transfer->buffer = (uint8_t *)temp;

			device->dropped_buffers_queue[device->received_samples_queue_head] = device->dropped_buffers;
			device->dropped_buffers = 0;
			
			device->received_samples_queue_head = (device->received_samples_queue_head + 1) & (RAW_BUFFER_COUNT - 1);
			device->received_buffer_count++;

			pthread_cond_signal(&device->consumer_cv);
		}
		else
		{
			device->dropped_buffers++;
		}

		pthread_mutex_unlock(&device->consumer_mp);

		if (libusb_submit_transfer(usb_transfer) != 0)
		{
			in_flight_add(device, -1);
			device->streaming = false;
		}
	}
	else
	{
		in_flight_add(device, -1);
		device->streaming = false;
	}
}

static void* transfer_threadproc(void* arg)
{
	lpc4370sdr_device_t* device = (lpc4370sdr_device_t*)arg;
	int error;
	struct timeval timeout = { 0, 500000 };

#ifdef _WIN32

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

#endif

	while (device->streaming && !device->stop_requested)
	{
		error = libusb_handle_events_timeout_completed(device->usb_context, &timeout, NULL);
		if (error < 0)
		{
			if (error != LIBUSB_ERROR_INTERRUPTED)
				device->streaming = false;
		}
	}
	
	device->streaming = false;

	return NULL;
}

static bool io_threads_running(const lpc4370sdr_device_t* device)
{
	return device->transfer_thread_running || device->consumer_thread_running || device->dsp_thread_running;
}

/* The thread that called lpc4370sdr_stop_rx() from its sample callback, once it is done */
static void join_deferred(lpc4370sdr_device_t* device)
{
	if (device->deferred_join_pending && !pthread_equal(pthread_self(), device->deferred_join))
	{
		pthread_join(device->deferred_join, NULL);
		device->deferred_join_pending = false;
	}
}

/* A stream or what is left of one: threads to join, transfers to take back. Settings that
   reallocate or replace what they use wait until lpc4370sdr_stop_rx(). A thread that stopped its
   own stream is joined here, when this is another thread. */
static bool io_busy(lpc4370sdr_device_t* device)
{
	join_deferred(device);
	return device->streaming || device->stop_requested || io_threads_running(device) || in_flight(device) > 0 ||
		device->deferred_join_pending;
}

static bool on_callback_thread(const lpc4370sdr_device_t* device)
{
	return callback_device == device;
}

/*
 * Starting and stopping take turns: lpc4370sdr_stop_rx() can come from a sample callback while
 * the application stops or restarts the stream. A callback thread only tries the lock: the thread
 * holding it may be waiting for that callback to return.
 */
static bool lock_state(lpc4370sdr_device_t* device)
{
	if (on_callback_thread(device))
	{
		return pthread_mutex_trylock(&device->state_mp) == 0;
	}
	pthread_mutex_lock(&device->state_mp);
	return true;
}

/* For the settings that need the stream stopped: true, with state_mp held until the setting is
   done, when there is no stream */
static bool lock_idle(lpc4370sdr_device_t* device)
{
	if (!lock_state(device))
	{
		return false;
	}
	if (io_busy(device))
	{
		pthread_mutex_unlock(&device->state_mp);
		return false;
	}
	return true;
}

static void join_io_thread(lpc4370sdr_device_t* device, pthread_t thread, bool* running)
{
	if (!*running)
	{
		return;
	}
	if (pthread_equal(pthread_self(), thread))
	{
		/* lpc4370sdr_stop_rx() from the sample callback: this thread ends once the callback
		   returns; the next start, stop or close joins it */
		device->deferred_join = thread;
		device->deferred_join_pending = true;
	}
	else
	{
		pthread_join(thread, NULL);
	}
	*running = false;
}

static int kill_io_threads(lpc4370sdr_device_t* device)
{
	if (device->stop_requested)
	{
		device->streaming = false;
		cancel_transfers(device);

		/* Wake every thread before joining any: a consumer waiting for a free DSP slot only
		   hears from the DSP thread, which may be the one stopping the stream */
		pthread_mutex_lock(&device->consumer_mp);
		pthread_cond_broadcast(&device->consumer_cv);
		pthread_mutex_unlock(&device->consumer_mp);
		pthread_mutex_lock(&device->dsp_mp);
		pthread_cond_broadcast(&device->dsp_cv);
		pthread_mutex_unlock(&device->dsp_mp);

		join_io_thread(device, device->transfer_thread, &device->transfer_thread_running);
		join_io_thread(device, device->consumer_thread, &device->consumer_thread_running);
		join_io_thread(device, device->dsp_thread, &device->dsp_thread_running);

		reap_transfers(device);
		device->stop_requested = false;
	}
	join_deferred(device);

	return LPC4370SDR_SUCCESS;
}

static int create_io_threads(lpc4370sdr_device_t* device, lpc4370sdr_sample_block_cb_fn callback)
{
	int result;
	pthread_attr_t attr;

	if (io_busy(device))
	{
		return LPC4370SDR_ERROR_BUSY;
	}

	device->callback = callback;
	device->received_samples_queue_head = 0;
	device->received_samples_queue_tail = 0;
	device->received_buffer_count = 0;
	device->dsp_head = 0;
	device->dsp_tail = 0;
	device->dsp_count = 0;
	device->dsp_active = device->dsp_thread_enabled;
	device->streaming = true;

	result = prepare_transfers(device, LIBUSB_ENDPOINT_IN | 1, (libusb_transfer_cb_fn)lpc4370sdr_libusb_transfer_callback);
	if (result != LPC4370SDR_SUCCESS)
	{
		device->streaming = false;
		reap_transfers(device);
		return result;
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	result = LPC4370SDR_SUCCESS;
	if (device->dsp_active)
	{
		if (pthread_create(&device->dsp_thread, &attr, dsp_threadproc, device) != 0)
			result = LPC4370SDR_ERROR_THREAD;
		else
			device->dsp_thread_running = true;
	}
	if (result == LPC4370SDR_SUCCESS)
	{
		if (pthread_create(&device->consumer_thread, &attr, consumer_threadproc, device) != 0)
			result = LPC4370SDR_ERROR_THREAD;
		else
			device->consumer_thread_running = true;
	}
	if (result == LPC4370SDR_SUCCESS)
	{
		if (pthread_create(&device->transfer_thread, &attr, transfer_threadproc, device) != 0)
			result = LPC4370SDR_ERROR_THREAD;
		else
			device->transfer_thread_running = true;
	}
	pthread_attr_destroy(&attr);

	if (result != LPC4370SDR_SUCCESS)
	{
		device->stop_requested = true;
		kill_io_threads(device);
	}
	return result;
}

static void lpc4370sdr_open_exit(lpc4370sdr_device_t* device)
{
	if (device->usb_device != NULL)
	{
		libusb_release_interface(device->usb_device, 0);
		libusb_close(device->usb_device);
		device->usb_device = NULL;
	}
	libusb_exit(device->usb_context);
	device->usb_context = NULL;
}

static void lpc4370sdr_open_device(lpc4370sdr_device_t* device,
	int* ret,
	uint64_t serial_number_val)
{
	int i;
	int result;
	libusb_device_handle** libusb_dev_handle;
	int serial_number_len;
	libusb_device_handle* dev_handle;
	libusb_device *dev;
	libusb_device** devices = NULL;

	ssize_t cnt;
	int serial_descriptor_index;
	struct libusb_device_descriptor device_descriptor;
	unsigned char serial_number[SERIAL_STRING_MAX + 1];

	libusb_dev_handle = &device->usb_device;
	*libusb_dev_handle = NULL;

	cnt = libusb_get_device_list(device->usb_context, &devices);
	if (cnt < 0)
	{
		*ret = LPC4370SDR_ERROR_NOT_FOUND;
		return;
	}

	i = 0;
	while ((dev = devices[i++]) != NULL)
	{
		libusb_get_device_descriptor(dev, &device_descriptor);

		if (usb_id_known(device_descriptor.idVendor, device_descriptor.idProduct))
		{
			if (serial_number_val != SERIAL_NUMBER_UNUSED)
			{
				serial_descriptor_index = device_descriptor.iSerialNumber;
				if (serial_descriptor_index > 0)
				{
					if (libusb_open(dev, libusb_dev_handle) != 0)
					{
						*libusb_dev_handle = NULL;
						continue;
					}
					dev_handle = *libusb_dev_handle;
					serial_number_len = libusb_get_string_descriptor_ascii(dev_handle,
						serial_descriptor_index,
						serial_number,
						sizeof(serial_number));
					uint64_t serial = 0;
					if (parse_serial(serial_number, serial_number_len, &serial))
					{
						if (serial == serial_number_val)
						{
#ifdef __linux__
							/* Check whether a kernel driver is attached to interface #0. If so, we'll
							* need to detach it.
							*/
							if (libusb_kernel_driver_active(dev_handle, 0))
							{
								libusb_detach_kernel_driver(dev_handle, 0);
							}
#endif
							result = libusb_set_configuration(dev_handle, 1);
							if (result != 0)
							{
								libusb_close(dev_handle);
								*libusb_dev_handle = NULL;
								continue;
							}
							result = libusb_claim_interface(dev_handle, 0);
							if (result != 0)
							{
								libusb_close(dev_handle);
								*libusb_dev_handle = NULL;
								continue;
							}
							break;
						}
						else
						{
							libusb_close(dev_handle);
							*libusb_dev_handle = NULL;
							continue;
						}
					}
					else
					{
						libusb_close(dev_handle);
						*libusb_dev_handle = NULL;
						continue;
					}
				}
			}
			else
			{
				if (libusb_open(dev, libusb_dev_handle) == 0)
				{
					dev_handle = *libusb_dev_handle;
#ifdef __linux__
					/* Check whether a kernel driver is attached to interface #0. If so, we'll
					* need to detach it.
					*/
					if (libusb_kernel_driver_active(dev_handle, 0))
					{
						libusb_detach_kernel_driver(dev_handle, 0);
					}
#endif
					result = libusb_set_configuration(dev_handle, 1);
					if (result != 0)
					{
						libusb_close(dev_handle);
						*libusb_dev_handle = NULL;
						continue;
					}
					result = libusb_claim_interface(dev_handle, 0);
					if (result != 0)
					{
						libusb_close(dev_handle);
						*libusb_dev_handle = NULL;
						continue;
					}
					break;
				}
			}
		}
	}
	libusb_free_device_list(devices, 1);

	dev_handle = device->usb_device;
	if (dev_handle == NULL)
	{
		*ret = LPC4370SDR_ERROR_NOT_FOUND;
		return;
	}

	*ret = LPC4370SDR_SUCCESS;
	return;
}

static void lpc4370sdr_open_device_fd(lpc4370sdr_device_t* device,
	int* ret,
	int fd)
{
	int result = -1;

#ifdef __ANDROID__
	result = libusb_wrap_sys_device(device->usb_context, (intptr_t)fd, &device->usb_device);
#else
	(void)fd;
	device->usb_device = NULL;
	*ret = LPC4370SDR_ERROR_UNSUPPORTED;
	return;
#endif

	if (result != 0 || device->usb_device == NULL)
	{
		*ret = LPC4370SDR_ERROR_LIBUSB;
		return;
	}

#ifdef __linux__
	/* Check whether a kernel driver is attached to interface #0. If so, we'll
	* need to detach it.
	*/
	if (libusb_kernel_driver_active(device->usb_device, 0))
	{
		libusb_detach_kernel_driver(device->usb_device, 0);
	}
#endif

	result = libusb_set_configuration(device->usb_device, 1);
	if (result != 0)
	{
		libusb_close(device->usb_device);
		device->usb_device = NULL;
		*ret = LPC4370SDR_ERROR_LIBUSB;
		return;
	}

	result = libusb_claim_interface(device->usb_device, 0);
	if (result != 0)
	{
		libusb_close(device->usb_device);
		device->usb_device = NULL;
		*ret = LPC4370SDR_ERROR_LIBUSB;
		return;
	}

	*ret = LPC4370SDR_SUCCESS;
	return;
}

/* The firmware lists at most 64 sample rates */
#define SAMPLERATES_MAX (64)

static int lpc4370sdr_read_samplerates_from_fw(struct lpc4370sdr_device* device, uint32_t* buffer, const uint32_t len)
{
	int result;
	uint32_t i;

	result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_GET_SAMPLERATES,
		0,
		len,
		(unsigned char*)buffer,
		(len > 0 ? len : 1) * sizeof(uint32_t),
		LIBUSB_CTRL_TIMEOUT_MS);

	if (result != (int)((len > 0 ? len : 1) * sizeof(uint32_t)))
	{
		return LPC4370SDR_ERROR_OTHER;
	}
	for (i = 0; i < (len > 0 ? len : 1); i++)
	{
		buffer[i] = TO_LE(buffer[i]);
	}

	return LPC4370SDR_SUCCESS;
}

static int lpc4370sdr_open_init(lpc4370sdr_device_t** device, uint64_t serial_number, int fd)
{
	lpc4370sdr_device_t* lib_device;
	int libusb_error;
	int result;

	*device = NULL;

	lib_device = (lpc4370sdr_device_t*)calloc(1, sizeof(lpc4370sdr_device_t));
	if (lib_device == NULL)
	{
		return LPC4370SDR_ERROR_NO_MEM;
	}
	/* Before anything that may take them, lpc4370sdr_set_packing() below included */
	pthread_cond_init(&lib_device->consumer_cv, NULL);
	pthread_mutex_init(&lib_device->consumer_mp, NULL);
	pthread_cond_init(&lib_device->dsp_cv, NULL);
	pthread_mutex_init(&lib_device->dsp_mp, NULL);
	pthread_mutex_init(&lib_device->state_mp, NULL);

#ifdef __ANDROID__
	// LibUSB does not support device discovery on android
	libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

	libusb_error = libusb_init(&lib_device->usb_context);
	if (libusb_error != 0)
	{
		free(lib_device);
		return LPC4370SDR_ERROR_LIBUSB;
	}

	if (fd == FILE_DESCRIPTOR_UNUSED) {
		lpc4370sdr_open_device(lib_device,
			&result,
			serial_number);
	}
	else {
		lpc4370sdr_open_device_fd(lib_device,
			&result,
			fd);
	}

	if (lib_device->usb_device == NULL)
	{
		libusb_exit(lib_device->usb_context);
		free(lib_device);
		return result;
	}

	lib_device->transfers = NULL;
	lib_device->callback = NULL;
	lib_device->transfer_count = 16;
	lib_device->buffer_size = 262144;
	lib_device->packing_enabled = false;
	lib_device->packing_mode = 0;
	lib_device->dev_mem_buffers = false;
	lib_device->framing_requested = true;
	lib_device->gap_fill = false;
	{
		const char* env_ppb = getenv("LPC4370SDR_CALIBRATION_PPB");
		if (env_ppb != NULL && *env_ppb != 0)
		{
			char* end;
			long ppb = strtol(env_ppb, &end, 10);
			if (*end == 0 && ppb >= INT32_MIN && ppb <= INT32_MAX)
				lpc4370sdr_set_calibration(lib_device, (int32_t)ppb);
		}
	}
	lib_device->framing_active = false;
	lib_device->framed_samples = NULL;
	lib_device->frame_prev = NULL;
	lib_device->usb_buf_slow = false;
	lib_device->dc_removal = true;
	lib_device->dsp_thread_enabled = (getenv("LPC4370SDR_NO_DSP_THREAD") == NULL);
	lib_device->dsp_active = false;
	lib_device->dsp_thread_running = false;
	lib_device->cb_meta = NULL;
	lib_device->cb_meta_valid = false;
	{
		/* HydraSDR firmware takes the frequency as 64 bits; the Airspy firmware stops on anything but 32.
		   The RFOne's version string starts with "HydraSDR" with either firmware: it has antenna inputs to
		   select (the Airspy boards do not; HydraSDR firmware on them would power down their LNA). */
		char version[256];
		lib_device->freq_64bit = lpc4370sdr_version_string_read(lib_device, version, sizeof(version) - 1) == LPC4370SDR_SUCCESS &&
			strncmp(version, "HydraSDR", 8) == 0;
		lib_device->rf_port_select = lib_device->freq_64bit;
	}
	lib_device->meta_valid = false;
	lib_device->expected_sample_index_valid = false;
	lib_device->sof_count_seen = 0;
	lib_device->pps_count_seen = 0;
	lib_device->chunk_ext_valid = false;
	lib_device->counts_ext_valid = false;
	lib_device->sof_frame_ext_valid = false;
	lib_device->version_noted = false;
	lib_device->uart_total = 0;
	lib_device->sof_divider = 0;
	memset(&lib_device->meta, 0, sizeof(lib_device->meta));
	lib_device->streaming = false;
	lib_device->stop_requested = false;
	lib_device->sample_type = LPC4370SDR_SAMPLE_FLOAT32_IQ;

	lib_device->supported_samplerates = NULL;
	result = lpc4370sdr_read_samplerates_from_fw(lib_device, &lib_device->supported_samplerate_count, 0);
	if (result == LPC4370SDR_SUCCESS && lib_device->supported_samplerate_count > 0 && lib_device->supported_samplerate_count <= SAMPLERATES_MAX)
	{
		lib_device->supported_samplerates = (uint32_t *) malloc(lib_device->supported_samplerate_count * sizeof(uint32_t));
		if (lib_device->supported_samplerates == NULL ||
			lpc4370sdr_read_samplerates_from_fw(lib_device, lib_device->supported_samplerates, lib_device->supported_samplerate_count) != LPC4370SDR_SUCCESS)
		{
			free(lib_device->supported_samplerates);
			lib_device->supported_samplerates = NULL;
		}
	}

	if (lib_device->supported_samplerates == NULL)
	{
		lib_device->supported_samplerate_count = 2;
		lib_device->supported_samplerates = (uint32_t *) malloc(lib_device->supported_samplerate_count * sizeof(uint32_t));
		if (lib_device->supported_samplerates == NULL)
		{
			lpc4370sdr_open_exit(lib_device);
			free(lib_device);
			return LPC4370SDR_ERROR_NO_MEM;
		}
		lib_device->supported_samplerates[0] = 10000000;
		lib_device->supported_samplerates[1] = 2500000;
	}

	lpc4370sdr_set_packing(lib_device, 0);

	lib_device->cnv_f = iqconverter_float_create(HB_KERNEL_FLOAT, HB_KERNEL_FLOAT_LEN);
	lib_device->cnv_i = iqconverter_int16_create(HB_KERNEL_INT16, HB_KERNEL_INT16_LEN);
	/* lpc4370sdr_set_packing() allocated them, unless the device did not answer */
	result = (lib_device->transfers == NULL) ? allocate_transfers(lib_device) : LPC4370SDR_SUCCESS;
	if (result != LPC4370SDR_SUCCESS || lib_device->cnv_f == NULL || lib_device->cnv_i == NULL)
	{
		free_transfers(lib_device);
		iqconverter_float_free(lib_device->cnv_f);
		iqconverter_int16_free(lib_device->cnv_i);
		lpc4370sdr_open_exit(lib_device);
		free(lib_device->supported_samplerates);
		free(lib_device);
		return LPC4370SDR_ERROR_NO_MEM;
	}

	*device = lib_device;

	return LPC4370SDR_SUCCESS;
}

#ifdef __cplusplus
extern "C"
{
#endif

	void ADDCALL lpc4370sdr_lib_version(lpc4370sdr_lib_version_t* lib_version)
	{
		lib_version->major_version = LPC4370SDR_VER_MAJOR;
		lib_version->minor_version = LPC4370SDR_VER_MINOR;
		lib_version->revision = LPC4370SDR_VER_REVISION;
	}

	/* lpc4370sdr_init() deprecated */
	int ADDCALL lpc4370sdr_init(void)
	{
		return LPC4370SDR_SUCCESS;
	}

	/* lpc4370sdr_exit() deprecated */
	int ADDCALL lpc4370sdr_exit(void)
	{
		return LPC4370SDR_SUCCESS;
	}

int ADDCALL lpc4370sdr_list_devices(uint64_t *serials, int count)
{
	libusb_device_handle* libusb_dev_handle;
	struct libusb_context *context;
	libusb_device** devices = NULL;
	libusb_device *dev;
	struct libusb_device_descriptor device_descriptor;

	int serial_descriptor_index;
	int serial_number_len;
	int output_count;
	int i;
	unsigned char serial_number[SERIAL_STRING_MAX + 1];

	if (serials)
	{
		if (count < 0)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		memset(serials, 0, sizeof(uint64_t) * count);
	}

#ifdef __ANDROID__
	// LibUSB does not support device discovery on android
	libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

	if (libusb_init(&context) != 0)
	{
		return LPC4370SDR_ERROR_LIBUSB;
	}

	if (libusb_get_device_list(context, &devices) < 0)
	{
		libusb_exit(context);
		return LPC4370SDR_ERROR_NOT_FOUND;
	}

	i = 0;
	output_count = 0;
	while ((dev = devices[i++]) != NULL && (!serials || output_count < count))
	{
		libusb_get_device_descriptor(dev, &device_descriptor);

		if (usb_id_known(device_descriptor.idVendor, device_descriptor.idProduct))
		{
			serial_descriptor_index = device_descriptor.iSerialNumber;
			if (serial_descriptor_index > 0)
			{
				if (libusb_open(dev, &libusb_dev_handle) != 0)
				{
					continue;
				}

				serial_number_len = libusb_get_string_descriptor_ascii(libusb_dev_handle,
					serial_descriptor_index,
					serial_number,
					sizeof(serial_number));

				uint64_t serial;
				if (parse_serial(serial_number, serial_number_len, &serial))
				{
					if (serials)
					{
						serials[output_count] = serial;
					}
					output_count++;
				}

				libusb_close(libusb_dev_handle);
			}
		}
	}

	libusb_free_device_list(devices, 1);
	libusb_exit(context);
	return output_count;
}

	int ADDCALL lpc4370sdr_open_sn(lpc4370sdr_device_t** device, uint64_t serial_number)
	{
		int result;

		result = lpc4370sdr_open_init(device, serial_number, FILE_DESCRIPTOR_UNUSED);
		return result;
	}

	int ADDCALL lpc4370sdr_open_fd(lpc4370sdr_device_t** device, int fd)
	{
		int result;

		result = lpc4370sdr_open_init(device, SERIAL_NUMBER_UNUSED, fd);
		return result;
	}

	int ADDCALL lpc4370sdr_open(lpc4370sdr_device_t** device)
	{
		int result;

		result = lpc4370sdr_open_init(device, SERIAL_NUMBER_UNUSED, FILE_DESCRIPTOR_UNUSED);
		return result;
	}

	int ADDCALL lpc4370sdr_close(lpc4370sdr_device_t* device)
	{
		int result;

		result = LPC4370SDR_SUCCESS;

		if (device != NULL)
		{
			/* Not from the sample callback: the device goes away under the thread that called it */
			if (on_callback_thread(device))
			{
				return LPC4370SDR_ERROR_BUSY;
			}

			result = lpc4370sdr_stop_rx(device);

			iqconverter_float_free(device->cnv_f);
			iqconverter_int16_free(device->cnv_i);

			pthread_cond_destroy(&device->consumer_cv);
			pthread_mutex_destroy(&device->consumer_mp);
			pthread_cond_destroy(&device->dsp_cv);
			pthread_mutex_destroy(&device->dsp_mp);
			pthread_mutex_destroy(&device->state_mp);

			free_transfers(device);
			lpc4370sdr_open_exit(device);
			free(device->supported_samplerates);
			free(device);
		}

		return result;
	}

	int ADDCALL lpc4370sdr_get_samplerates(struct lpc4370sdr_device* device, uint32_t* buffer, const uint32_t len)
	{
		uint32_t i;

		if (len == 0)
		{
			*buffer = device->supported_samplerate_count;
		}
		else if (len <= device->supported_samplerate_count)
		{
			memcpy(buffer, device->supported_samplerates, len * sizeof(uint32_t));

			if (!SAMPLE_TYPE_IS_IQ(device->sample_type))
			{
				for (i = 0; i < len; i++)
				{
					buffer[i] *= 2;
				}
			}
		}
		else
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_samplerate(lpc4370sdr_device_t* device, uint32_t samplerate)
	{
		int result;
		uint8_t retval;
		uint8_t length;
		uint32_t i;

		if (samplerate >= MIN_SAMPLERATE_BY_VALUE)
		{
			for (i = 0; i < device->supported_samplerate_count; i++)
			{
				if (samplerate == device->supported_samplerates[i])
				{
					samplerate = i;
					break;
				}
			}

			if (samplerate >= MIN_SAMPLERATE_BY_VALUE)
			{
				if (SAMPLE_TYPE_IS_IQ(device->sample_type))
				{
					samplerate *= 2;
				}
				samplerate /= 1000;
			}
		}

		libusb_clear_halt(device->usb_device, LIBUSB_ENDPOINT_IN | 1);

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_SAMPLERATE,
			0,
			samplerate,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	/* Internal: lpc4370sdr_start_rx() and lpc4370sdr_stop_rx() switch the receiver */
	static int lpc4370sdr_set_receiver_mode(lpc4370sdr_device_t* device, receiver_mode_t value)
	{
		int result;
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_RECEIVER_MODE,
			value,
			0,
			NULL,
			0,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	static int start_rx_locked(lpc4370sdr_device_t* device, lpc4370sdr_sample_block_cb_fn callback, void* ctx)
	{
		int result;

		if (device->streaming || on_callback_thread(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		/* A stream that ended by itself (the callback returned non-zero, or USB failed) without
		   lpc4370sdr_stop_rx(): take its threads and transfers back first */
		if (io_busy(device))
		{
			device->stop_requested = true;
			kill_io_threads(device);
		}
		/* After a failed reallocation in lpc4370sdr_set_packing() */
		if (device->transfers == NULL)
		{
			result = allocate_transfers(device);
			if (result != LPC4370SDR_SUCCESS)
			{
				return result;
			}
		}

		iqconverter_float_reset(device->cnv_f);
		iqconverter_int16_reset(device->cnv_i);

		memset(device->dropped_buffers_queue, 0, RAW_BUFFER_COUNT * sizeof(uint32_t));
		device->dropped_buffers = 0;
		memset(&device->meta, 0, sizeof(device->meta));
		device->meta_valid = false;
		device->expected_sample_index_valid = false;
		device->sof_count_seen = 0;
		device->pps_count_seen = 0;
		device->chunk_ext_valid = false;
		device->counts_ext_valid = false;
		device->sof_frame_ext_valid = false;
		device->uart_total = 0;
		device->frame_prev_len = 0;
		device->frame_carry_len = 0;
		device->chunk_index_valid = false;
		device->stream_start_valid = false;
		device->meta_pending = false;
		device->frame_tail = false;

		result = lpc4370sdr_set_receiver_mode(device, RECEIVER_MODE_OFF);
		if (result != LPC4370SDR_SUCCESS)
		{
			return result;
		}

		libusb_clear_halt(device->usb_device, LIBUSB_ENDPOINT_IN | 1);

		device->framing_active = false;
		if (device->framing_requested)
		{
			device->framing_active = (send_framing(device, 1) == LPC4370SDR_SUCCESS);
		}
		if (device->sof_divider != 0)
		{
			result = send_sof_divider(device, device->sof_divider);
			if (result != LPC4370SDR_SUCCESS)
			{
				return result;
			}
		}

		result = lpc4370sdr_set_receiver_mode(device, RECEIVER_MODE_RX);
		if (result == LPC4370SDR_SUCCESS)
		{
			device->ctx = ctx;
			result = create_io_threads(device, callback);
		}

		return result;
	}

	int ADDCALL lpc4370sdr_start_rx(lpc4370sdr_device_t* device, lpc4370sdr_sample_block_cb_fn callback, void* ctx)
	{
		int result;

		if (!lock_state(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = start_rx_locked(device, callback, ctx);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	int ADDCALL lpc4370sdr_stop_rx(lpc4370sdr_device_t* device)
	{
		int result1, result2;

		if (!lock_state(device))
		{
			/* A sample callback while another thread stops or starts the stream */
			device->streaming = false;
			return LPC4370SDR_SUCCESS;
		}
		device->stop_requested = true;
		result1 = lpc4370sdr_set_receiver_mode(device, RECEIVER_MODE_OFF);
		result2 = kill_io_threads(device);
		pthread_mutex_unlock(&device->state_mp);

		if (result1 != LPC4370SDR_SUCCESS)
		{
			return result1;
		}
		return result2;
	}

	int ADDCALL lpc4370sdr_si5351c_read(lpc4370sdr_device_t* device, uint8_t register_number, uint8_t* value)
	{
		uint8_t temp_value;
		int result;

		temp_value = 0;
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SI5351C_READ,
			0,
			register_number,
			(unsigned char*)&temp_value,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			*value = temp_value;
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_si5351c_write(lpc4370sdr_device_t* device, uint8_t register_number, uint8_t value)
	{
		int result;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SI5351C_WRITE,
			value,
			register_number,
			NULL,
			0,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_r820t_read(lpc4370sdr_device_t* device, uint8_t register_number, uint8_t* value)
	{
		int result;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_R820T_READ,
			0,
			register_number,
			(unsigned char*)value,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_r820t_write(lpc4370sdr_device_t* device, uint8_t register_number, uint8_t value)
	{
		int result;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_R820T_WRITE,
			value,
			register_number,
			NULL,
			0,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_gpio_read(lpc4370sdr_device_t* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t* value)
	{
		int result;
		uint8_t port_pin;

		port_pin = ((uint8_t)port) << 5;
		port_pin = port_pin | pin;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_GPIO_READ,
			0,
			port_pin,
			(unsigned char*)value,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_gpio_write(lpc4370sdr_device_t* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t value)
	{
		int result;
		uint8_t port_pin;

		port_pin = ((uint8_t)port) << 5;
		port_pin = port_pin | pin;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_GPIO_WRITE,
			value,
			port_pin,
			NULL,
			0,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}


	int ADDCALL lpc4370sdr_gpiodir_read(lpc4370sdr_device_t* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t* value)
	{
		int result;
		uint8_t port_pin;

		port_pin = ((uint8_t)port) << 5;
		port_pin = port_pin | pin;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_GPIODIR_READ,
			0,
			port_pin,
			(unsigned char*)value,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_gpiodir_write(lpc4370sdr_device_t* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t value)
	{
		int result;
		uint8_t port_pin;

		port_pin = ((uint8_t)port) << 5;
		port_pin = port_pin | pin;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_GPIODIR_WRITE,
			value,
			port_pin,
			NULL,
			0,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_spiflash_erase(lpc4370sdr_device_t* device)
	{
		int result;
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SPIFLASH_ERASE,
			0,
			0,
			NULL,
			0,
			SPIFLASH_ERASE_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_spiflash_erase_sector(lpc4370sdr_device_t* device, const uint16_t sector_num)
	{
		int result;
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SPIFLASH_ERASE_SECTOR,
			sector_num,
			0,
			NULL,
			0,
			SPIFLASH_ERASE_TIMEOUT_MS);

		if (result != 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_spiflash_write(lpc4370sdr_device_t* device, const uint32_t address, const uint16_t length, unsigned char* const data)
	{
		int result;

		if (address > 0x0FFFFF)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SPIFLASH_WRITE,
			address >> 16,
			address & 0xFFFF,
			data,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_spiflash_read(lpc4370sdr_device_t* device, const uint32_t address, const uint16_t length, unsigned char* data)
	{
		int result;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SPIFLASH_READ,
			address >> 16,
			address & 0xFFFF,
			data,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_get_stream_status(lpc4370sdr_device_t* device, lpc4370sdr_stream_status_t* status)
	{
		int result;
		uint32_t* words = (uint32_t*)status;
		size_t i;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_GET_STREAM_STATUS,
			0,
			0,
			(unsigned char*)status,
			sizeof(lpc4370sdr_stream_status_t),
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < (int)(13 * sizeof(uint32_t))) /* an older firmware stops after pps_count */
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		if (result < (int)sizeof(lpc4370sdr_stream_status_t))
		{
			memset((uint8_t*)status + result, 0, sizeof(lpc4370sdr_stream_status_t) - result);
		}

		for (i = 0; i < sizeof(lpc4370sdr_stream_status_t) / sizeof(uint32_t); i++)
		{
			words[i] = TO_LE(words[i]);
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_board_id_read(lpc4370sdr_device_t* device, uint8_t* value)
	{
		int result;
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_BOARD_ID_READ,
			0,
			0,
			value,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_version_string_read(lpc4370sdr_device_t* device, char* version, uint8_t length)
	{
#define VERSION_LOCAL_SIZE (128)
		int result;
		char version_local[VERSION_LOCAL_SIZE] = "";

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_VERSION_STRING_READ,
			0,
			0,
			(unsigned char*)version_local,
			(VERSION_LOCAL_SIZE - 1),
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 0)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else
		{
			if (length > 0)
			{
				const int num_bytes_to_copy = (length > VERSION_LOCAL_SIZE ? VERSION_LOCAL_SIZE : length) - 1;
				memcpy(version, version_local, num_bytes_to_copy);
				version[num_bytes_to_copy] = 0;
				return LPC4370SDR_SUCCESS;
			}
			else
			{
				return LPC4370SDR_ERROR_INVALID_PARAM;
			}
		}
	}

	int ADDCALL lpc4370sdr_board_partid_serialno_read(lpc4370sdr_device_t* device, lpc4370sdr_read_partid_serialno_t* read_partid_serialno)
	{
		uint8_t length;
		int result;

		length = sizeof(lpc4370sdr_read_partid_serialno_t);
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_BOARD_PARTID_SERIALNO_READ,
			0,
			0,
			(unsigned char*)read_partid_serialno,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {

			read_partid_serialno->part_id[0] = TO_LE(read_partid_serialno->part_id[0]);
			read_partid_serialno->part_id[1] = TO_LE(read_partid_serialno->part_id[1]);
			read_partid_serialno->serial_no[0] = TO_LE(read_partid_serialno->serial_no[0]);
			read_partid_serialno->serial_no[1] = TO_LE(read_partid_serialno->serial_no[1]);
			read_partid_serialno->serial_no[2] = TO_LE(read_partid_serialno->serial_no[2]);
			read_partid_serialno->serial_no[3] = TO_LE(read_partid_serialno->serial_no[3]);

			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_usb_position(struct lpc4370sdr_device* device, uint8_t* busnum, uint8_t* devnum, char* port, size_t port_len)
	{
		libusb_device* dev = libusb_get_device(device->usb_device);
		const uint8_t bus = libusb_get_bus_number(dev);

		if (busnum != NULL)
		{
			*busnum = bus;
		}
		if (devnum != NULL)
		{
			*devnum = libusb_get_device_address(dev);
		}
		if (port != NULL)
		{
			/* USB 3 allows 7 tiers of hubs below the root */
			uint8_t ports[7];
			const int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
			size_t used;
			int i;

			if (n < 0)
			{
				return LPC4370SDR_ERROR_LIBUSB;
			}
			used = (size_t)snprintf(port, port_len, "%u", bus);
			for (i = 0; i < n && used < port_len; i++)
			{
				used += (size_t)snprintf(port + used, port_len - used, i == 0 ? "-%u" : ".%u", ports[i]);
			}
			if (used >= port_len)
			{
				return LPC4370SDR_ERROR_INVALID_PARAM;
			}
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_sample_type(struct lpc4370sdr_device* device, enum lpc4370sdr_sample_type sample_type)
	{
		if ((unsigned)sample_type >= LPC4370SDR_SAMPLE_END)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		/* The buffers of a stream are sized for its sample type */
		if (sample_type != device->sample_type)
		{
			if (!lock_idle(device))
			{
				return LPC4370SDR_ERROR_BUSY;
			}
			device->sample_type = sample_type;
			pthread_mutex_unlock(&device->state_mp);
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_freq(lpc4370sdr_device_t* device, const uint32_t freq_hz)
	{
		set_freq_params_t set_freq_params;
		unsigned char freq64[8];
		unsigned char* data = (unsigned char*)&set_freq_params;
		uint8_t length;
		int result;
		int i;

		set_freq_params.freq_hz = TO_LE(freq_hz);
		length = sizeof(set_freq_params_t);
		if (device->freq_64bit)
		{
			/* 64-bit little-endian */
			for (i = 0; i < 8; i++)
				freq64[i] = (unsigned char)((uint64_t)freq_hz >> (8 * i));
			data = freq64;
			length = sizeof(freq64);
		}

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_FREQ,
			0,
			0,
			data,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	static int lpc4370sdr_set_conversion_filter_float32_locked(struct lpc4370sdr_device* device, const float *kernel, const uint32_t len)
	{
		iqconverter_float_t *cnv;

		if (len > INT32_MAX)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		cnv = iqconverter_float_create(kernel, (int)len);
		if (cnv == NULL)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		cnv->dc_removal = device->dc_removal;
		iqconverter_float_free(device->cnv_f);
		device->cnv_f = cnv;

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_conversion_filter_float32(struct lpc4370sdr_device* device, const float *kernel, const uint32_t len)
	{
		int result;

		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = lpc4370sdr_set_conversion_filter_float32_locked(device, kernel, len);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	static int lpc4370sdr_set_conversion_filter_int16_locked(struct lpc4370sdr_device* device, const int16_t *kernel, const uint32_t len)
	{
		iqconverter_int16_t *cnv;

		if (len > INT32_MAX)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		cnv = iqconverter_int16_create(kernel, (int)len);
		if (cnv == NULL)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		cnv->dc_removal = device->dc_removal;
		iqconverter_int16_free(device->cnv_i);
		device->cnv_i = cnv;

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_conversion_filter_int16(struct lpc4370sdr_device* device, const int16_t *kernel, const uint32_t len)
	{
		int result;

		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = lpc4370sdr_set_conversion_filter_int16_locked(device, kernel, len);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	int ADDCALL lpc4370sdr_get_conversion_delay(struct lpc4370sdr_device* device, double* delay, uint32_t* decimation)
	{
		double d = 0;
		uint32_t m = 1;

		if (device == NULL)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		pthread_mutex_lock(&device->state_mp);
		/* the converters keep half the kernel and its centre: len = L / 2 + 1, so (L - 1) / 4 = (len - 1) / 2 */
		if (device->sample_type == LPC4370SDR_SAMPLE_FLOAT32_IQ)
		{
			d = (device->cnv_f->len - 1) / 2.0;
			m = 2;
		}
		else if (device->sample_type == LPC4370SDR_SAMPLE_INT16_IQ)
		{
			d = (device->cnv_i->len - 1) / 2.0;
			m = 2;
		}
		pthread_mutex_unlock(&device->state_mp);
		if (delay != NULL)
		{
			*delay = d;
		}
		if (decimation != NULL)
		{
			*decimation = m;
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_dc_removal(struct lpc4370sdr_device* device, uint8_t enable)
	{
		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		device->dc_removal = enable != 0;
		device->cnv_f->dc_removal = device->dc_removal;
		device->cnv_i->dc_removal = device->dc_removal;
		pthread_mutex_unlock(&device->state_mp);
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_lna_gain(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		uint8_t length;

		if (value > 14)
			value = 14;

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_LNA_GAIN,
			0,
			value,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_set_mixer_gain(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		uint8_t length;

		if (value > 15)
			value = 15;

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_MIXER_GAIN,
			0,
			value,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_set_vga_gain(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		uint8_t length;

		if (value > 15)
			value = 15;

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_VGA_GAIN,
			0,
			value,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_set_lna_agc(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		uint8_t length;

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_LNA_AGC,
			0,
			value,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_set_mixer_agc(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		uint8_t length;

		length = 1;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_MIXER_AGC,
			0,
			value,
			&retval,
			length,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < length)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		else {
			return LPC4370SDR_SUCCESS;
		}
	}

	int ADDCALL lpc4370sdr_set_linearity_gain(struct lpc4370sdr_device* device, uint8_t value)
	{
		int rc;

		if (value >= GAIN_COUNT)
		{
			value = GAIN_COUNT - 1;
		}

		value = GAIN_COUNT - 1 - value;

		rc = lpc4370sdr_set_mixer_agc(device, 0);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_lna_agc(device, 0);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_vga_gain(device, lpc4370sdr_linearity_vga_gains[value]);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_mixer_gain(device, lpc4370sdr_linearity_mixer_gains[value]);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_lna_gain(device, lpc4370sdr_linearity_lna_gains[value]);
		if (rc < 0)
			return rc;

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_sensitivity_gain(struct lpc4370sdr_device* device, uint8_t value)
	{
		int rc;

		if (value >= GAIN_COUNT)
		{
			value = GAIN_COUNT - 1;
		}

		value = GAIN_COUNT - 1 - value;

		rc = lpc4370sdr_set_mixer_agc(device, 0);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_lna_agc(device, 0);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_vga_gain(device, lpc4370sdr_sensitivity_vga_gains[value]);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_mixer_gain(device, lpc4370sdr_sensitivity_mixer_gains[value]);
		if (rc < 0)
			return rc;

		rc = lpc4370sdr_set_lna_gain(device, lpc4370sdr_sensitivity_lna_gains[value]);
		if (rc < 0)
			return rc;

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_rf_bias(lpc4370sdr_device_t* device, uint8_t value)
	{
		return lpc4370sdr_gpio_write(device, GPIO_PORT1, GPIO_PIN13, value);
	}

	int ADDCALL lpc4370sdr_set_rf_port(lpc4370sdr_device_t* device, lpc4370sdr_rf_port_t rf_port)
	{
		int result;
		uint8_t retval = 0;

		if (!device->rf_port_select)
		{
			return LPC4370SDR_ERROR_UNSUPPORTED;
		}
		if ((unsigned)rf_port > LPC4370SDR_RF_PORT_RX2)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_RF_PORT,
			0,
			(uint16_t)rf_port,
			&retval,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		return (retval == 1) ? LPC4370SDR_SUCCESS : LPC4370SDR_ERROR_OTHER;
	}

	static int lpc4370sdr_set_packing_locked(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;
		uint8_t retval;
		bool packing_enabled;

		/* 0 = 16-bit, 1 = 12-bit, 2 = 12-bit with a chunk counter and the ADC status, 3 = 8-bit */
		if (value > 3)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_SET_PACKING,
			0,
			value,
			&retval,
			1,
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < 1)
		{
			return  LPC4370SDR_ERROR_LIBUSB;
		}

		packing_enabled = value ? true : false;
		if (value != device->packing_mode || device->transfers == NULL)
		{
			result = free_transfers(device);
			if (result != LPC4370SDR_SUCCESS)
			{
				return result;
			}

			device->packing_enabled = packing_enabled;
			device->packing_mode = value;
			device->buffer_size = value == 3 ? (4096 * 36) : packing_enabled ? (6144 * 24) : 262144;

			/* On failure the buffers stay unallocated: lpc4370sdr_start_rx() tries again */
			result = allocate_transfers(device);
			if (result != LPC4370SDR_SUCCESS)
			{
				return result;
			}
		}

		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_packing(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;

		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = lpc4370sdr_set_packing_locked(device, value);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	static int lpc4370sdr_set_dsp_thread_locked(lpc4370sdr_device_t* device, uint8_t enabled)
	{
		device->dsp_thread_enabled = enabled ? true : false;
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_dsp_thread(lpc4370sdr_device_t* device, uint8_t enabled)
	{
		int result;

		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = lpc4370sdr_set_dsp_thread_locked(device, enabled);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	int ADDCALL lpc4370sdr_set_gap_fill(lpc4370sdr_device_t* device, uint8_t enabled)
	{
		device->gap_fill = enabled ? true : false;
		return LPC4370SDR_SUCCESS;
	}

	static int lpc4370sdr_set_framing_locked(lpc4370sdr_device_t* device, uint8_t value)
	{
		device->framing_requested = value ? true : false;
		if (device->framing_requested)
		{
			return send_framing(device, 1);
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_set_framing(lpc4370sdr_device_t* device, uint8_t value)
	{
		int result;

		if (!lock_idle(device))
		{
			return LPC4370SDR_ERROR_BUSY;
		}
		result = lpc4370sdr_set_framing_locked(device, value);
		pthread_mutex_unlock(&device->state_mp);
		return result;
	}

	static int lpc4370sdr_watchdog_request(lpc4370sdr_device_t* device, uint16_t feed, lpc4370sdr_watchdog_status_t* status)
	{
		lpc4370sdr_watchdog_status_t local;
		lpc4370sdr_watchdog_status_t* s = status ? status : &local;
		uint32_t* words = (uint32_t*)s;
		size_t i;
		int result;

		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_WATCHDOG,
			0,
			feed,
			(unsigned char*)s,
			sizeof(lpc4370sdr_watchdog_status_t),
			LIBUSB_CTRL_TIMEOUT_MS);

		if (result < (int)sizeof(lpc4370sdr_watchdog_status_t))
		{
			return LPC4370SDR_ERROR_LIBUSB;
		}
		for (i = 0; i < sizeof(lpc4370sdr_watchdog_status_t) / sizeof(uint32_t); i++)
		{
			words[i] = TO_LE(words[i]);
		}
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_watchdog_status(lpc4370sdr_device_t* device, lpc4370sdr_watchdog_status_t* status)
	{
		return lpc4370sdr_watchdog_request(device, 0, status);
	}

	int ADDCALL lpc4370sdr_watchdog_feed(lpc4370sdr_device_t* device, lpc4370sdr_watchdog_status_t* status)
	{
		return lpc4370sdr_watchdog_request(device, 1, status);
	}

	int ADDCALL lpc4370sdr_transfer_get_metadata(lpc4370sdr_transfer* transfer, lpc4370sdr_transfer_metadata_t* metadata)
	{
		lpc4370sdr_device_t* device;

		if (transfer == NULL || metadata == NULL || transfer->device == NULL)
		{
			return LPC4370SDR_ERROR_INVALID_PARAM;
		}
		device = transfer->device;
		if (!device->framing_active || !device->cb_meta_valid || device->cb_meta == NULL)
		{
			return LPC4370SDR_ERROR_UNSUPPORTED;
		}
		*metadata = *device->cb_meta;
		return LPC4370SDR_SUCCESS;
	}

	int ADDCALL lpc4370sdr_is_streaming(lpc4370sdr_device_t* device)
	{
		return (device->streaming == true && device->stop_requested == false);
	}

	const char* ADDCALL lpc4370sdr_error_name(enum lpc4370sdr_error errcode)
	{
		switch (errcode)
		{
		case LPC4370SDR_SUCCESS:
			return "LPC4370SDR_SUCCESS";

		case LPC4370SDR_TRUE:
			return "LPC4370SDR_TRUE";

		case LPC4370SDR_ERROR_INVALID_PARAM:
			return "LPC4370SDR_ERROR_INVALID_PARAM";

		case LPC4370SDR_ERROR_NOT_FOUND:
			return "LPC4370SDR_ERROR_NOT_FOUND";

		case LPC4370SDR_ERROR_BUSY:
			return "LPC4370SDR_ERROR_BUSY";

		case LPC4370SDR_ERROR_NO_MEM:
			return "LPC4370SDR_ERROR_NO_MEM";

		case LPC4370SDR_ERROR_UNSUPPORTED:
			return "LPC4370SDR_ERROR_UNSUPPORTED";

		case LPC4370SDR_ERROR_LIBUSB:
			return "LPC4370SDR_ERROR_LIBUSB";

		case LPC4370SDR_ERROR_THREAD:
			return "LPC4370SDR_ERROR_THREAD";

		case LPC4370SDR_ERROR_STREAMING_THREAD_ERR:
			return "LPC4370SDR_ERROR_STREAMING_THREAD_ERR";

		case LPC4370SDR_ERROR_STREAMING_STOPPED:
			return "LPC4370SDR_ERROR_STREAMING_STOPPED";

		case LPC4370SDR_ERROR_OTHER:
			return "LPC4370SDR_ERROR_OTHER";

		default:
			return "lpc4370sdr unknown error";
		}
	}

	const char* ADDCALL lpc4370sdr_board_id_name(enum lpc4370sdr_board_id board_id)
	{
		switch (board_id)
		{
		case LPC4370SDR_BOARD_ID_PROTO_AIRSPY:
			return "AIRSPY";

		case LPC4370SDR_BOARD_ID_HYDRASDR_RFONE:
			return "HydraSDR RFOne";

		case LPC4370SDR_BOARD_ID_INVALID:
			return "Invalid Board ID";

		default:
			return "Unknown Board ID";
		}
	}

#ifdef __cplusplus
} // __cplusplus defined.
#endif

int ADDCALL lpc4370sdr_set_sof_divider(struct lpc4370sdr_device* device, uint32_t divider)
{
	device->sof_divider = divider;
	return send_sof_divider(device, divider);
}

int ADDCALL lpc4370sdr_set_uart_baud(struct lpc4370sdr_device* device, uint32_t baud)
{
	int result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_SET_UART_BAUD,
		baud & 0xFFFF,
		baud >> 16,
		NULL,
		0,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result != 0)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_uart_write(struct lpc4370sdr_device* device, const uint8_t* data, uint16_t len)
{
	int result;
	if (len == 0 || len > 64)
		return LPC4370SDR_ERROR_INVALID_PARAM;
	result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_UART_WRITE,
		0,
		0,
		(unsigned char*)data,
		len,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result < len)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_set_calibration(struct lpc4370sdr_device* device, int32_t correction_ppb)
{
	uint32_t v = (uint32_t)correction_ppb;
	int result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_SET_CALIBRATION,
		v & 0xFFFF,
		v >> 16,
		NULL,
		0,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result != 0)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_get_calibration(struct lpc4370sdr_device* device, lpc4370sdr_calibration_t* calibration)
{
	int result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_GET_CALIBRATION,
		0,
		0,
		(unsigned char*)calibration,
		sizeof(lpc4370sdr_calibration_t),
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result < (int)sizeof(lpc4370sdr_calibration_t))
		return LPC4370SDR_ERROR_LIBUSB;
	calibration->correction_ppb = (int32_t)TO_LE((uint32_t)calibration->correction_ppb);
	calibration->source = TO_LE(calibration->source);
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_mem_read(struct lpc4370sdr_device* device, uint32_t address, void* data, uint16_t len)
{
	int result;
	if (len == 0 || len > 64)
		return LPC4370SDR_ERROR_INVALID_PARAM;
	result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_MEM_READ,
		address & 0xFFFF,
		address >> 16,
		(unsigned char*)data,
		len,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result < len)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_mem_write(struct lpc4370sdr_device* device, uint32_t address, const void* data, uint16_t len)
{
	int result;
	if (len == 0 || len > 64)
		return LPC4370SDR_ERROR_INVALID_PARAM;
	result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_MEM_WRITE,
		address & 0xFFFF,
		address >> 16,
		(unsigned char*)data,
		len,
		LIBUSB_CTRL_TIMEOUT_MS);
	if (result < len)
		return LPC4370SDR_ERROR_LIBUSB;
	return LPC4370SDR_SUCCESS;
}

int ADDCALL lpc4370sdr_call(struct lpc4370sdr_device* device, uint32_t core, uint32_t address, const uint32_t args[4], uint32_t* r0, uint32_t timeout_ms)
{
	lpc4370sdr_call_request_t req;
	lpc4370sdr_call_result_t res;
	int result;
	uint32_t waited = 0;
	int i;

	req.core = TO_LE(core);
	req.address = TO_LE(address);
	for (i = 0; i < 4; i++)
		req.r[i] = args ? TO_LE(args[i]) : 0;
	result = libusb_control_transfer(
		device->usb_device,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
		LPC4370SDR_CALL, 0, 0, (unsigned char*)&req, sizeof(req), LIBUSB_CTRL_TIMEOUT_MS);
	if (result < (int)sizeof(req))
		return LPC4370SDR_ERROR_LIBUSB;

	for (;;)
	{
		result = libusb_control_transfer(
			device->usb_device,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
			LPC4370SDR_CALL, 0, 0, (unsigned char*)&res, sizeof(res), LIBUSB_CTRL_TIMEOUT_MS);
		if (result < (int)sizeof(res))
			return LPC4370SDR_ERROR_LIBUSB;
		res.status = TO_LE(res.status);
		if (res.status == 0)
		{
			if (r0 != NULL)
				*r0 = TO_LE(res.r0);
			return LPC4370SDR_SUCCESS;
		}
		if (res.status != 1)
			return LPC4370SDR_ERROR_INVALID_PARAM;
		if (waited >= timeout_ms)
			return LPC4370SDR_ERROR_BUSY;
#ifdef _WIN32
		Sleep(1);
#else
		usleep(1000);
#endif
		waited++;
	}
}
