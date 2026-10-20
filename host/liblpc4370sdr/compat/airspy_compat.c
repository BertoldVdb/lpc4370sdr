/*
Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org>

This file is part of lpc4370sdr.

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

		Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
		Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the
		documentation and/or other materials provided with the distribution.
		Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software
		without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * The libairspy API (airspy.h of libairspy 1.0.12) on top of liblpc4370sdr, so that programs
 * built for libairspy run unchanged: built as libairspy.so.0 / airspy.dll.
 *
 * Every function forwards to its lpc4370sdr_ counterpart; the types are the same apart from
 * their names. The sample callback gets blocks of the size libairspy delivers: with framing the
 * library's blocks vary in size, so they are gathered into fixed-size blocks here. Gap fill is
 * on: short gaps come filled with zeros; what the library leaves out of a longer gap is filled
 * here up to the end of the block being gathered, and the rest counts in the next block's
 * dropped_samples, which is what libairspy's dropped_samples means. A sample's position in the
 * stream, plus the dropped_samples up to it, is then its time.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* The libairspy API is what this library exports; liblpc4370sdr, built in, stays hidden
   (LPC4370SDR_NO_EXPORT, -fvisibility=hidden) */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif
#include "airspy.h"

/* Exported by libairspy though airspy.h does not declare them: its gain tables (here a copy of
   those liblpc4370sdr's airspy_set_linearity_gain() and airspy_set_sensitivity_gain() use) and
   airspy_set_receiver_mode(). The I/Q converters are in airspy_iqconverter.c. */
uint8_t airspy_linearity_vga_gains[22] = { 13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4 };
uint8_t airspy_linearity_mixer_gains[22] = { 12, 12, 11, 9, 8, 7, 6, 6, 5, 0, 0, 1, 0, 0, 2, 2, 1, 1, 1, 1, 0, 0 };
uint8_t airspy_linearity_lna_gains[22] = { 14, 14, 14, 13, 12, 10, 9, 9, 8, 9, 8, 6, 5, 3, 1, 0, 0, 0, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_vga_gains[22] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4 };
uint8_t airspy_sensitivity_mixer_gains[22] = { 12, 12, 12, 12, 11, 10, 10, 9, 9, 8, 7, 4, 4, 4, 3, 2, 2, 1, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_lna_gains[22] = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12, 9, 9, 8, 7, 6, 5, 3, 2, 1, 0 };
int ADDCALL airspy_set_receiver_mode(struct airspy_device* device, receiver_mode_t value);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif
#include "lpc4370sdr.h"

struct airspy_device
{
	struct lpc4370sdr_device* dev;
	airspy_sample_block_cb_fn callback;
	void* ctx;
	enum airspy_sample_type sample_type;
	uint8_t packing;
	/* The block being gathered */
	uint8_t* buf;
	size_t buf_bytes;
	size_t sample_bytes; /* bytes per output sample (an IQ pair for the IQ types) */
	uint32_t block;      /* samples per block delivered */
	uint32_t fill;       /* samples in buf */
	uint64_t dropped;    /* samples missing before the block being gathered */
	uint32_t delay;      /* the I/Q conversion's delay in output samples, rounded up (0 for the real types) */
	volatile int stop;   /* the application asked to stop: from its callback, or airspy_stop_rx() */
	struct airspy_device* next; /* in deferred_close */
};

/* Devices closed from their own sample callback, whose stream had yet to end: closed for good by
   the next airspy_open*() or airspy_exit() */
static pthread_mutex_t deferred_close_mp = PTHREAD_MUTEX_INITIALIZER;
static struct airspy_device* deferred_close;

/* The device whose sample callback this thread is running, if any */
#if defined(_MSC_VER)
static __declspec(thread) struct airspy_device* callback_device;
#else
static __thread struct airspy_device* callback_device;
#endif

#define DEV(device) ((device)->dev)

/* Real samples in a USB buffer without framing: the block size libairspy delivers */
static uint32_t block_real_samples(uint8_t packing)
{
	switch (packing)
	{
	case 0:
		return 262144 / 2;
	case 3:
		return 4096 * 36;
	default:
		return 6144 * 24 / 3 * 2;
	}
}

