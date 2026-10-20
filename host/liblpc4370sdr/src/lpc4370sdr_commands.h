/*
Copyright (c) 2013-2016, Benjamin Vernoux <bvernoux@airspy.com>
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

#ifndef __LPC4370SDR_COMMANDS_H__
#define __LPC4370SDR_COMMANDS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* After libairspy's airspy_commands.h (the libairspy compatibility layer includes both): the
   enums both define are taken from there */
#ifndef __AIRSPY_COMMANDS_H__
typedef enum
{
    RECEIVER_MODE_OFF = 0,
    RECEIVER_MODE_RX = 1
} receiver_mode_t;
#endif

/*
  Note: lpc4370sdr_samplerate_t is now obsolete and left for backward compatibility.
  The list of supported sample rates should be retrieved at run time by calling lpc4370sdr_get_samplerates().
  Refer to the lpc4370sdr tools for illustrations.
  The indices are the Airspy R2's (and those of its old firmware, which has no list): on the other
  boards index 1 is another rate (5 MSPS on the HydraSDR RFOne, 3 MSPS on the Airspy Mini).
*/
typedef enum 
{
    LPC4370SDR_SAMPLERATE_10MSPS = 0, /* 12bits 10MHz IQ */
    LPC4370SDR_SAMPLERATE_2_5MSPS = 1, /* 12bits 2.5MHz IQ */
    LPC4370SDR_SAMPLERATE_END = 2 /* End index for sample rate (corresponds to number of samplerate) */
} lpc4370sdr_samplerate_t;


#define LPC4370SDR_CONF_CMD_SHIFT_BIT (3) // Up to 3bits=8 samplerates (lpc4370sdr_samplerate_t enum shall not exceed 7)

// Commands (usb vendor request) shared between Firmware and Host.
#define LPC4370SDR_CMD_MAX (28)
#define LPC4370SDR_EXT_CMD_BASE (0x80)
typedef enum
{
    LPC4370SDR_RESET                      = 0 , /* the device reboots at once, without answering: lpc4370sdr_reset() */
    LPC4370SDR_RECEIVER_MODE              = 1 ,
    LPC4370SDR_SI5351C_WRITE              = 2 ,
    LPC4370SDR_SI5351C_READ               = 3 ,
    LPC4370SDR_R820T_WRITE                = 4 ,
    LPC4370SDR_R820T_READ                 = 5 ,
    LPC4370SDR_SPIFLASH_ERASE             = 6 ,
    LPC4370SDR_SPIFLASH_WRITE             = 7 ,
    LPC4370SDR_SPIFLASH_READ              = 8 ,
    LPC4370SDR_BOARD_ID_READ              = 9 ,
    LPC4370SDR_VERSION_STRING_READ        = 10,
    LPC4370SDR_BOARD_PARTID_SERIALNO_READ = 11,
    LPC4370SDR_SET_SAMPLERATE             = 12,
    LPC4370SDR_SET_FREQ                   = 13,
    LPC4370SDR_SET_LNA_GAIN               = 14,
    LPC4370SDR_SET_MIXER_GAIN             = 15,
    LPC4370SDR_SET_VGA_GAIN               = 16,
    LPC4370SDR_SET_LNA_AGC                = 17,
    LPC4370SDR_SET_MIXER_AGC              = 18,
    LPC4370SDR_MS_VENDOR_CMD              = 19,
    LPC4370SDR_SET_RF_BIAS_CMD            = 20,
    LPC4370SDR_GPIO_WRITE                 = 21,
    LPC4370SDR_GPIO_READ                  = 22,
    LPC4370SDR_GPIODIR_WRITE              = 23,
    LPC4370SDR_GPIODIR_READ               = 24,
    LPC4370SDR_GET_SAMPLERATES            = 25,
    LPC4370SDR_SET_PACKING                = 26,
    LPC4370SDR_SPIFLASH_ERASE_SECTOR      = 27,
    LPC4370SDR_SET_RF_PORT                = LPC4370SDR_CMD_MAX, /* IN, wIndex = lpc4370sdr_rf_port_t; returns 1; HydraSDR RFOne only */
    /* Stream metadata, timing and debug commands of the lpc4370sdr firmware, on every board: kept clear
       of the Airspy and HydraSDR range (29..33 are hydrasdr-host v1.1's, which this firmware stalls) */
    LPC4370SDR_GET_STREAM_STATUS          = LPC4370SDR_EXT_CMD_BASE + 0,
    LPC4370SDR_SET_FRAMING                = LPC4370SDR_EXT_CMD_BASE + 1,
    LPC4370SDR_WATCHDOG                   = LPC4370SDR_EXT_CMD_BASE + 2,
    LPC4370SDR_SET_UART_BAUD              = LPC4370SDR_EXT_CMD_BASE + 3, /* wValue = baud & 0xFFFF, wIndex = baud >> 16 */
    LPC4370SDR_UART_WRITE                 = LPC4370SDR_EXT_CMD_BASE + 4, /* OUT data = bytes to transmit (max 64) */
    LPC4370SDR_SET_CALIBRATION            = LPC4370SDR_EXT_CMD_BASE + 5, /* wValue|wIndex<<16 = crystal correction in ppb (int32), applied at once */
    LPC4370SDR_GET_CALIBRATION            = LPC4370SDR_EXT_CMD_BASE + 6, /* IN: lpc4370sdr_calibration_t */
    /* Debug access, see lpc4370sdr_debug: */
    LPC4370SDR_MEM_READ                   = LPC4370SDR_EXT_CMD_BASE + 7, /* IN: wValue | wIndex << 16 = address, wLength <= 64 bytes */
    LPC4370SDR_MEM_WRITE                  = LPC4370SDR_EXT_CMD_BASE + 8, /* OUT: same addressing, data = bytes to write */
    LPC4370SDR_CALL                       = LPC4370SDR_EXT_CMD_BASE + 9, /* OUT: lpc4370sdr_call_request_t runs a function; IN: lpc4370sdr_call_result_t */
    LPC4370SDR_SET_SOF_DIVIDER            = LPC4370SDR_EXT_CMD_BASE + 10 /* wValue | wIndex << 16 = divider: tag the first SOF of every USB frame whose number is a multiple of it, 0 = off; kept by the restart a setting change performs, cleared when the receiver goes off */
} lpc4370sdr_vendor_request;

