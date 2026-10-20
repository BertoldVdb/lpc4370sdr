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

#include <libopencm3/lpc43xx/uart.h>
#include <libopencm3/lpc43xx/cgu.h>
#include <libopencm3/lpc43xx/ccu.h>
#include <libopencm3/lpc43xx/scu.h>
#include <libopencm3/lpc43xx/m0/nvic.h>
#include <libopencm3/cm3/nvic.h>

#include "lpc4370sdr_conf.h"
#include "lpc4370sdr_stream.h"
#include "uart.h"
#include "m0.h"

#define UART_RX_RING (1024)
#define UART_TX_RING (256)

typedef struct
{
  uint32_t base;                /* UARTn register base */
  uint32_t irq;
  volatile uint32_t* base_clk;  /* CGU_BASE_UARTn_CLK */
  volatile uint32_t* ccu1_cfg;
  volatile uint32_t* ccu2_cfg;  /* CCU2_CLK_APB0_UARTn_CFG, same layout */
  uint32_t rx_pin, rx_func;
  uint32_t tx_pin, tx_func;
} uart_port_t;

static const uart_port_t port_usart0 = {
  UART0, NVIC_USART0_IRQ, &CGU_BASE_UART0_CLK, &CCU1_CLK_M4_USART0_CFG, &CCU2_CLK_APB0_USART0_CFG,
  P2_1, SCU_CONF_FUNCTION1, /* U0_RXD */
  P2_0, SCU_CONF_FUNCTION1  /* U0_TXD */
};

#ifdef LPC4370SDR_NO_FLASH
static const uart_port_t port_uart1 = {
  UART1, NVIC_UART1_IRQ, &CGU_BASE_UART1_CLK, &CCU1_CLK_M4_UART1_CFG, &CCU2_CLK_APB0_UART1_CFG,
  P3_5, SCU_CONF_FUNCTION4, /* U1_RXD, flash pad 3 (/WP) */
  P3_4, SCU_CONF_FUNCTION4  /* U1_TXD, flash pad 7 (/HOLD) */
};
#endif

static const uart_port_t* port;

static volatile uint8_t rx_ring[UART_RX_RING];
static volatile uint32_t rx_head, rx_tail;
static volatile uint32_t rx_taken; /* bytes taken out of the ring since boot, see lpc4370sdr_uart_taken() */
static volatile lpc4370sdr_stream_state_t* const ustream = LPC4370SDR_STREAM_STATE;
static volatile uint8_t tx_ring[UART_TX_RING];
static volatile uint32_t tx_head, tx_tail;
static uint32_t current_baud;

static const uart_port_t* board_port(void)
{
  if(!LPC4370SDR_HW_MINI_PINS(lpc4370sdr_conf->conf_hw.hardware_type))
    return &port_usart0;
#ifdef LPC4370SDR_NO_FLASH
  return &port_uart1; /* Airspy Mini */
#else
  return 0; /* the flash chip owns those pads */
#endif
}

int lpc4370sdr_uart_available(void)
{
  return board_port() != 0;
}

static uint32_t uart_clock_hz(void)
{
  return (lpc4370sdr_conf->conf_hw.hardware_type & HW_FEATURE_SI5351C) ? 20000000 : 24000000;
}