static int is_iq(enum airspy_sample_type type)
{
	return type == AIRSPY_SAMPLE_FLOAT32_IQ || type == AIRSPY_SAMPLE_INT16_IQ;
}

static size_t sample_bytes(enum airspy_sample_type type)
{
	switch (type)
	{
	case AIRSPY_SAMPLE_FLOAT32_IQ:
		return 2 * sizeof(float);
	case AIRSPY_SAMPLE_FLOAT32_REAL:
		return sizeof(float);
	case AIRSPY_SAMPLE_INT16_IQ:
		return 2 * sizeof(int16_t);
	default:
		return sizeof(int16_t);
	}
}

static void deliver(struct airspy_device* d)
{
	airspy_transfer transfer;

	transfer.device = d;
	transfer.ctx = d->ctx;
	transfer.samples = d->buf;
	transfer.sample_count = (int)d->block;
	transfer.dropped_samples = d->dropped;
	transfer.sample_type = d->sample_type;
	d->fill = 0;
	d->dropped = 0;
	if (d->callback(&transfer) != 0)
		d->stop = 1;
}

/* Samples [from, to) of the library's block into the block being gathered */
static void append(struct airspy_device* d, const uint8_t* samples, uint32_t from, uint32_t to)
{
	while (from < to && !d->stop)
	{
		uint32_t n = d->block - d->fill;
		if (n > to - from)
			n = to - from;
		memcpy(d->buf + d->fill * d->sample_bytes, samples + from * d->sample_bytes, n * d->sample_bytes);
		d->fill += n;
		from += n;
		if (d->fill == d->block)
			deliver(d);
	}
}

/* 'missing' samples: zeros up to the end of the block being gathered, the rest before the next block */
static void gap(struct airspy_device* d, uint64_t missing)
{
	if (d->fill > 0 && !d->stop)
	{
		uint32_t n = d->block - d->fill, i;
		if (n > missing)
			n = (uint32_t)missing;
		if (d->sample_type == AIRSPY_SAMPLE_UINT16_REAL)
		{
			uint16_t* p = (uint16_t*)d->buf + d->fill;
			for (i = 0; i < n; i++)
				p[i] = 2048; /* mid-scale */
		}
		else
		{
			memset(d->buf + d->fill * d->sample_bytes, 0, n * d->sample_bytes);
		}
		d->fill += n;
		missing -= n;
		if (d->fill == d->block)
			deliver(d);
	}
	d->dropped += missing;
}

static int compat_gather(lpc4370sdr_transfer* t)
{
	struct airspy_device* d = (struct airspy_device*)t->ctx;
	const uint8_t* samples = (const uint8_t*)t->samples;
	lpc4370sdr_transfer_metadata_t meta;
	uint32_t pos = 0, i;

	if (d->block == 0)
	{
		/* Raw samples: passed on as they come */
		airspy_transfer transfer;
		transfer.device = d;
		transfer.ctx = d->ctx;
		transfer.samples = t->samples;
		transfer.sample_count = t->sample_count;
		transfer.dropped_samples = t->dropped_samples;
		transfer.sample_type = d->sample_type;
		if (d->callback(&transfer) != 0)
			d->stop = 1;
		return d->stop;
	}

	if (lpc4370sdr_transfer_get_metadata(t, &meta) == LPC4370SDR_SUCCESS)
	{
		const uint32_t shift = is_iq(d->sample_type) ? 1 : 0; /* gap positions are ADC samples */
		uint64_t listed = 0;

		for (i = 0; i < meta.gaps_len; i++)
		{
			const lpc4370sdr_gap_t* g = &meta.gaps[i];
			const uint64_t missing = (g->samples - g->filled) >> shift;
			/* The missing samples lie before the filled ones. The I/Q conversion's output lags
			   its input: the first 'delay' output samples from the gap's place on are from before it */
			uint32_t at = (g->offset >> shift) + d->delay;

			if (missing == 0)
				continue;
			if (at > (uint32_t)t->sample_count)
				at = (uint32_t)t->sample_count;
			append(d, samples, pos, at);
			pos = at;
			gap(d, missing);
			listed += missing;
		}
		/* Gaps past the metadata's list: where they are is not known */
		if (t->dropped_samples > listed)
			d->dropped += t->dropped_samples - listed;
	}
	else if (t->dropped_samples)
	{
		/* No framing: only whole buffers are known to be missing, before this one */
		pos = d->delay < (uint32_t)t->sample_count ? d->delay : (uint32_t)t->sample_count;
		append(d, samples, 0, pos);
		gap(d, t->dropped_samples);
	}

	if (pos == 0 && d->fill == 0 && (uint32_t)t->sample_count == d->block && !d->stop)
	{
		/* A whole block as it is: no copy */
		airspy_transfer transfer;
		transfer.device = d;
		transfer.ctx = d->ctx;
		transfer.samples = t->samples;
		transfer.sample_count = t->sample_count;
		transfer.dropped_samples = d->dropped;
		transfer.sample_type = d->sample_type;
		d->dropped = 0;
		if (d->callback(&transfer) != 0)
			d->stop = 1;
	}
	else
	{
		append(d, samples, pos, (uint32_t)t->sample_count);
	}
	return d->stop;
}