#define LPC4370SDR_FRAME_HEADER_SIZE (96)
#define LPC4370SDR_FRAME_MAGIC (0x42445642)
#define LPC4370SDR_FRAME_FLAG_PACKED (1 << 0) /* 12-bit packed samples (packing 1 and 2) */
#define LPC4370SDR_FRAME_FLAG_8BIT   (1 << 1) /* one byte per sample, the top 8 bits (packing 3) */
#define LPC4370SDR_FRAME_FLAG_TIMESTAMP (1 << 2) /* packing 2: chunk counter and ADC status ahead of the samples */
/* The payload ends with LPC4370SDR_FRAME_TAIL_SAMPLES value positions that hold no samples (any
 * packing: 16-bit, 12-bit or 8-bit ones); its last 32-bit word is ~chunk_index, written once the chunk
 * is complete. A chunk cut short on USB, or sent while the device's ADC DMA was overwriting it, fails
 * the check. Older firmware sets it in packing 1 to 3 only. */
#define LPC4370SDR_FRAME_FLAG_TAIL   (1 << 3)
#define LPC4370SDR_FRAME_TAIL_SAMPLES (16)

/* Crystal correction in effect: the flash block's value */
#define LPC4370SDR_CALIBRATION_SOURCE_NONE  (0)
#define LPC4370SDR_CALIBRATION_SOURCE_FLASH (1)
#define LPC4370SDR_CALIBRATION_SOURCE_HOST  (2)
typedef struct
{
  uint32_t core; /* 0 = M0 (runs in the USB request handler), 1 = M4 (runs in its main loop) */
  uint32_t address; /* function address; the Thumb bit is added by the device */
  uint32_t r[4];    /* arguments */
} lpc4370sdr_call_request_t;

typedef struct
{
  uint32_t status; /* 0 = done (r0 valid), 1 = still running on the M4, 2 = bad request */
  uint32_t r0;
} lpc4370sdr_call_result_t;

typedef struct
{
  int32_t correction_ppb;
  uint32_t source; /* LPC4370SDR_CALIBRATION_SOURCE_* */
} lpc4370sdr_calibration_t;



/* Chunk size on the wire, framed or not */
#define LPC4370SDR_FRAME_WIRE_UNPACKED (16384)
#define LPC4370SDR_FRAME_WIRE_PACKED (6144)
#define LPC4370SDR_FRAME_WIRE_8BIT (4096)
#define LPC4370SDR_FRAME_UART_BYTES (15) /* auxiliary UART bytes carried per chunk header */

/*
 * The chunk header, version 2. Fields that only grow are sent modulo 2^16 (a host extends them from
 * one header to the next: they never move 2^15 between two chunks); the chunk's 64-bit sample index
 * is chunk_index (extended past its 32-bit wrap by the host) times sample_count. The PPS edge's and
 * the SOF's 64-bit sample indices are sent as their low words, which a host extends from the
 * chunk's sample index: only while the edge is less than 2^31 samples behind the chunk. The last
 * edge stays in the headers when the input stops, so a host takes an edge only when pps_count
 * (sof_count) changed. ext[] carries items a host need not know to stay in step - see
 * LPC4370SDR_EXT_*. check makes a header hard to mistake for sample data when a host looks for the
 * chunk boundaries again.
 */
