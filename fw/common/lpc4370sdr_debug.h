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

/* Debug access from the host: the CALL request's mailbox. make DEBUG_REQUESTS=0
   (LPC4370SDR_NO_DEBUG_REQUESTS) leaves MEM_WRITE and CALL out of the firmware. */
#ifndef __LPC4370SDR_DEBUG_H__
#define __LPC4370SDR_DEBUG_H__

#include <stdint.h>

typedef struct
{
  volatile uint32_t pending;  /* 1 while the M4 has not run it yet */
  volatile uint32_t address; /* function address, Thumb bit added by the callee */
  volatile uint32_t args[4];
  volatile uint32_t result;   /* R0 after the call */
} lpc4370sdr_debug_mailbox_t;

extern uint32_t cm4_data_share; /* linker script */
#define LPC4370SDR_DEBUG_MAILBOX_OFFSET (0x300)
#define LPC4370SDR_DEBUG_MAILBOX ((volatile lpc4370sdr_debug_mailbox_t *)((uint8_t *)&cm4_data_share + LPC4370SDR_DEBUG_MAILBOX_OFFSET))

#endif
