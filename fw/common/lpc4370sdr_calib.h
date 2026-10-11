/*
 * Copyright 2016-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
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

#ifndef __LPC4370SDR_CALIB_H__
#define __LPC4370SDR_CALIB_H__

#ifdef __cplusplus
extern "C"
{
#endif

#define LPC4370SDR_FLASH_CALIB_OFFSET (0x20000) /* After 128KB (0 to 128KB reserved for firmware) */
#define LPC4370SDR_FLASH_CALIB_HEADER (0xCA1B0001)
#define LPC4370SDR_CALIBRATION_PPB_MAX (1000000) /* corrections beyond 1000 ppm are refused or clamped */

/* For each configuration the index corresponds to uint32_t */
typedef struct
{
  uint32_t header; /* Shall be equal to LPC4370SDR_FLASH_CALIB_HEADER */
  uint32_t timestamp; /* Epoch Unix Time Stamp */
  int32_t correction_ppb;
} lpc4370sdr_calib_t;

#ifdef __cplusplus
}
#endif

#endif /* __LPC4370SDR_CALIB_H__ */