#define LPC4370SDR_FRAME_VERSION (2)
#define LPC4370SDR_FRAME_EXT_BYTES (31)

typedef struct
{
	uint32_t magic;          /* LPC4370SDR_FRAME_MAGIC */
	uint32_t chunk_index;    /* chunk number since stream start (the device's counter is 32-bit too: it wraps) */
	uint8_t version;         /* LPC4370SDR_FRAME_VERSION */
	uint8_t flags;           /* LPC4370SDR_FRAME_FLAG_* */
	uint8_t stream_start_count; /* streams started since power-on (mod 256): it changes when a setting change
	                               restarted the stream, and chunk_index starts again */
	uint8_t uart_len;        /* bytes received on the auxiliary UART (GNSS module) carried in this chunk */
	uint16_t sample_count;   /* real samples in this chunk */
	uint16_t lost_chunks;    /* chunks the device lost so far (mod 2^16), see lpc4370sdr_stream_status_t */
	uint16_t overrun_chunks; /* chunks the device delivered corrupted so far (mod 2^16) */
	uint16_t overrun_back;   /* the last chunk counted in overrun_chunks: this many chunks before this one
	                            (0xFFFF: that many or more) - the device sent it while the DMA overwrote it */
	uint32_t freq_hz;        /* tuner frequency the device was set to when this chunk was queued */
	uint32_t pps_sample_index_lo; /* sample index of the last PPS edge captured since the stream started (low word) */
	uint16_t pps_fraction;   /* position of that edge within the sample, in 1/2^16 sample units */
	uint16_t pps_count;      /* PPS edges captured since the stream started (mod 2^16) */
	int16_t pps_uart_rel;    /* the edge among the auxiliary UART's bytes: the bytes received before it end this
	                            many bytes after the last of this chunk's uart_data (negative: before it) */
	uint16_t pps_uart_quiet_us; /* microseconds from the last UART byte received before the edge to the edge, a
	                               lower bound, at most 65535; 0 when a byte arrived at the edge */
	/* USB Start-Of-Frame tagging (LPC4370SDR_SET_SOF_DIVIDER), independent of the PPS input */
	uint32_t sof_sample_index_lo; /* sample index of the last tagged SOF (low word) */
	uint16_t sof_fraction;   /* position of that SOF within the sample, in 1/2^16 sample units */
	uint16_t sof_count;      /* SOFs tagged since the stream started (mod 2^16) */
	uint16_t sof_frame;      /* its USB frame number, extended past the 11-bit wrap by the device (mod 2^16) */
	uint8_t uart_data[LPC4370SDR_FRAME_UART_BYTES];
	/* Items, one after the other: type (LPC4370SDR_EXT_*), length, that many bytes; a type 0 or the
	   end of the area ends them. A host passes on the ones it does not know. */
	uint8_t ext[LPC4370SDR_FRAME_EXT_BYTES];
	uint32_t check;          /* ~(sum of the header's other 23 32-bit words) */
} lpc4370sdr_frame_header_t;   /* LPC4370SDR_FRAME_HEADER_SIZE bytes */

/* the size, checked by the compiler (C and C++) */
typedef char lpc4370sdr_frame_header_size_check[(sizeof(lpc4370sdr_frame_header_t) == LPC4370SDR_FRAME_HEADER_SIZE) ? 1 : -1];

#define LPC4370SDR_EXT_END (0)
/* the device's stream health, every LPC4370SDR_EXT_HEALTH_EVERY chunks (16 bytes, little-endian):
   dma_errors u32, adc_overflows u32, usb_errors u32, backlog_max u16, m0_lag_max u16 - the
   LPC4370SDR_GET_STREAM_STATUS fields, in step with the samples */
#define LPC4370SDR_EXT_HEALTH (1)
#define LPC4370SDR_EXT_HEALTH_EVERY (128)
/* 0x80..0xFE: a test build's (LPC4370SDR_EXT_TEST) pattern, see lpc4370sdr_ext_test_fill() */
#define LPC4370SDR_EXT_TEST_FIRST (0x80)

/*
 * The test pattern for chunk c: every 89th chunk empty; else 1 to 5 items (every 5th chunk: as many as
 * fit), of 0 to 29 bytes, the last one cut to fill the area exactly when it would not fit; type and
 * contents from c and the item's place. Unused bytes 0. Shared by the device and the host's check.
 */
