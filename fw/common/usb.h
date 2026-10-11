/*
 * Copyright 2012 Jared Boone
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

#ifndef __USB_H__
#define __USB_H__

// TODO: Refactor to support high performance operations without having to
// expose usb_transfer_descriptor_t. Or usb_endpoint_prime(). Or, or, or...
#include <libopencm3/lpc43xx/usb.h>

#include "usb_type.h"

void usb_peripheral_reset();

/* SETUP packets received so far: a request handled late checks it is still the current one */
extern volatile uint32_t usb_setup_count;

void usb_device_init(
  const uint_fast8_t device_ordinal,
  usb_device_t* const device
);

void usb_run(
  usb_device_t* const device
);

void usb_run_tasks(
  const usb_device_t* const device
);

usb_speed_t usb_speed(
  const usb_device_t* const device
);

void usb_set_address_immediate(
  const usb_device_t* const device,
  const uint_fast8_t address
);

void usb_set_address_deferred(
  const usb_device_t* const device,
  const uint_fast8_t address
);

usb_queue_head_t* usb_queue_head(const uint_fast8_t endpoint_address);

void usb_endpoint_init(
  const usb_endpoint_t* const endpoint
);

void usb_endpoint_stall(
  const usb_endpoint_t* const endpoint
);

void usb_endpoint_disable(
  const usb_endpoint_t* const endpoint
);

void usb_endpoint_flush(
  const usb_endpoint_t* const endpoint
);

bool usb_endpoint_is_ready(
  const usb_endpoint_t* const endpoint
);

void usb_endpoint_prime(
  const usb_endpoint_t* const endpoint,
  usb_transfer_descriptor_t* const first_td
);

void usb_endpoint_schedule_wait(
  const usb_endpoint_t* const endpoint,
  usb_transfer_descriptor_t* const td
);

void usb_endpoint_schedule_append(
  const usb_endpoint_t* const endpoint,
  usb_transfer_descriptor_t* const tail_td,
  usb_transfer_descriptor_t* const new_td
);

#endif//__USB_H__