static int compat_callback(lpc4370sdr_transfer* t)
{
	struct airspy_device* d = (struct airspy_device*)t->ctx;
	int result;

	if (d->stop)
		return 1;
	callback_device = d;
	result = compat_gather(t);
	callback_device = NULL;
	return result;
}

static int wrap(struct airspy_device** device, struct lpc4370sdr_device* dev, int result)
{
	struct airspy_device* d;

	*device = NULL;
	if (result != LPC4370SDR_SUCCESS)
		return result;
	d = (struct airspy_device*)calloc(1, sizeof(*d));
	if (d == NULL)
	{
		lpc4370sdr_close(dev);
		return AIRSPY_ERROR_NO_MEM;
	}
	d->dev = dev;
	d->sample_type = AIRSPY_SAMPLE_FLOAT32_IQ;
	*device = d;
	return AIRSPY_SUCCESS;
}

void ADDCALL airspy_lib_version(airspy_lib_version_t* lib_version)
{
	lib_version->major_version = AIRSPY_VER_MAJOR;
	lib_version->minor_version = AIRSPY_VER_MINOR;
	lib_version->revision = AIRSPY_VER_REVISION;
}

static void free_device(struct airspy_device* device)
{
	free(device->buf);
	free(device);
}

static void defer_close(struct airspy_device* device)
{
	pthread_mutex_lock(&deferred_close_mp);
	device->next = deferred_close;
	deferred_close = device;
	pthread_mutex_unlock(&deferred_close_mp);
}

static void close_deferred(void)
{
	struct airspy_device* d;
	struct airspy_device* next;

	pthread_mutex_lock(&deferred_close_mp);
	d = deferred_close;
	deferred_close = NULL;
	pthread_mutex_unlock(&deferred_close_mp);
	for (; d != NULL; d = next)
	{
		next = d->next;
		if (callback_device == d)
		{
			/* From its own callback still */
			defer_close(d);
			continue;
		}
		lpc4370sdr_close(DEV(d));
		free_device(d);
	}
}

int ADDCALL airspy_init(void)
{
	return lpc4370sdr_init();
}

int ADDCALL airspy_exit(void)
{
	close_deferred();
	return lpc4370sdr_exit();
}

int ADDCALL airspy_list_devices(uint64_t* serials, int count)
{
	return lpc4370sdr_list_devices(serials, count);
}

int ADDCALL airspy_open_sn(struct airspy_device** device, uint64_t serial_number)
{
	struct lpc4370sdr_device* dev = NULL;
	int result;

	close_deferred();
	result = lpc4370sdr_open_sn(&dev, serial_number);
	return wrap(device, dev, result);
}

int ADDCALL airspy_open_fd(struct airspy_device** device, int fd)
{
	struct lpc4370sdr_device* dev = NULL;
	int result;

	close_deferred();
	result = lpc4370sdr_open_fd(&dev, fd);
	return wrap(device, dev, result);
}

