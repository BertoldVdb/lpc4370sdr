/*
 * Copyright 2012 Jared Boone
 * Copyright 2013-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from HydraSDR, based on the HackRF project).
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

#include "usb.h"
#include "usb_type.h"
#include "usb_request.h"
#include "usb_descriptor.h"
#include "usb_standard_request.h"

#include "m0.h"
#include "usb_req.h"
#include "lpc4370sdr_commands.h"
#include "r82x.h"
#include "watchdog.h"

extern usb_endpoint_t usb_endpoint_bulk_in;

static volatile receiver_mode_t receiver_mode = RECEIVER_MODE_OFF;

void set_receiver_mode(const receiver_mode_t new_receiver_mode)
{
  usb_streaming_disable();

  if( new_receiver_mode == RECEIVER_MODE_RX )
  {
    usb_endpoint_init(&usb_endpoint_bulk_in);
    ADCHS_start(sample_rate_conf_no);
    watchdog_host_contact();
  }else
  {
    ADCHS_stop(sample_rate_conf_no);
    /* The next stream starts unframed and untagged */
    set_framing_m4(0);
    set_sof_divider_m4(0);
  }
  receiver_mode = new_receiver_mode;
}

receiver_mode_t get_receiver_mode(void)
{
  return receiver_mode;
}


