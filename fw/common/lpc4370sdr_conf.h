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

#ifndef __LPC4370SDR_CONF_H__
#define __LPC4370SDR_CONF_H__

#include "core.h"
#include "lpc4370sdr_commands.h"
#include "r82x.h"

#ifdef __cplusplus
extern "C"
{
#endif

extern uint32_t conf_data_share; /* defined in linker script */
extern uint32_t conf_data_share_size; /* defined in linker script */

#define LPC4370SDR_SI5351C_CONFIG_XTAL (0)
#define LPC4370SDR_SI5351C_CONFIG_CLKIN (1)

#define LPC4370SDR_CONF_SRAM_ADDR (&conf_data_share)
#define LPC4370SDR_CONF_MAX_DATA_SIZE ((uint32_t)(&conf_data_share_size))

#define LPC4370SDR_SAMPLERATE_DEFAULT_CONF (0)
#define LPC4370SDR_SAMPLERATE_CONF_ALT (BIT7)

#define LPC4370SDR_CONF_NB_MAX (64) /* Maximum number of LPC4370SDR_CONF */

/* Hardware Feature/Type Definition used for hw_conf_t hardware_type field */
#define HW_FEATURE_R82X   (BIT0)
#define HW_FEATURE_SI5351C (BIT1)

#define HW_TYPE_HYDRASDR_RFONE  (HW_FEATURE_R82X|HW_FEATURE_SI5351C) /* Hardware HydraSDR RFOne (also used on the Airspy R2) */
#define HW_TYPE_AIRSPY_MINI  (HW_FEATURE_R82X) /* Hardware Airspy Mini */

/* Pin assignment of the PPS input (timing.c) and the serial pass-through: the Mini has no SI5351C */
#define LPC4370SDR_HW_MINI_PINS(hw_type) (((hw_type) & HW_FEATURE_SI5351C) == 0)

/* For each configuration the index corresponds to uint32_t */
typedef struct
{
  uint32_t r82x_if_freq;
  uint8_t r82x_if_bw; /* from 0 to 63 */
  uint8_t padding0;
  uint16_t padding1;
} m0_conf_t;

typedef struct
{
  core_clock_pll0_idivb_t lpc4370sdr_m4_conf;
  m0_conf_t m0_conf;
} m0_m4_conf_t;

typedef struct 
{
  uint16_t i2c0_pll1_ls_hs_conf_val;
  uint16_t i2c1_pll1_hs_conf_val;
  uint16_t i2c1_pll1_ls_conf_val;
  uint16_t padding;
  uint32_t spare;
} i2c_conf_t;

typedef struct
{
  uint8_t major;
  uint8_t minor;
  uint16_t revision;
} conf_struct_version_t;

typedef struct
{
  uint8_t conf[180];
} si5351c_conf_t;

typedef struct
{
  uint32_t hardware_type; /* See HW_TYPE_XXX */
  uint8_t version[16]; /* String with name of the Hardware NULL terminated */
} hw_conf_t;

typedef struct
{
  conf_struct_version_t conf_struct_version;

  hw_conf_t conf_hw;

  i2c_conf_t i2c_conf;

  core_clocks_t m4_init_conf;

  r82x_priv_t r82x_conf_rw;

  /* 1st Expansion Conf Point M0/M4 */
  uint16_t sizeof_m0_m4_conf_t;
  uint16_t nb_m0_m4_conf_t;
  /* Variable size Conf M0/M4 Addr */
  m0_m4_conf_t* m0_m4_conf; /* Filled by sys_clock_init */

  /* 2nd Expansion Conf Point M0/M4 ALT */ 
  uint16_t sizeof_m0_m4_alt_conf_t;
  uint16_t nb_m0_m4_alt_conf_t;
  /* Variable size Conf M0/M4 ALT Addr */
  m0_m4_conf_t* m0_m4_alt_conf; /* Filled by sys_clock_init */

  /* 3rd Expansion Conf Point SI5351C */ 
  uint16_t sizeof_si5351c_conf_t;
  uint16_t nb_si5351c_conf_t;
  /* Variable size Conf SI5351C Addr */ 
  si5351c_conf_t* si5351c_config; /* Filled by sys_clock_init */

  /* 1st Expansion Conf Point M0/M4 Data (mandatory) */
  /* 2nd Expansion Conf Point ALT M0/M4 Data (optional) */ 
  /* 3rd Expansion Conf Point SI5351C Data (depending on conf_hw.hardware_type) */ 
} lpc4370sdr_conf_t;

extern lpc4370sdr_conf_t* lpc4370sdr_conf;

#ifdef __cplusplus
}
#endif

#endif /* __LPC4370SDR_CONF_H__ */
