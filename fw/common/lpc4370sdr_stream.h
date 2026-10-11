/*
 * Copyright 2026 Bertold Van den Bergh <vandenbergh@bertold.org>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from HydraSDR).
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

#ifndef __LPC4370SDR_STREAM_H
#define __LPC4370SDR_STREAM_H

#include <stdint.h>
#include "lpc4370sdr_commands.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ADC -> USB ring buffer, shared by the M4 */
#define LPC4370SDR_STREAM_PIECE_SIZE  (16384)
#define LPC4370SDR_STREAM_NUM_PIECES  (11)
#define LPC4370SDR_STREAM_SLOT_SIZE   (8192) /* one GPDMA linked-list item fills one slot */
#define LPC4370SDR_STREAM_NUM_SLOTS   (LPC4370SDR_STREAM_NUM_PIECES * 2)

static const uint32_t lpc4370sdr_stream_pieces[LPC4370SDR_STREAM_NUM_PIECES] =
{
  0x10008000, 0x1000C000, 0x10010000, 0x10014000, 0x10018000, 0x1001C000, /* local SRAM 1 */
  0x10080000, 0x10084000, 0x10088000, 0x1008C000,                         /* local SRAM 2 */
  0x2000C000                                                              /* AHB SRAM */
};

static inline uint8_t* lpc4370sdr_stream_slot(uint32_t slot)
{
  return (uint8_t*)(lpc4370sdr_stream_pieces[slot / 2] + (slot & 1) * LPC4370SDR_STREAM_SLOT_SIZE);
}

/* Unpacked: one chunk is two slots of 16-bit samples */
#define LPC4370SDR_STREAM_CHUNK_BYTES_UNPACKED  (2 * LPC4370SDR_STREAM_SLOT_SIZE)
/* Packed: one chunk is one slot */
#define LPC4370SDR_STREAM_CHUNK_BYTES_PACKED    ((LPC4370SDR_STREAM_SLOT_SIZE / 4) * 3)
/* 8-bit mode (packing 3) */
#define LPC4370SDR_STREAM_CHUNK_BYTES_8BIT      (LPC4370SDR_STREAM_SLOT_SIZE / 2)

/* Packing 2: the chunk counter and the ADC status take the place of the first two packed samples */
#define LPC4370SDR_STREAM_TIMESTAMP_SAMPLES     (2)

/* Transfers the M0 can hold in the USB controller at once */
#define LPC4370SDR_STREAM_USB_POOL              (4)
/* The M0 queues a chunk only while the DMA is at least this many chunks away from its slot:
 * room for the transfers ahead of it to drain. Older chunks are skipped and counted as lost,
 * since they would be overwritten while the USB controller sends them. */
#define LPC4370SDR_STREAM_QUEUE_GUARD           (LPC4370SDR_STREAM_USB_POOL + 1)

/* Stream state, placed at cm4_data_share (AHB SRAM visible to both cores) */
typedef struct
{
  volatile uint32_t captured; /* chunks ready for USB: DMA done and, when packing, packed */
  volatile uint32_t chunk_bytes;  /* bytes per chunk as sent over USB */
  volatile uint32_t chunk_slots; /* DMA slots per chunk: 2 unpacked, 1 packed */
  volatile uint32_t ring_chunks;  /* number of chunks the ring holds */
  volatile uint32_t header_bytes; /* LPC4370SDR_FRAME_HEADER_SIZE when framing is on, else 0 */
  volatile uint32_t chunk_samples;/* real ADC samples per chunk */
  volatile uint32_t overruns; /* chunks the DMA started rewriting while their USB transfer was still pending (M4 DMA ISR) */
  volatile uint32_t backlog_max; /* highest number of chunks pending at the device */
  volatile uint32_t m0_lag_max;
  volatile uint32_t dma_errors; /* GPDMA error interrupts: the ADC DMA could not write the ring (bus error) */
  volatile uint32_t adc_overflows; /* chunks in which the ADC FIFO overflow flag was set */
  /* PPS capture (M4 timer ISR) */
  volatile uint32_t pps_seq;
  volatile uint32_t pps_count;     /* edges captured since the stream started */
  volatile uint32_t pps_index_lo;  /* sample index of the last edge */
  volatile uint32_t pps_index_hi;
  volatile uint32_t pps_fraction; /* position within that sample, 1/2^32 sample units */
  volatile uint32_t pps_uart_in;  /* uart_in at the edge: the auxiliary UART's bytes received before it */
  volatile uint32_t pps_uart_quiet_us; /* microseconds from the last of those bytes to the edge (at most 65535),
                                          0 when one came after it */
  /* USB Start-Of-Frame capture (M4 timer ISR) */
  volatile uint32_t sof_seq;
  volatile uint32_t sof_count;     /* tagged SOFs since the stream started */
  volatile uint32_t sof_edges;     /* every SOF captured since the stream started */
  volatile uint32_t sof_index_lo;  /* sample index of the last tagged SOF */
  volatile uint32_t sof_index_hi;
  volatile uint32_t sof_fraction;
  volatile uint32_t sof_frame;     /* its USB frame index, see lpc4370sdr_frame_header_t */
  volatile uint32_t sof_divider;   /* tagging divider in effect, 0 = off */
  volatile uint32_t calib_xtal_nominal;
  volatile int32_t  calib_ppb;
  volatile uint32_t calib_source;
  volatile uint32_t delivered; /* chunks whose USB transfer completed (USB ISR) */
  volatile uint32_t lost; /* chunks never queued because the DMA overwrote them first (main loop) */
  volatile uint32_t queued; /* chunks handed to the USB controller (main loop) */
  volatile uint32_t usb_errors; /* bulk IN transfers the USB controller retired with an error (USB ISR) */
  volatile uint32_t packing; /* lpc4370sdr_packing_type in effect */
  volatile uint32_t dma_done; /* chunks the ADC DMA has completed (M4 DMA ISR) */
  volatile uint32_t overrun_last; /* index of the last chunk counted in overruns */
  volatile uint32_t usb_td[LPC4370SDR_STREAM_USB_POOL]; /* the stream's USB transfer descriptors, set by the M0 */
  volatile uint32_t usb_qh; /* the bulk IN endpoint's queue head, set by the M0 */
  volatile uint32_t first_chunk; /* chunk counter value of the stream's first chunk, which fills ring position 0 */
  volatile uint32_t start_count; /* streams the M4 started since boot: lpc4370sdr_frame_header_t stream_start_count */
  /* Auxiliary UART receive (M0 UART ISR), for the PPS capture: bytes stored in the receive ring
     since boot, and the PPS timer's count read once the ISR had them - the last byte arrived no
     later than that */
  volatile uint32_t uart_seq;
  volatile uint32_t uart_in;
  volatile uint32_t uart_tick;
  volatile uint32_t uart_timer_tc; /* address of the PPS timer's count register, 0 = none yet (M4) */
} lpc4370sdr_stream_state_t;

extern uint32_t cm4_data_share; /* defined in linker script */
#define LPC4370SDR_STREAM_STATE ((volatile lpc4370sdr_stream_state_t *)&cm4_data_share)

#ifdef __cplusplus
}
#endif

#endif /* __LPC4370SDR_STREAM_H */
