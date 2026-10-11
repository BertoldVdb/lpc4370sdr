/*
Copyright (c) 2012, Jared Boone <jared@sharebrained.com>
Copyright (c) 2013, Michael Ossmann <mike@ossmann.com>
Copyright (c) 2013-2016, Benjamin Vernoux <bvernoux@airspy.com>
Copyright (C) 2013-2016, Youssef Touil <youssef@airspy.com>
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

#ifndef __LPC4370SDR_H__
#define __LPC4370SDR_H__

#include <stddef.h>
#include <stdint.h>
#include "lpc4370sdr_commands.h"

/* 0.2.0: lpc4370sdr_transfer_metadata_t changed (uart_data holds LPC4370SDR_METADATA_UART_MAX = 1024
   bytes, stream_restarts added), lpc4370sdr_set_receiver_mode() is no longer exported and
   lpc4370sdr_config_read/write() are gone: rebuild programs built against 0.1.0
   0.2.1: lpc4370sdr_set_dc_removal() added; slow zero-copy buffers are replaced by ordinary
   ones instead of being copied out (binary compatible with 0.2.0)
   0.3.0: chunk header version 2 (LPC4370SDR_FRAME_VERSION): firmware that sends it, and only that.
   lpc4370sdr_transfer_metadata_t grew uart_offset, pps_uart_count and pps_uart_quiet_us (the UART
   bytes and the PPS edge in order, and how long the line was quiet before it) and ext[] (the
   header's extension items, lpc4370sdr_ext_next()): rebuild programs built against 0.2
   0.3.1: lpc4370sdr_usb_position() added (binary compatible with 0.3.0)
   0.3.2: lpc4370sdr_get_conversion_delay() added (binary compatible with 0.3.0)
   0.3.3: the I/Q converters, filter kernels and gain tables are no longer exported
   (binary compatible with 0.3.0) */
#define LPC4370SDR_VERSION "0.3.3"
#define LPC4370SDR_VER_MAJOR 0
#define LPC4370SDR_VER_MINOR 3
#define LPC4370SDR_VER_REVISION 3

#ifdef _WIN32
	 #define ADD_EXPORTS
	 
	/* You should define ADD_EXPORTS *only* when building the DLL. */
	#ifdef ADD_EXPORTS
		#define LPC4370SDR_API __declspec(dllexport)
	#else
		#define LPC4370SDR_API __declspec(dllimport)
	#endif

	/* Define calling convention in one place, for convenience. */
	#define ADDCALL __cdecl

#else /* _WIN32 not defined. */

	/* The library is built with hidden symbols: only its API is exported (not from a library
	   that has it built in and defines LPC4370SDR_NO_EXPORT, like the libairspy compatibility one) */
	#if (defined(__GNUC__) || defined(__clang__)) && !defined(LPC4370SDR_NO_EXPORT)
		#define LPC4370SDR_API __attribute__((visibility("default")))
	#else
		#define LPC4370SDR_API
	#endif
	#define ADDCALL

#endif

