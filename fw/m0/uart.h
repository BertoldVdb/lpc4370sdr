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

/* Serial pass-through for a GNSS module (or anything serial) */
#ifndef __LPC4370SDR_UART_H__
#define __LPC4370SDR_UART_H__

#include <stdint.h>

#define LPC4370SDR_UART_DEFAULT_BAUD (9600)
#define LPC4370SDR_UART_BAUD_MIN (300)
#define LPC4370SDR_UART_BAUD_MAX (1000000)

int lpc4370sdr_uart_available(void);
void lpc4370sdr_uart_init(uint32_t baud);
void lpc4370sdr_uart_flush_rx(void);
uint32_t lpc4370sdr_uart_read(uint8_t* dst, uint32_t max);  /* stream_queue_chunks(): the USB ISR, or the main loop with interrupts off */
/* Bytes taken out of the receive ring since boot, read or dropped: with the stream state's uart_in
   (bytes stored) it numbers every byte, so a PPS edge can be placed among them */
uint32_t lpc4370sdr_uart_taken(void);
uint32_t lpc4370sdr_uart_write(const uint8_t* src, uint32_t len); /* all or nothing: returns len or 0 */
uint32_t lpc4370sdr_uart_baud(void);

#endif