void lpc4370sdr_uart_init(uint32_t baud)
{
  uint32_t pclk = uart_clock_hz();
  uint32_t best_dl = 1, best_mul = 1, best_div = 0;
  uint64_t best_err = ~0ull;
  uint32_t mul, div;

  port = board_port();
  if(port == 0)
    return;

  if(baud < LPC4370SDR_UART_BAUD_MIN || baud > LPC4370SDR_UART_BAUD_MAX)
    baud = LPC4370SDR_UART_DEFAULT_BAUD;

  for(mul = 1; mul < 16; mul++)
  {
    for(div = 0; div < mul; div++)
    {
      uint64_t denom = 16ull * baud * (mul + div);
      uint32_t dl = (uint32_t)((((uint64_t)pclk * mul) + denom / 2) / denom);
      uint64_t actual, err;
      if(dl < 1 || (div && dl < 3) || dl > 0xFFFF)
        continue;
      actual = ((uint64_t)pclk * mul * 1000) / (16ull * dl * (mul + div)); /* milli-baud */
      err = actual > (uint64_t)baud * 1000 ? actual - (uint64_t)baud * 1000 : (uint64_t)baud * 1000 - actual;
      if(err < best_err)
      {
        best_err = err;
        best_dl = dl;
        best_mul = mul;
        best_div = div;
      }
    }
  }

  nvic_disable_irq(port->irq);

  *port->base_clk = CGU_BASE_UART0_CLK_AUTOBLOCK | CGU_BASE_UART0_CLK_CLK_SEL(CGU_SRC_GP_CLKIN);
  port->ccu1_cfg[0] |= 1;
  while((port->ccu1_cfg[1] & 1) == 0);
  port->ccu2_cfg[0] |= 1;
  while((port->ccu2_cfg[1] & 1) == 0);

  scu_pinmux(port->rx_pin, port->rx_func | SCU_CONF_EZI_EN_IN_BUFFER); /* pull-up keeps RX idle high */
  scu_pinmux(port->tx_pin, port->tx_func);

  UART_IER(port->base) = 0;
  UART_LCR(port->base) = UART_LCR_DLAB_EN | UART_LCR_WLEN8;
  UART_DLL(port->base) = best_dl & 0xFF;
  UART_DLM(port->base) = (best_dl >> 8) & 0xFF;
  UART_LCR(port->base) = UART_LCR_WLEN8 | UART_LCR_ONE_STOPBIT | UART_LCR_NO_PARITY;
  UART_FDR(port->base) = UART_FDR_MULVAL(best_mul) | UART_FDR_DIVADDVAL(best_div);
  UART_FCR(port->base) = UART_FCR_FIFO_EN | UART_FCR_RX_RS | UART_FCR_TX_RS; /* RX trigger: 1 byte */

  rx_head = rx_tail = 0;
  rx_taken = ustream->uart_in; /* what the ring held is dropped: taken */
  tx_head = tx_tail = 0;
  current_baud = baud;

  UART_IER(port->base) = UART_IER_RBRINT_EN;
  nvic_set_priority(port->irq, M0_IRQ_PRIORITY_UART);
  nvic_enable_irq(port->irq);
}

uint32_t lpc4370sdr_uart_baud(void)
{
  return current_baud;
}

void lpc4370sdr_uart_flush_rx(void)
{
  if(port)
    nvic_disable_irq(port->irq);
  rx_tail = rx_head;
  rx_taken = ustream->uart_in;
  if(port)
    nvic_enable_irq(port->irq);
}

uint32_t lpc4370sdr_uart_taken(void)
{
  return rx_taken;
}

uint32_t lpc4370sdr_uart_read(uint8_t* dst, uint32_t max)
{
  uint32_t n = 0;
  while(n < max && rx_tail != rx_head)
  {
    dst[n++] = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) & (UART_RX_RING - 1);
  }
  rx_taken += n;
  return n;
}

static void tx_fill_fifo(void)
{
  uint32_t n = 16; /* FIFO depth after THRE */
  while(n-- && tx_tail != tx_head)
  {
    UART_THR(port->base) = tx_ring[tx_tail];
    tx_tail = (tx_tail + 1) & (UART_TX_RING - 1);
  }
}

uint32_t lpc4370sdr_uart_write(const uint8_t* src, uint32_t len)
{
  uint32_t n = 0;
  if(port == 0)
    return 0;
  /* Accept all or nothing, so a host that retries a refused write sends no byte twice */
  if(((tx_tail - tx_head - 1) & (UART_TX_RING - 1)) < len)
    return 0;
  while(n < len)
  {
    tx_ring[tx_head] = src[n++];
    tx_head = (tx_head + 1) & (UART_TX_RING - 1);
  }
  nvic_disable_irq(port->irq);
  if(UART_LSR(port->base) & UART_LSR_THRE)
    tx_fill_fifo();
  UART_IER(port->base) = UART_IER_RBRINT_EN | UART_IER_THREINT_EN;
  nvic_enable_irq(port->irq);
  return n;
}

static void uart_isr(void)
{
  uint32_t iir = UART_IIR(port->base); /* reading it clears the THRE interrupt */
  uint32_t stored = 0;
  (void)iir;

  while(UART_LSR(port->base) & UART_LSR_RDR)
  {
    uint8_t c = UART_RBR(port->base);
    uint32_t next = (rx_head + 1) & (UART_RX_RING - 1);
    if(next != rx_tail)
    {
      rx_ring[rx_head] = c;
      rx_head = next;
      stored++;
    }
  }

  /* For the PPS capture: the bytes and the PPS timer's count, read after them, so every byte
     counted arrived no later than the count says */
  if(stored)
  {
    uint32_t tc = ustream->uart_timer_tc;
    ustream->uart_seq = (ustream->uart_seq + 1) | 1;
    ustream->uart_in += stored;
    ustream->uart_tick = tc ? MMIO32(tc) : 0;
    ustream->uart_seq = ustream->uart_seq + 1;
  }

  if(UART_LSR(port->base) & UART_LSR_THRE)
  {
    if(tx_tail != tx_head)
      tx_fill_fifo();
    else
      UART_IER(port->base) = UART_IER_RBRINT_EN;
  }
}

void usart0_isr(void)
{
  uart_isr();
}

void uart1_isr(void)
{
  uart_isr();
}
