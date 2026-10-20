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

/* The ADC ring, lpc4370sdr_stream_pieces[] in lpc4370sdr_stream.h, takes the ram_*_usb regions in
   full: the linker places nothing there, and they must stay where that table has them. */
ASSERT(ORIGIN(ram_local1_usb) == 0x10008000 && LENGTH(ram_local1_usb) == 6 * 16K &&
       ORIGIN(ram_local2_usb) == 0x10080000 && LENGTH(ram_local2_usb) == 4 * 16K &&
       ORIGIN(ram_ahb2_usb) == 0x2000C000 && LENGTH(ram_ahb2_usb) == 16K,
       "the ram_*_usb regions do not match lpc4370sdr_stream_pieces[]")