int ADDCALL airspy_open(struct airspy_device** device)
{
	struct lpc4370sdr_device* dev = NULL;
	int result;

	close_deferred();
	result = lpc4370sdr_open(&dev);
	return wrap(device, dev, result);
}

int ADDCALL airspy_close(struct airspy_device* device)
{
	int result = AIRSPY_SUCCESS;
	if (device != NULL)
	{
		/* From the sample callback the stream stops, and the device is closed once the stream
		   has ended: by the next airspy_open*() or airspy_exit() */
		if (callback_device == device)
		{
			device->stop = 1;
			defer_close(device);
			return AIRSPY_SUCCESS;
		}
		result = lpc4370sdr_close(DEV(device));
		if (result == LPC4370SDR_ERROR_BUSY)
		{
			device->stop = 1;
			return result;
		}
		free_device(device);
	}
	return result;
}

int ADDCALL airspy_get_samplerates(struct airspy_device* device, uint32_t* buffer, const uint32_t len)
{
	return lpc4370sdr_get_samplerates(DEV(device), buffer, len);
}

int ADDCALL airspy_set_samplerate(struct airspy_device* device, uint32_t samplerate)
{
	return lpc4370sdr_set_samplerate(DEV(device), samplerate);
}

int ADDCALL airspy_set_conversion_filter_float32(struct airspy_device* device, const float* kernel, const uint32_t len)
{
	return lpc4370sdr_set_conversion_filter_float32(DEV(device), kernel, len);
}

int ADDCALL airspy_set_conversion_filter_int16(struct airspy_device* device, const int16_t* kernel, const uint32_t len)
{
	return lpc4370sdr_set_conversion_filter_int16(DEV(device), kernel, len);
}

int ADDCALL airspy_start_rx(struct airspy_device* device, airspy_sample_block_cb_fn callback, void* rx_ctx)
{
	struct airspy_device* d = device;

	if (callback_device == d || lpc4370sdr_is_streaming(DEV(d)) == LPC4370SDR_TRUE)
		return AIRSPY_ERROR_BUSY;
	/* What is left of the last stream (stopped from its callback, ended by the callback's return
	   value or by a USB error) goes first: its callback thread may still be in compat_callback() */
	lpc4370sdr_stop_rx(DEV(d));
	d->callback = callback;
	d->ctx = rx_ctx;
	d->fill = 0;
	d->dropped = 0;
	d->stop = 0;
	d->sample_bytes = sample_bytes(d->sample_type);
	if (d->sample_type == AIRSPY_SAMPLE_RAW)
	{
		d->block = 0;
	}
	else
	{
		size_t bytes;
		double delay = 0;
		lpc4370sdr_get_conversion_delay(DEV(d), &delay, NULL);
		d->delay = (uint32_t)(delay + 0.5);
		d->block = block_real_samples(d->packing) >> (is_iq(d->sample_type) ? 1 : 0);
		bytes = (size_t)d->block * d->sample_bytes;
		if (bytes > d->buf_bytes)
		{
			uint8_t* buf = (uint8_t*)realloc(d->buf, bytes);
			if (buf == NULL)
				return AIRSPY_ERROR_NO_MEM;
			d->buf = buf;
			d->buf_bytes = bytes;
		}
	}
	lpc4370sdr_set_gap_fill(DEV(d), 1);
	return lpc4370sdr_start_rx(DEV(d), compat_callback, d);
}

int ADDCALL airspy_stop_rx(struct airspy_device* device)
{
	device->stop = 1; /* no block after this, also when called from the callback */
	return lpc4370sdr_stop_rx(DEV(device));
}

/* libairspy's own start and stop of the device's stream. Here the stream is liblpc4370sdr's:
   RECEIVER_MODE_OFF stops it like airspy_stop_rx(), RECEIVER_MODE_RX only goes with it */
int ADDCALL airspy_set_receiver_mode(struct airspy_device* device, receiver_mode_t value)
{
	switch (value)
	{
	case RECEIVER_MODE_OFF:
		return airspy_stop_rx(device);
	case RECEIVER_MODE_RX:
		return lpc4370sdr_is_streaming(DEV(device)) == LPC4370SDR_TRUE ? AIRSPY_SUCCESS : AIRSPY_ERROR_UNSUPPORTED;
	default:
		return AIRSPY_ERROR_INVALID_PARAM;
	}
}

