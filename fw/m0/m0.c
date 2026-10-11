/*
 * Copyright 2012 Jared Boone
 * Copyright 2015 Ian Gilmour <ian@sdrsharp.com>
 * Copyright 2013-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
 *
 * This file is part of lpc4370sdr (derived from HydraSDR, based on the AirSpy/HackRF project).
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

#include <string.h>

#include <libopencm3/lpc43xx/cgu.h>
#include <libopencm3/lpc43xx/i2c.h>
#include <libopencm3/lpc43xx/gpio.h>
#include <libopencm3/lpc43xx/m0/nvic.h>
#include <libopencm3/lpc43xx/creg.h>
#include <libopencm3/lpc43xx/rgu.h>
#include <libopencm3/cm3/cortex.h>
#include <libopencm3/cm3/cortex.h>

#include <core.h>
#include <si5351c.h>
#include <r82x.h>
#include <w25q80bv.h>
#include <rom_iap.h>
#include <signal_mcu.h>

#include "usb.h"
#include "usb_standard_request.h"

#include "usb_device.h"
#include "usb_endpoint.h"
#include "usb_descriptor.h"

#include "lpc4370sdr_conf.h"
#include "usb_req.h"
#include "lpc4370sdr_commands.h"
#include "rx.h"
#include "r82x.h"
#include "m0.h"
#include "lpc4370sdr_stream.h"
#include "watchdog.h"
#include "uart.h"
#include "m0.hdr"

extern uint32_t cm0_data_share; /* defined in linker script */

volatile unsigned int phase = 0;

board_type_t board_type = BOARD_HYDRASDR_RFONE;

volatile lpc4370sdr_stream_state_t * const stream = LPC4370SDR_STREAM_STATE;
static volatile uint32_t stream_epoch = 0;

volatile mcore_t *start_adchs = (mcore_t *)(&cm0_data_share);
volatile mcore_t *set_samplerate = (mcore_t *)((&cm0_data_share)+1);
volatile mcore_t *set_packing = (mcore_t *)((&cm0_data_share)+2);
volatile mcore_t *set_framing = (mcore_t *)((&cm0_data_share)+3);
volatile mcore_t *set_sof = (mcore_t *)((&cm0_data_share)+4);
volatile uint32_t *set_sof_divider = (uint32_t *)((&cm0_data_share)+5);

#define MASTER_TXEV_FLAG  ((uint32_t *) 0x40043130)
#define MASTER_TXEV_QUIT()  { *MASTER_TXEV_FLAG = 0x0; }


const char version_string[] = " " LPC4370SDR_FW_GIT_TAG " " LPC4370SDR_FW_CHECKIN_DATE;

typedef struct {
  uint32_t freq_hz;
  uint32_t divider;
} set_sample_r_params_t;

set_sample_r_params_t set_sample_r_params;

__attribute__ ((always_inline)) static inline void start_stop_adchs_m4(uint8_t conf_num, uint8_t command)
{
  start_adchs->conf = conf_num;
  start_adchs->cmd = command;

  signal_sev();

  /* Wait until M4 have finished executing the command (it set the data to 0) */
  while(1)
  {
    if(start_adchs->raw == 0)
      break;
  }
}

void set_samplerate_m4(uint8_t conf_num)
{
  set_samplerate->conf = conf_num;
  set_samplerate->cmd = SET_SAMPLERATE_CMD;

  signal_sev();

  /* Wait until M4 have finished executing the command (it set the data to 0) */
  while(1)
  {
    if(set_samplerate->raw == 0)
      break;
  }
}

void set_packing_m4(uint8_t state)
{
  set_packing->conf = state;
  set_packing->cmd = SET_PACKING_CMD;
  
  signal_sev();
  
  while(1)
  {
    if(set_packing->raw == 0)
      break;
  }
}

void set_framing_m4(uint8_t state)
{
  set_framing->conf = state;
  set_framing->cmd = SET_FRAMING_CMD;

  signal_sev();

  while(1)
  {
    if(set_framing->raw == 0)
      break;
  }
}

void set_sof_divider_m4(uint32_t divider)
{
  *set_sof_divider = divider;
  set_sof->cmd = SET_SOF_CMD;

  signal_sev();

  while(1)
  {
    if(set_sof->raw == 0)
      break;
  }
}