static inline void lpc4370sdr_ext_test_fill(uint8_t* ext, uint32_t c)
{
	uint32_t pos = 0, k = 0, j;
	const uint32_t items = (c % 5u == 4u) ? 255u : 1u + c % 5u;

	for (j = 0; j < LPC4370SDR_FRAME_EXT_BYTES; j++)
		ext[j] = 0;
	if (c % 89u == 0)
		return;
	while (k < items && LPC4370SDR_FRAME_EXT_BYTES - pos >= 2)
	{
		uint32_t len = (c * 7u + k * 13u) % 30u;
		if (len > LPC4370SDR_FRAME_EXT_BYTES - pos - 2)
			len = LPC4370SDR_FRAME_EXT_BYTES - pos - 2;
		ext[pos] = (uint8_t)(LPC4370SDR_EXT_TEST_FIRST + (c + k) % 0x7Fu);
		ext[pos + 1] = (uint8_t)len;
		for (j = 0; j < len; j++)
			ext[pos + 2 + j] = (uint8_t)(c * 3u + k * 101u + j * 31u + (c >> 8));
		pos += 2 + len;
		k++;
	}
}


/* LPC4370SDR_WATCHDOG: wIndex 1 feeds the watchdog and reports */
#define LPC4370SDR_WATCHDOG_FLAG_ARMED (1 << 0)             /* the watchdog is running */
#define LPC4370SDR_WATCHDOG_FLAG_RESET_BY_WATCHDOG (1 << 1) /* the last reset was a watchdog time-out (best effort) */
#define LPC4370SDR_WATCHDOG_FLAG_HOST_OWNED (1 << 2) /* strict mode: the host has made contact, only the host feeds */
#define LPC4370SDR_WATCHDOG_FLAG_STRICT (1 << 3) /* firmware built with WATCHDOG=1: the host protocol applies */
#define LPC4370SDR_WATCHDOG_SELF_FEEDS (6) /* strict mode: self feeds before contact, about 5.6 s each */

typedef struct
{
    uint32_t flags;           /* LPC4370SDR_WATCHDOG_FLAG_* */
    uint32_t timeout_ms;      /* time between feeds the device tolerates */
    uint32_t remaining_ms; /* time left before a reset when the reply was built */
    uint32_t self_feeds_left; /* strict mode: self feeds left before contact; 0xFFFFFFFF when unlimited */
} lpc4370sdr_watchdog_status_t;

/* Antenna inputs of the HydraSDR RFOne (R828D tuner) */
typedef enum
{
    LPC4370SDR_RF_PORT_RX0 = 0, /* ANT, the SMA input */
    LPC4370SDR_RF_PORT_RX1 = 1, /* CABLE1 */
    LPC4370SDR_RF_PORT_RX2 = 2  /* CABLE2 */
} lpc4370sdr_rf_port_t;

#ifdef __AIRSPY_COMMANDS_H__
typedef airspy_common_config_pages_t lpc4370sdr_common_config_pages_t;
typedef airspy_gpio_port_t lpc4370sdr_gpio_port_t;
typedef airspy_gpio_pin_t lpc4370sdr_gpio_pin_t;
#else
typedef enum
{
    CONFIG_CALIBRATION = 0,
    //CONFIG_META        = 1,
} lpc4370sdr_common_config_pages_t;

typedef enum
{
    GPIO_PORT0 = 0,
    GPIO_PORT1 = 1,
    GPIO_PORT2 = 2,
    GPIO_PORT3 = 3,
    GPIO_PORT4 = 4,
    GPIO_PORT5 = 5,
    GPIO_PORT6 = 6,
    GPIO_PORT7 = 7
} lpc4370sdr_gpio_port_t;

typedef enum
{
    GPIO_PIN0 = 0,
    GPIO_PIN1 = 1,
    GPIO_PIN2 = 2,
    GPIO_PIN3 = 3,
    GPIO_PIN4 = 4,
    GPIO_PIN5 = 5,
    GPIO_PIN6 = 6,
    GPIO_PIN7 = 7,
    GPIO_PIN8 = 8,
    GPIO_PIN9 = 9,
    GPIO_PIN10 = 10,
    GPIO_PIN11 = 11,
    GPIO_PIN12 = 12,
    GPIO_PIN13 = 13,
    GPIO_PIN14 = 14,
    GPIO_PIN15 = 15,
    GPIO_PIN16 = 16,
    GPIO_PIN17 = 17,
    GPIO_PIN18 = 18,
    GPIO_PIN19 = 19,
    GPIO_PIN20 = 20,
    GPIO_PIN21 = 21,
    GPIO_PIN22 = 22,
    GPIO_PIN23 = 23,
    GPIO_PIN24 = 24,
    GPIO_PIN25 = 25,
    GPIO_PIN26 = 26,
    GPIO_PIN27 = 27,
    GPIO_PIN28 = 28,
    GPIO_PIN29 = 29,
    GPIO_PIN30 = 30,
    GPIO_PIN31 = 31
} lpc4370sdr_gpio_pin_t;
#endif

#ifdef __cplusplus
} // __cplusplus defined.
#endif

#endif//__LPC4370SDR_COMMANDS_H__