int ADDCALL airspy_is_streaming(struct airspy_device* device)
{
	return lpc4370sdr_is_streaming(DEV(device));
}

int ADDCALL airspy_si5351c_write(struct airspy_device* device, uint8_t register_number, uint8_t value)
{
	return lpc4370sdr_si5351c_write(DEV(device), register_number, value);
}

int ADDCALL airspy_si5351c_read(struct airspy_device* device, uint8_t register_number, uint8_t* value)
{
	return lpc4370sdr_si5351c_read(DEV(device), register_number, value);
}

/* Declared by libairspy 1.0.12 but not implemented there either */
int ADDCALL airspy_config_write(struct airspy_device* device, const uint8_t page_index, const uint16_t length, unsigned char* data)
{
	(void)device; (void)page_index; (void)length; (void)data;
	return AIRSPY_ERROR_UNSUPPORTED;
}

int ADDCALL airspy_config_read(struct airspy_device* device, const uint8_t page_index, const uint16_t length, unsigned char* data)
{
	(void)device; (void)page_index; (void)length; (void)data;
	return AIRSPY_ERROR_UNSUPPORTED;
}

int ADDCALL airspy_r820t_write(struct airspy_device* device, uint8_t register_number, uint8_t value)
{
	return lpc4370sdr_r820t_write(DEV(device), register_number, value);
}

int ADDCALL airspy_r820t_read(struct airspy_device* device, uint8_t register_number, uint8_t* value)
{
	return lpc4370sdr_r820t_read(DEV(device), register_number, value);
}

int ADDCALL airspy_gpio_write(struct airspy_device* device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t value)
{
	return lpc4370sdr_gpio_write(DEV(device), port, pin, value);
}

int ADDCALL airspy_gpio_read(struct airspy_device* device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t* value)
{
	return lpc4370sdr_gpio_read(DEV(device), port, pin, value);
}

int ADDCALL airspy_gpiodir_write(struct airspy_device* device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t value)
{
	return lpc4370sdr_gpiodir_write(DEV(device), port, pin, value);
}

int ADDCALL airspy_gpiodir_read(struct airspy_device* device, airspy_gpio_port_t port, airspy_gpio_pin_t pin, uint8_t* value)
{
	return lpc4370sdr_gpiodir_read(DEV(device), port, pin, value);
}

int ADDCALL airspy_spiflash_erase(struct airspy_device* device)
{
	return lpc4370sdr_spiflash_erase(DEV(device));
}

int ADDCALL airspy_spiflash_write(struct airspy_device* device, const uint32_t address, const uint16_t length, unsigned char* const data)
{
	return lpc4370sdr_spiflash_write(DEV(device), address, length, data);
}

int ADDCALL airspy_spiflash_read(struct airspy_device* device, const uint32_t address, const uint16_t length, unsigned char* data)
{
	return lpc4370sdr_spiflash_read(DEV(device), address, length, data);
}

int ADDCALL airspy_spiflash_erase_sector(struct airspy_device* device, const uint16_t sector_num)
{
	return lpc4370sdr_spiflash_erase_sector(DEV(device), sector_num);
}

int ADDCALL airspy_board_id_read(struct airspy_device* device, uint8_t* value)
{
	return lpc4370sdr_board_id_read(DEV(device), value);
}

int ADDCALL airspy_version_string_read(struct airspy_device* device, char* version, uint8_t length)
{
	return lpc4370sdr_version_string_read(DEV(device), version, length);
}

int ADDCALL airspy_board_partid_serialno_read(struct airspy_device* device, airspy_read_partid_serialno_t* read_partid_serialno)
{
	return lpc4370sdr_board_partid_serialno_read(DEV(device), (lpc4370sdr_read_partid_serialno_t*)read_partid_serialno);
}

/* libairspy changes the sample type of a stream from its next buffer on. liblpc4370sdr's stream
   and the blocks gathered here are sized for their type, so the stream restarts with the new one:
   the samples of the restart are missing, uncounted. Not from the sample callback, which cannot
   wait for its stream to end: there liblpc4370sdr refuses the change with AIRSPY_ERROR_BUSY. */