#ifdef LPC4370SDR_EXT_TEST
/* test build: counters jump to this short of their 16-bit wrap in the header once they leave 0
   (0 keeps meaning "none yet"), so a host's extension of them is tried within seconds */
#define EXT_TEST_PPS_OFFSET (65530u)
#define EXT_TEST_SOF_OFFSET (65000u)
#define EXT_TEST_LOST_OFFSET (65530u)
#else
#define EXT_TEST_PPS_OFFSET (0u)
#define EXT_TEST_SOF_OFFSET (0u)
#define EXT_TEST_LOST_OFFSET (0u)
#endif

static void stream_write_ext(uint8_t* ext, uint32_t chunk_index)
{
#ifdef LPC4370SDR_EXT_TEST
  lpc4370sdr_ext_test_fill(ext, chunk_index);
#else
  uint32_t i;
  for(i = 0; i < LPC4370SDR_FRAME_EXT_BYTES; i++)
    ext[i] = 0;
  if((chunk_index % LPC4370SDR_EXT_HEALTH_EVERY) == 0)
  {
    const uint32_t v[3] = { stream->dma_errors, stream->adc_overflows, stream->usb_errors };
    const uint32_t b = stream->backlog_max, l = stream->m0_lag_max;
    ext[0] = LPC4370SDR_EXT_HEALTH;
    ext[1] = 16;
    for(i = 0; i < 12; i++)
      ext[2 + i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
    ext[14] = (uint8_t)(b > 0xFFFF ? 0xFF : b);
    ext[15] = (uint8_t)(b > 0xFFFF ? 0xFF : b >> 8);
    ext[16] = (uint8_t)(l > 0xFFFF ? 0xFF : l);
    ext[17] = (uint8_t)(l > 0xFFFF ? 0xFF : l >> 8);
  }
#endif
}

static void stream_write_header(uint8_t* chunk, uint32_t chunk_index, uint32_t chunk_samples)
{
  lpc4370sdr_frame_header_t* h = (lpc4370sdr_frame_header_t*)chunk;
  const uint32_t* w = (const uint32_t*)chunk;
  uint32_t pps_uart_in, sum = 0, i, back;
  int32_t rel;

  h->magic = LPC4370SDR_FRAME_MAGIC;
  h->chunk_index = chunk_index;
  h->version = LPC4370SDR_FRAME_VERSION;
  h->flags = (stream->packing == LPC4370SDR_PACKING_ON) ? LPC4370SDR_FRAME_FLAG_PACKED :
             (stream->packing == LPC4370SDR_PACKING_TIMESTAMP_ON) ? (LPC4370SDR_FRAME_FLAG_PACKED | LPC4370SDR_FRAME_FLAG_TIMESTAMP) :
             (stream->packing == LPC4370SDR_PACKING_8BIT) ? LPC4370SDR_FRAME_FLAG_8BIT : 0;
  h->flags |= LPC4370SDR_FRAME_FLAG_TAIL;
  h->stream_start_count = (uint8_t)stream->start_count; /* a START while running does not count */
  h->sample_count = (uint16_t)chunk_samples;
  h->lost_chunks = (uint16_t)(stream->lost ? stream->lost + EXT_TEST_LOST_OFFSET : 0);
  h->overrun_chunks = (uint16_t)(stream->overruns ? stream->overruns + EXT_TEST_LOST_OFFSET : 0);
  back = chunk_index - stream->overrun_last;
  h->overrun_back = (uint16_t)(back > 0xFFFF ? 0xFFFF : back);
  h->freq_hz = (uint32_t)set_freq_params.freq_hz;
  {
    uint32_t s1, s2;
    uint32_t tries = 16;
    do
    {
      s1 = stream->pps_seq;
      h->pps_sample_index_lo = stream->pps_index_lo;
      h->pps_fraction = (uint16_t)(stream->pps_fraction >> 16);
      h->pps_count = (uint16_t)(stream->pps_count ? stream->pps_count + EXT_TEST_PPS_OFFSET : 0);
      h->pps_uart_quiet_us = (uint16_t)stream->pps_uart_quiet_us;
      pps_uart_in = stream->pps_uart_in;
      s2 = stream->pps_seq;
    } while(((s1 != s2) || (s1 & 1)) && --tries);
  }
  {
    uint32_t s1, s2;
    uint32_t tries = 16;
    do
    {
      s1 = stream->sof_seq;
      h->sof_sample_index_lo = stream->sof_index_lo;
      h->sof_fraction = (uint16_t)(stream->sof_fraction >> 16);
      h->sof_frame = (uint16_t)stream->sof_frame;
      h->sof_count = (uint16_t)(stream->sof_count ? stream->sof_count + EXT_TEST_SOF_OFFSET : 0);
      s2 = stream->sof_seq;
    } while(((s1 != s2) || (s1 & 1)) && --tries);
  }
  h->uart_len = (uint8_t)lpc4370sdr_uart_read(h->uart_data, LPC4370SDR_FRAME_UART_BYTES);
  for(i = h->uart_len; i < LPC4370SDR_FRAME_UART_BYTES; i++)
    h->uart_data[i] = 0;
  /* Where the edge falls among the bytes: both counts number the same bytes, the ring's order */
  rel = (int32_t)(pps_uart_in - lpc4370sdr_uart_taken());
  h->pps_uart_rel = (int16_t)(rel > 32767 ? 32767 : rel < -32768 ? -32768 : rel);
  stream_write_ext(h->ext, chunk_index);

  for(i = 0; i < LPC4370SDR_FRAME_HEADER_SIZE / 4 - 1; i++)
    sum += w[i];
  h->check = ~sum;
}

void usb_configuration_changed(usb_device_t* const device)
{
  if( device->configuration->number )
  {
    /* RECEIVER ON */
    set_receiver_mode(get_receiver_mode());
  } else
  {
    /* RECEIVER OFF */
    /* Configuration number equal 0 means usb bus reset. */
    set_receiver_mode(RECEIVER_MODE_OFF);
  }
}

void ADCHS_start(uint8_t conf_num)
{
  uint32_t i;

  lpc4370sdr_uart_flush_rx();
  for(i = 0; i < LPC4370SDR_STREAM_USB_POOL; i++)
    stream->usb_td[i] = (uint32_t)usb_endpoint_bulk_in_td(i);
  stream->usb_qh = (uint32_t)usb_queue_head(usb_endpoint_bulk_in.address);
  stream->usb_errors = 0;

  start_stop_adchs_m4(conf_num, START_ADCHS_CMD); /* resets the chunk counters */

  //enable_r82x_power();

  /* Re-Init I2C0 & I2C1 after PLL1 frequency is modified (for I2C1 also because PowerOn on R82X) */
  i2c0_init(lpc4370sdr_conf->i2c_conf.i2c0_pll1_ls_hs_conf_val); /* Si5351C I2C peripheral */
  i2c1_init(lpc4370sdr_conf->i2c_conf.i2c1_pll1_hs_conf_val); /* R82X I2C peripheral */

  if((conf_num & LPC4370SDR_SAMPLERATE_CONF_ALT) == LPC4370SDR_SAMPLERATE_CONF_ALT)
  {
    conf_num = conf_num & (~LPC4370SDR_SAMPLERATE_CONF_ALT);
    r82x_init(&lpc4370sdr_conf->r82x_conf_rw, lpc4370sdr_conf->m0_m4_alt_conf[conf_num].m0_conf.r82x_if_freq);
    r82x_set_if_bandwidth(&lpc4370sdr_conf->r82x_conf_rw, lpc4370sdr_conf->m0_m4_alt_conf[conf_num].m0_conf.r82x_if_bw);
  }else
  {
    r82x_init(&lpc4370sdr_conf->r82x_conf_rw, lpc4370sdr_conf->m0_m4_conf[conf_num].m0_conf.r82x_if_freq);
    r82x_set_if_bandwidth(&lpc4370sdr_conf->r82x_conf_rw, lpc4370sdr_conf->m0_m4_conf[conf_num].m0_conf.r82x_if_bw);
  }
  phase = 1;
  stream_epoch++;
}

/* Framing and SOF tagging stay as they are: a setting change restarts the stream with them.
   set_receiver_mode() clears them when the receiver goes off. */
void ADCHS_stop(uint8_t conf_num)
{
  /* Retire the transfers still pointing into the ring before the M4 clears and refills it */
  usb_endpoint_flush(&usb_endpoint_bulk_in);
  r82x_standby();
  start_stop_adchs_m4(conf_num, STOP_ADCHS_CMD);

  /* Re-Init I2C0 & I2C1 after PLL1 frequency is modified. The M4 leaves PLL1 at its high speed
     once the first stream started (its low speed switch is disabled). */
  i2c0_init(lpc4370sdr_conf->i2c_conf.i2c0_pll1_ls_hs_conf_val); /* Si5351C I2C peripheral */
  i2c1_init(lpc4370sdr_conf->i2c_conf.i2c1_pll1_hs_conf_val); /* R82X I2C peripheral */
  stream_epoch++;
}

static void stream_queue_chunks(void);
static volatile uint32_t stream_queued;
static uint32_t stream_queued_pos; /* ring position of chunk stream_queued */
static volatile uint32_t stream_run_epoch;
static uint32_t queued_start_count = ~0u; /* stream->start_count of the stream being queued */

void usb_bulk_in_transfer_complete(usb_endpoint_t* const endpoint)
{
  uint32_t before = usb_queue_active_count(endpoint);
  uint32_t errors = usb_queue_transfer_errors;
  usb_queue_transfer_complete(endpoint);
  stream->delivered += before - usb_queue_active_count(endpoint);
  stream->usb_errors += usb_queue_transfer_errors - errors;
  watchdog_stream_activity();
  if(stream_run_epoch == stream_epoch && get_receiver_mode() == RECEIVER_MODE_RX)
    stream_queue_chunks();
  signal_sev();
}

/* Move the queue position forward by n chunks */
static void stream_queue_advance(uint32_t n, uint32_t ring_chunks)
{
  stream_queued += n;
  stream_queued_pos += (n < ring_chunks) ? n : n % ring_chunks;
  if(stream_queued_pos >= ring_chunks)
    stream_queued_pos -= ring_chunks;
}

static void stream_queue_chunks(void)
{
  uint32_t queued = stream_queued;
  uint32_t captured = stream->captured;
  uint32_t dma_done = stream->dma_done;
  uint32_t ring_chunks = stream->ring_chunks;
  uint32_t chunk_slots = stream->chunk_slots;
  uint32_t chunk_bytes = stream->chunk_bytes;
  uint32_t header_bytes = stream->header_bytes;
  uint32_t chunk_samples = stream->chunk_samples;
  int32_t pending = (int32_t)(captured - queued);

  if(pending < 0 || ring_chunks <= LPC4370SDR_STREAM_QUEUE_GUARD)
    return;

  /* Skip the chunks the DMA would reach before the transfers ahead of them drain */
  {
    uint32_t oldest = dma_done - (ring_chunks - LPC4370SDR_STREAM_QUEUE_GUARD);
    if((int32_t)(oldest - captured) > 0)
      oldest = captured;
    if((int32_t)(oldest - queued) > 0)
    {
      stream->lost += oldest - queued;
      stream_queue_advance(oldest - queued, ring_chunks);
    }
  }

  /* Write a header only for a chunk that gets a transfer: the slot may still be sent by an older
     pending transfer the DMA lapped, which must not go out under this chunk's header */
  while((int32_t)(captured - stream_queued) > 0 && usb_queue_has_free_transfer(&usb_endpoint_bulk_in))
  {
    uint8_t* chunk = lpc4370sdr_stream_slot(stream_queued_pos * chunk_slots);
    if(header_bytes)
      stream_write_header(chunk, stream_queued, chunk_samples);
    if(usb_transfer_schedule_tag(&usb_endpoint_bulk_in, chunk, chunk_bytes, stream_queued) != 0)
      break;
    stream_queue_advance(1, ring_chunks);
  }

  stream->queued = stream_queued;
}

/* The R2 shares the RFOne's table {10, 5, 2.5, 15 MSPS}; stock Airspy firmware has 2.5 MSPS at
   index 1, which hosts may select by index. Both cores read the table in shared RAM and no stream
   runs yet, so swap the two entries. */
static void airspy_r2_samplerate_order(void)
{
  m0_m4_conf_t* conf = lpc4370sdr_conf->m0_m4_conf;
  if(lpc4370sdr_conf->nb_m0_m4_conf_t >= 3 &&
     conf[1].m0_conf.r82x_if_freq == 2500000 && conf[2].m0_conf.r82x_if_freq == 1250000)
  {
    m0_m4_conf_t t = conf[1];
    conf[1] = conf[2];
    conf[2] = t;
  }
}

/* adchs_isr managed by M4 */
void m4core_isr(void)
{
  MASTER_TXEV_QUIT();
}

/*
M0 Core Manage USB 
*/
int main(void)
{
  iap_cmd_res_t iap_cmd_res;
  usb_descriptor_serial_number_t serial_number;
  usb_req_init();

  watchdog_init();
  lpc4370sdr_uart_init(LPC4370SDR_UART_DEFAULT_BAUD);

  /* An R820T2 means Airspy hardware: the clock generator tells the R2 from the Mini. Only the
     RFOne has an R828D, and the Mini is the only board without the SI5351C. */
  {
    uint8_t tuner = 0;
    uint32_t tries;
    for(tries = 0; tries < 5 && tuner == 0; tries++)
      tuner = r82x_detect();
    if(tuner == R820T_I2C_ADDR || !(lpc4370sdr_conf->conf_hw.hardware_type & HW_FEATURE_SI5351C))
    {
      board_type = (lpc4370sdr_conf->conf_hw.hardware_type & HW_FEATURE_SI5351C) ? BOARD_AIRSPY_R2 : BOARD_AIRSPY_MINI;
      if(tuner != R820T_I2C_ADDR)
        r82x_i2c_addr = R820T_I2C_ADDR;
      if(board_type == BOARD_AIRSPY_R2)
        airspy_r2_samplerate_order();
      usb_descriptor_use_airspy();
    }
  }

  /* R82X Startup */
  r82x_startup(&lpc4370sdr_conf->r82x_conf_rw);

  usb_set_configuration_changed_cb(usb_configuration_changed);
  usb_peripheral_reset();

  usb_device_init(0, &usb_device);

  usb_queue_init(&usb_endpoint_control_out_queue);
  usb_queue_init(&usb_endpoint_control_in_queue);
  usb_queue_init(&usb_endpoint_bulk_out_queue);
  usb_queue_init(&usb_endpoint_bulk_in_queue);

  usb_endpoint_init(&usb_endpoint_control_out);
  usb_endpoint_init(&usb_endpoint_control_in);

  /* Read IAP Serial Number Identification */
  iap_cmd_res.cmd_param.command_code = IAP_CMD_READ_SERIAL_NO;
  iap_cmd_call(&iap_cmd_res);
  if(iap_cmd_res.status_res.status_ret == CMD_SUCCESS)
  {
    /* Only retrieve 2 last 32bits for Serial Number */
    serial_number.sn_32b[0] = iap_cmd_res.status_res.iap_result[2];
    serial_number.sn_32b[1] = iap_cmd_res.status_res.iap_result[3];
    usb_descriptor_fill_string_serial_number(serial_number);
  }

  nvic_set_priority(NVIC_USB0_IRQ, 255);
  
  nvic_set_priority(NVIC_M4CORE_IRQ, 1);
  nvic_enable_irq(NVIC_M4CORE_IRQ);

  usb_run(&usb_device);

  uint32_t epoch = stream_epoch;
  stream_queued = 0;
  stream_run_epoch = epoch;
  while(true)
  {
    signal_wfe();
    watchdog_heartbeat();
    deferred_job_run();
    /* The USB ISR restarts streams: check for one and queue in the same critical section */
    cm_disable_interrupts();
    if(epoch != stream_epoch)
    {
      epoch = stream_epoch;
      if(get_receiver_mode() == RECEIVER_MODE_RX && stream->start_count == queued_start_count)
      {
        /* The same stream goes on (receiver switched on again while running): the transfers the
           endpoint re-init flushed are lost, queuing continues where it was */
        usb_endpoint_flush(&usb_endpoint_bulk_in);
        stream->lost += stream_queued - (stream->delivered + stream->lost);
      }
      else if(get_receiver_mode() == RECEIVER_MODE_RX)
      {
        uint32_t captured = stream->captured;
        uint32_t first = stream->first_chunk;
        usb_endpoint_flush(&usb_endpoint_bulk_in);
        queued_start_count = stream->start_count;
        stream_queued = captured;
        stream_queued_pos = (captured - first) % stream->ring_chunks;
        stream->delivered = captured;
        stream->lost = 0;
        stream->queued = captured;
      }
      else
      {
        stream_queued = 0;
        queued_start_count = ~0u;
      }
      stream_run_epoch = epoch;
    }
    else if(get_receiver_mode() == RECEIVER_MODE_RX)
    {
      stream_queue_chunks();
    }
    cm_enable_interrupts();
  }
}
