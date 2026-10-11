/*
 * Copyright 2014-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
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

#ifndef __M0_H__
#define __M0_H__

#include "lpc4370sdr_commands.h"
#include "lpc4370sdr_stream.h"
#include "usb_type.h"

void ADCHS_start(uint8_t conf_num);
void ADCHS_stop(uint8_t conf_num);

void set_samplerate_m4(uint8_t conf_num);
void set_packing_m4(uint8_t state);
void set_framing_m4(uint8_t state);
void set_sof_divider_m4(uint32_t divider);

/* The same image runs on the HydraSDR RFOne and on Airspy boards */
typedef enum
{
  BOARD_HYDRASDR_RFONE = 0, /* R828D tuner */
  BOARD_AIRSPY_R2,          /* R820T2 tuner, SI5351C clock generator */
  BOARD_AIRSPY_MINI         /* R820T2 tuner, no SI5351C */
} board_type_t;

extern board_type_t board_type;
#define BOARD_IS_AIRSPY() (board_type != BOARD_HYDRASDR_RFONE)

/* Ring buffer state shared with the M4 */
extern volatile lpc4370sdr_stream_state_t * const stream;

/* Bulk IN completion callback, USB ISR context */
void usb_bulk_in_transfer_complete(usb_endpoint_t* const endpoint);

#endif//__M0_H__