int ADDCALL airspy_set_sample_type(struct airspy_device* device, enum airspy_sample_type sample_type)
{
	const int restart = sample_type != device->sample_type && callback_device != device &&
		lpc4370sdr_is_streaming(DEV(device)) == LPC4370SDR_TRUE;
	int result;

	if (restart)
		airspy_stop_rx(device);
	result = lpc4370sdr_set_sample_type(DEV(device), (enum lpc4370sdr_sample_type)sample_type);
	if (result == LPC4370SDR_SUCCESS)
		device->sample_type = sample_type;
	if (restart)
	{
		const int started = airspy_start_rx(device, device->callback, device->ctx);
		if (result == LPC4370SDR_SUCCESS)
			result = started;
	}
	return result;
}

int ADDCALL airspy_set_freq(struct airspy_device* device, const uint32_t freq_hz)
{
	return lpc4370sdr_set_freq(DEV(device), freq_hz);
}

int ADDCALL airspy_set_lna_gain(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_lna_gain(DEV(device), value);
}

int ADDCALL airspy_set_mixer_gain(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_mixer_gain(DEV(device), value);
}

int ADDCALL airspy_set_vga_gain(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_vga_gain(DEV(device), value);
}

int ADDCALL airspy_set_lna_agc(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_lna_agc(DEV(device), value);
}

int ADDCALL airspy_set_mixer_agc(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_mixer_agc(DEV(device), value);
}

int ADDCALL airspy_set_linearity_gain(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_linearity_gain(DEV(device), value);
}

int ADDCALL airspy_set_sensitivity_gain(struct airspy_device* device, uint8_t value)
{
	return lpc4370sdr_set_sensitivity_gain(DEV(device), value);
}

int ADDCALL airspy_set_rf_bias(struct airspy_device* dev, uint8_t value)
{
	return lpc4370sdr_set_rf_bias(DEV(dev), value);
}

/* libairspy: any value but 0 packs (liblpc4370sdr's 2 and 3 are other formats) */
int ADDCALL airspy_set_packing(struct airspy_device* device, uint8_t value)
{
	int result;

	value = value ? 1 : 0;
	result = lpc4370sdr_set_packing(DEV(device), value);
	if (result == LPC4370SDR_SUCCESS)
		device->packing = value;
	return result;
}

const char* ADDCALL airspy_error_name(enum airspy_error errcode)
{
	switch (errcode)
	{
	case AIRSPY_SUCCESS:
		return "AIRSPY_SUCCESS";
	case AIRSPY_TRUE:
		return "AIRSPY_TRUE";
	case AIRSPY_ERROR_INVALID_PARAM:
		return "AIRSPY_ERROR_INVALID_PARAM";
	case AIRSPY_ERROR_NOT_FOUND:
		return "AIRSPY_ERROR_NOT_FOUND";
	case AIRSPY_ERROR_BUSY:
		return "AIRSPY_ERROR_BUSY";
	case AIRSPY_ERROR_NO_MEM:
		return "AIRSPY_ERROR_NO_MEM";
	case AIRSPY_ERROR_UNSUPPORTED:
		return "AIRSPY_ERROR_UNSUPPORTED";
	case AIRSPY_ERROR_LIBUSB:
		return "AIRSPY_ERROR_LIBUSB";
	case AIRSPY_ERROR_THREAD:
		return "AIRSPY_ERROR_THREAD";
	case AIRSPY_ERROR_STREAMING_THREAD_ERR:
		return "AIRSPY_ERROR_STREAMING_THREAD_ERR";
	case AIRSPY_ERROR_STREAMING_STOPPED:
		return "AIRSPY_ERROR_STREAMING_STOPPED";
	case AIRSPY_ERROR_OTHER:
		return "AIRSPY_ERROR_OTHER";
	default:
		return "airspy unknown error";
	}
}

const char* ADDCALL airspy_board_id_name(enum airspy_board_id board_id)
{
	return lpc4370sdr_board_id_name((enum lpc4370sdr_board_id)board_id);
}