#ifdef __cplusplus
extern "C"
{
#endif

enum lpc4370sdr_error
{
	LPC4370SDR_SUCCESS = 0,
	LPC4370SDR_TRUE = 1,
	LPC4370SDR_ERROR_INVALID_PARAM = -2,
	LPC4370SDR_ERROR_NOT_FOUND = -5,
	LPC4370SDR_ERROR_BUSY = -6,
	LPC4370SDR_ERROR_NO_MEM = -11,
	LPC4370SDR_ERROR_UNSUPPORTED = -12,
	LPC4370SDR_ERROR_LIBUSB = -1000,
	LPC4370SDR_ERROR_THREAD = -1001,
	LPC4370SDR_ERROR_STREAMING_THREAD_ERR = -1002,
	LPC4370SDR_ERROR_STREAMING_STOPPED = -1003,
	LPC4370SDR_ERROR_OTHER = -9999,
};

enum lpc4370sdr_board_id
{
	LPC4370SDR_BOARD_ID_PROTO_AIRSPY  = 0,
	LPC4370SDR_BOARD_ID_HYDRASDR_RFONE = 1,
	LPC4370SDR_BOARD_ID_INVALID = 0xFF,
};

enum lpc4370sdr_sample_type
{
	LPC4370SDR_SAMPLE_FLOAT32_IQ = 0,   /* 2 * 32bit float per sample */
	LPC4370SDR_SAMPLE_FLOAT32_REAL = 1, /* 1 * 32bit float per sample */
	LPC4370SDR_SAMPLE_INT16_IQ = 2,     /* 2 * 16bit int per sample */
	LPC4370SDR_SAMPLE_INT16_REAL = 3,   /* 1 * 16bit int per sample */
	LPC4370SDR_SAMPLE_UINT16_REAL = 4,  /* 1 * 16bit unsigned int per sample */
	LPC4370SDR_SAMPLE_RAW = 5,          /* Raw packed samples from the device */
	LPC4370SDR_SAMPLE_END = 6           /* Number of supported sample types */
};

#define MAX_CONFIG_PAGE_SIZE (0x10000)

struct lpc4370sdr_device;

typedef struct {
	struct lpc4370sdr_device* device;
	void* ctx;
	void* samples;
	int sample_count;
	/* Samples missing before or in this block (IQ output: IQ samples). Framed: those of the
	   metadata's gaps that gap fill did not replace. Otherwise: an estimate from the USB buffers
	   the library dropped before this block. */
	uint64_t dropped_samples;
	enum lpc4370sdr_sample_type sample_type;
} lpc4370sdr_transfer_t, lpc4370sdr_transfer;

typedef struct {
	uint32_t part_id[2];
	uint32_t serial_no[4];
} lpc4370sdr_read_partid_serialno_t;

typedef struct {
	uint32_t major_version;
	uint32_t minor_version;
	uint32_t revision;
} lpc4370sdr_lib_version_t;

/* Device stream counters, in chunks */
typedef struct {
	uint32_t captured;      /* chunks captured by the ADC DMA */
	uint32_t delivered; /* chunks whose USB transfer to the host completed */
	uint32_t lost; /* chunks overwritten before they could be sent: the sample stream has gaps */
	uint32_t overruns;
	uint32_t backlog_max; /* worst number of chunks pending at the device */
	uint32_t ring_chunks;   /* chunks the device ring buffer holds */
	uint32_t chunk_bytes;   /* bytes per chunk on USB */
	uint32_t chunk_samples; /* real ADC samples per chunk (half as many IQ samples) */
	uint32_t m0_lag_max; /* worst lag of the device's own USB queuing behind the ADC, in chunks */
	uint32_t dma_errors; /* ADC DMA bus errors: the DMA could not write part of the ring */
	uint32_t usb_errors; /* bulk transfers the USB controller retired with an error */
  uint32_t adc_overflows; /* chunks during which the ADC FIFO overflowed */
  uint32_t pps_count; /* PPS edges captured since the stream started */
  uint32_t sof_count; /* USB SOFs tagged since the stream started, see lpc4370sdr_set_sof_divider() */
  uint32_t sof_edges; /* USB SOFs captured since the stream started, tagged or not */
  uint32_t sof_divider; /* SOF tagging divider in effect, 0 = off */
} lpc4370sdr_stream_status_t;

/* Metadata of the block delivered to the sample callback */
#define LPC4370SDR_METADATA_UART_MAX (1024) /* more than the 15 bytes per chunk times the chunks per block (37 at most, 8-bit) */
#define LPC4370SDR_METADATA_SOF_MAX (40) /* more than the chunks per block: each chunk carries one SOF tag */
/* A block that follows buffers the library could not take samples from (every chunk corrupt, say)
   also carries their UART bytes and SOF tags, as far as they fit */

/* A USB Start-Of-Frame tag: the device captured the SOF opening USB frame 'frame' at this sample */
typedef struct {
	uint64_t sample_index;
	uint32_t fraction; /* position of the SOF within that sample, in 1/2^32 sample units */
	uint32_t frame; /* USB frame number, extended past the controller's 11-bit wrap by the device */
	uint32_t count; /* SOFs tagged since the stream started, this one included */
} lpc4370sdr_sof_tag_t;

#define LPC4370SDR_METADATA_GAPS_MAX (40) /* more than the chunks per block: a gap can precede each chunk */

/*
 * Samples missing at one place in a block. Positions and counts are ADC samples, like sample_index;
 * IQ output has half as many IQ samples. With gap fill (lpc4370sdr_set_gap_fill()) the last
 * 'filled' of the missing samples are in the block, as mid-scale (zero) samples from 'offset' on,
 * right before the samples that follow the gap; the others are not.
 */
typedef struct {
	uint32_t offset;  /* samples of the block before the gap, filled ones of earlier gaps included */
	uint32_t filled;  /* missing samples that gap fill put in the block, from 'offset' on */
	uint64_t samples; /* samples missing here */
} lpc4370sdr_gap_t;

/* One chunk header's extension area: the chunk's first sample and its index (both counted from the
   stream start, 64-bit), and the area's bytes */
#define LPC4370SDR_METADATA_EXT_MAX (64)
typedef struct {
	uint64_t sample_index;
	uint64_t chunk_index;
	uint8_t data[LPC4370SDR_FRAME_EXT_BYTES];
} lpc4370sdr_ext_area_t;

/*
 * The next item of an extension area, from *pos (start at 0): 1 with its type, length and bytes,
 * and *pos moved past it; 0 at the end (a type 0, or the area full); -1 when the item runs past
 * the end of the area.
 */
static inline int lpc4370sdr_ext_next(const uint8_t* area, uint32_t* pos, uint8_t* type, const uint8_t** data, uint8_t* len)
{
	if (*pos + 2 > LPC4370SDR_FRAME_EXT_BYTES || area[*pos] == LPC4370SDR_EXT_END)
		return 0;
	if (*pos + 2u + area[*pos + 1] > LPC4370SDR_FRAME_EXT_BYTES)
		return -1;
	*type = area[*pos];
	*len = area[*pos + 1];
	*data = area + *pos + 2;
	*pos += 2u + *len;
	return 1;
}

/*
 * Timeline: the block's sample at position p (ADC samples, filled ones included) has the device
 * sample index sample_index + p + the sum of (samples - filled) over the gaps with
 * 0 < offset <= p. A gap at offset 0 lies before the block's first sample: the previous block
 * ended (samples - filled) samples before sample_index.
 */
typedef struct {
	uint64_t sample_index; /* device sample index of the first sample of this block (a filled one when the block starts with a gap) */
	uint64_t gap_samples; /* samples missing between the previous block's last sample and this block's last, see gaps */
	uint32_t chunks;         /* device chunks in this block */
	uint32_t lost_chunks; /* chunks the device lost since the stream started */
	uint32_t overrun_chunks; /* chunks the device delivered corrupted since the stream started */
	uint32_t sync_errors; /* chunks with an invalid header since the stream started */
	uint32_t duplicate_chunks; /* chunks in this block whose samples had already been delivered */
	uint32_t freq_hz;
	/* External PPS input, from the block's last chunk */
	uint64_t pps_sample_index;
	uint32_t pps_fraction; /* position of the edge within that sample, in 1/2^32 sample units */
	uint32_t pps_count; /* PPS edges captured since the stream started */
	/* Bytes received on the device's auxiliary UART */
	uint32_t uart_len;
	uint8_t uart_data[LPC4370SDR_METADATA_UART_MAX];
	uint32_t flags;          /* LPC4370SDR_FRAME_FLAG_* of the first chunk */
	/* USB Start-Of-Frame tags, see lpc4370sdr_set_sof_divider(): the block's last one, and every distinct one its chunks carried */
	uint64_t sof_sample_index;
	uint32_t sof_fraction;
	uint32_t sof_frame;
	uint32_t sof_count;
	uint32_t sof_tags_len;
	lpc4370sdr_sof_tag_t sof_tags[LPC4370SDR_METADATA_SOF_MAX];
	uint32_t adc_status; /* packing 2: the ADC status bits of the block's chunks, ORed */
	/* Device sample index of the first sample of the last chunk counted in overrun_chunks (valid when
	   overrun_chunks is not 0). The device learns of an overrun after it queued the chunk, so the chunk
	   arrives in an earlier block than the one reporting it; with the chunk tail marker the library
	   drops such chunks itself, see corrupt_chunks. */
	uint64_t overrun_sample_index;
	uint32_t corrupt_chunks; /* chunks dropped because their tail marker showed the device overwrote them, since the stream started; they count in gap_samples */
	uint32_t unaccounted_chunks; /* chunks missing from the device's chunk sequence that it did not count as lost: lost between the device and the library, since the stream started */
	uint32_t resyncs; /* times the library found the chunk boundaries again after data went missing on USB, since the stream started */
	/* Where samples are missing, in stream order: gaps_len entries. Gaps past LPC4370SDR_METADATA_GAPS_MAX
	   count in gap_samples only (with gap fill they are still filled). */
	uint32_t gaps_len;
	lpc4370sdr_gap_t gaps[LPC4370SDR_METADATA_GAPS_MAX];
	uint64_t filled_samples; /* samples gap fill put in this block */
	/* Times the device restarted the stream since lpc4370sdr_start_rx(): a lpc4370sdr_set_samplerate()
	   while streaming does. The sample index starts again near 0; samples of the old stream that would
	   have shared a block with the new one are dropped. */
	uint32_t stream_restarts;
	/*
	 * The UART bytes and the PPS edge in order. The bytes are numbered from lpc4370sdr_start_rx() on:
	 * uart_data[i] is byte uart_offset + i. The device counts the bytes it receives and, at the edge,
	 * how many had arrived: for the edge pps_count reports, bytes [0, pps_uart_count) came before it
	 * and the rest after. pps_uart_quiet_us: microseconds from the last byte before the edge to the
	 * edge, a lower bound (the device stamps a byte once it has taken it from the UART), at most
	 * 65535; 0 when a byte arrived at the edge - the line was busy, and the bytes right before
	 * pps_uart_count may have come just after it. Both stay with the edge until the next one; 0
	 * before the first.
	 */
	uint64_t uart_offset;
	uint64_t pps_uart_count;
	uint32_t pps_uart_quiet_us;
	/* The chunk headers' extension areas that carried items, in stream order, each with its chunk's
	   samples: a chunk whose samples are dropped (corrupt_chunks) takes its area with it. Up to
	   LPC4370SDR_METADATA_EXT_MAX per block (more than a block's chunks), the others counted in
	   ext_dropped. Read the items with lpc4370sdr_ext_next(); ext_malformed counts areas whose
	   items ran past the end, since lpc4370sdr_start_rx(). */
	uint32_t ext_len;
	lpc4370sdr_ext_area_t ext[LPC4370SDR_METADATA_EXT_MAX];
	uint32_t ext_dropped;
	uint32_t ext_malformed;
} lpc4370sdr_transfer_metadata_t;


typedef int (*lpc4370sdr_sample_block_cb_fn)(lpc4370sdr_transfer* transfer);

extern LPC4370SDR_API void ADDCALL lpc4370sdr_lib_version(lpc4370sdr_lib_version_t* lib_version);
/* lpc4370sdr_init() deprecated */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_init(void);
/* lpc4370sdr_exit() deprecated */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_exit(void);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_list_devices(uint64_t *serials, int count);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_open_sn(struct lpc4370sdr_device** device, uint64_t serial_number);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_open_fd(struct lpc4370sdr_device** device, int fd);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_open(struct lpc4370sdr_device** device);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_close(struct lpc4370sdr_device* device);

/* Use lpc4370sdr_get_samplerates(device, buffer, 0) to get the number of available sample rates. It will be returned in the first element of buffer */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_get_samplerates(struct lpc4370sdr_device* device, uint32_t* buffer, const uint32_t len);

/* Parameter samplerate can be either the index of a samplerate or directly its value in Hz within the list returned by lpc4370sdr_get_samplerates() */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_samplerate(struct lpc4370sdr_device* device, uint32_t samplerate);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_conversion_filter_float32(struct lpc4370sdr_device* device, const float *kernel, const uint32_t len);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_conversion_filter_int16(struct lpc4370sdr_device* device, const int16_t *kernel, const uint32_t len);
/* The I/Q conversion removes the ADC's DC before the fs/4 translation (the default, 1). That DC
   ends up at the edge of the I/Q band, -fs/4 of the I/Q rate; a consumer that only uses the
   middle of the band can turn the removal off (0) and save its cost - a serial filter that is
   most of the conversion's time on small cores. Kept across lpc4370sdr_set_conversion_filter_*(). */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_dc_removal(struct lpc4370sdr_device* device, uint8_t enable);
/* Where a capture counted in ADC samples - a chunk's PPS and SOF sample indices - lands in the
   stream the callback gets: at output sample adc_sample / decimation + delay. The I/Q sample
   types halve the rate (decimation 2) through the conversion filter in force for their type,
   whose output pair m is centred on ADC sample 2m - (len - 1) / 2, so delay = (len - 1) / 4 I/Q
   samples: 11.5 with the default 47-tap kernel. The real types and RAW deliver the ADC samples
   as they are: decimation 1, delay 0. Either pointer may be NULL. */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_get_conversion_delay(struct lpc4370sdr_device* device, double* delay, uint32_t* decimation);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_start_rx(struct lpc4370sdr_device* device, lpc4370sdr_sample_block_cb_fn callback, void* rx_ctx);
/* May be called from the sample callback; lpc4370sdr_close() may not (LPC4370SDR_ERROR_BUSY) */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_stop_rx(struct lpc4370sdr_device* device);

/* return LPC4370SDR_TRUE if success */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_is_streaming(struct lpc4370sdr_device* device);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_si5351c_write(struct lpc4370sdr_device* device, uint8_t register_number, uint8_t value);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_si5351c_read(struct lpc4370sdr_device* device, uint8_t register_number, uint8_t* value);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_r820t_write(struct lpc4370sdr_device* device, uint8_t register_number, uint8_t value);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_r820t_read(struct lpc4370sdr_device* device, uint8_t register_number, uint8_t* value);

/* Parameter value shall be 0=clear GPIO or 1=set GPIO */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_gpio_write(struct lpc4370sdr_device* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t value);
/* Parameter value corresponds to GPIO state 0 or 1 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_gpio_read(struct lpc4370sdr_device* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t* value);

/* Parameter value shall be 0=GPIO Input direction or 1=GPIO Output direction */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_gpiodir_write(struct lpc4370sdr_device* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t value);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_gpiodir_read(struct lpc4370sdr_device* device, lpc4370sdr_gpio_port_t port, lpc4370sdr_gpio_pin_t pin, uint8_t* value);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_spiflash_erase(struct lpc4370sdr_device* device);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_spiflash_write(struct lpc4370sdr_device* device, const uint32_t address, const uint16_t length, unsigned char* const data);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_spiflash_read(struct lpc4370sdr_device* device, const uint32_t address, const uint16_t length, unsigned char* data);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_board_id_read(struct lpc4370sdr_device* device, uint8_t* value);
/* Parameter length shall be at least 128bytes to avoid possible string clipping */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_version_string_read(struct lpc4370sdr_device* device, char* version, uint8_t length);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_board_partid_serialno_read(struct lpc4370sdr_device* device, lpc4370sdr_read_partid_serialno_t* read_partid_serialno);

/*
 * Where the device sits on the USB: its bus number and device address (as lsusb shows them, both
 * may be NULL) and, in port (port_len bytes, may be NULL), the sysfs port path as Linux names the
 * device: "<bus>-<port>[.<port>...]", e.g. "1-1.2". Together they find the device's host
 * controller (sysfs, or a service that lists controllers with their devices). The address changes
 * when the device re-enumerates; the port path stays as long as it is plugged into the same port.
 * LPC4370SDR_ERROR_INVALID_PARAM when port_len is too small (32 bytes holds any path:
 * "255-255.255.255.255.255.255.255" and its NUL).
 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_usb_position(struct lpc4370sdr_device* device, uint8_t* busnum, uint8_t* devnum, char* port, size_t port_len);

/* Not while streaming (LPC4370SDR_ERROR_BUSY until lpc4370sdr_stop_rx()) */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_sample_type(struct lpc4370sdr_device* device, enum lpc4370sdr_sample_type sample_type);

/* Parameter freq_hz shall be between 24000000(24MHz) and 1750000000(1.75GHz) */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_freq(struct lpc4370sdr_device* device, const uint32_t freq_hz);

/* Parameter value shall be between 0 and 14 (larger values are taken as 14) */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_lna_gain(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value shall be between 0 and 15 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_mixer_gain(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value shall be between 0 and 15 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_vga_gain(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value:
	0=Disable LNA Automatic Gain Control
	1=Enable LNA Automatic Gain Control
*/
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_lna_agc(struct lpc4370sdr_device* device, uint8_t value);
/* Parameter value:
	0=Disable MIXER Automatic Gain Control
	1=Enable MIXER Automatic Gain Control
*/
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_mixer_agc(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value: 0..21 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_linearity_gain(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value: 0..21 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_sensitivity_gain(struct lpc4370sdr_device* device, uint8_t value);

/* Parameter value shall be 0=Disable BiasT or 1=Enable BiasT */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_rf_bias(struct lpc4370sdr_device* dev, uint8_t value);

/* Antenna input of the HydraSDR RFOne (lpc4370sdr or HydraSDR firmware): LPC4370SDR_ERROR_UNSUPPORTED
   on the Airspy boards, whose R820T2 has a single input */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_rf_port(struct lpc4370sdr_device* device, lpc4370sdr_rf_port_t rf_port);

/* 0 = 16-bit samples, 1 = 12-bit packed, 2 = 12-bit packed with a chunk counter and the ADC status
   (HydraSDR RFOne), 3 = 8-bit. Not while streaming. */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_packing(struct lpc4370sdr_device* device, uint8_t value);

/* Read the device stream counters */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_get_stream_status(struct lpc4370sdr_device* device, lpc4370sdr_stream_status_t* status);

/* USB Start-Of-Frame timestamps: the device tags the SOF opening every USB frame (1 ms; at high speed the
   microframe 0 SOF, the one a full-speed bus sees) whose frame number is a multiple of the divider, with its
   sample index; 0 turns it off. The tags reach the sample callback through lpc4370sdr_transfer_get_metadata().
   Can be changed while streaming; the library sends it again at every lpc4370sdr_start_rx(). Each chunk header
   carries one tag, so keep the tag rate below the chunk rate (sample rate / chunk_samples) to receive every
   tag; a skipped tag shows as a jump in the tag count. Works alongside the PPS input. */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_sof_divider(struct lpc4370sdr_device* device, uint32_t divider);

/* Auxiliary UART on the device (GNSS module): 8N1 */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_uart_baud(struct lpc4370sdr_device* device, uint32_t baud);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_uart_write(struct lpc4370sdr_device* device, const uint8_t* data, uint16_t len);

/* Crystal correction in ppb, applied at once to the tuner */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_calibration(struct lpc4370sdr_device* device, int32_t correction_ppb);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_get_calibration(struct lpc4370sdr_device* device, lpc4370sdr_calibration_t* calibration);

/* Debug access to the device: read or write up to 64 bytes of memory */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_mem_read(struct lpc4370sdr_device* device, uint32_t address, void* data, uint16_t len);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_mem_write(struct lpc4370sdr_device* device, uint32_t address, const void* data, uint16_t len);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_call(struct lpc4370sdr_device* device, uint32_t core, uint32_t address, const uint32_t args[4], uint32_t* r0, uint32_t timeout_ms);

/* IQ output (FLOAT32_IQ, INT16_IQ) is converted on two threads: the second one also calls the
   sample callback. On by default (off when the environment variable LPC4370SDR_NO_DSP_THREAD is
   set); 0 does everything on one thread. Not while streaming. */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_dsp_thread(struct lpc4370sdr_device* device, uint8_t enabled);

/* Framed chunks: the library asks the device, at every lpc4370sdr_start_rx() */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_framing(struct lpc4370sdr_device* device, uint8_t value);

/* Gap fill (framed streams, not LPC4370SDR_SAMPLE_RAW): samples missing from the stream are
   replaced by mid-scale (zero) samples, so a sample's position in the stream is its time. Up to
   one USB buffer's worth of samples per block; the rest of a longer gap stays missing and counts in
   dropped_samples. The metadata's gaps list says where the filled samples are. Off by default;
   can be changed while streaming. */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_set_gap_fill(struct lpc4370sdr_device* device, uint8_t enabled);

extern LPC4370SDR_API int ADDCALL lpc4370sdr_watchdog_status(struct lpc4370sdr_device* device, lpc4370sdr_watchdog_status_t* status);
extern LPC4370SDR_API int ADDCALL lpc4370sdr_watchdog_feed(struct lpc4370sdr_device* device, lpc4370sdr_watchdog_status_t* status);

/* Fill in the metadata of the block a sample callback is being called with */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_transfer_get_metadata(lpc4370sdr_transfer* transfer, lpc4370sdr_transfer_metadata_t* metadata);

extern LPC4370SDR_API const char* ADDCALL lpc4370sdr_error_name(enum lpc4370sdr_error errcode);
extern LPC4370SDR_API const char* ADDCALL lpc4370sdr_board_id_name(enum lpc4370sdr_board_id board_id);

/* Parameter sector_num shall be between 2 & 13 (sector 0 & 1 are reserved) */
extern LPC4370SDR_API int ADDCALL lpc4370sdr_spiflash_erase_sector(struct lpc4370sdr_device* device, const uint16_t sector_num);

#ifdef __cplusplus
} // __cplusplus defined.
#endif

#endif//__LPC4370SDR_H__
