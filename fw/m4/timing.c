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

/*
 * Free-running timers capture the events. They run on PLL1 and the ADC on
 * PLL0AUDIO, IDIVB or GP_CLKIN itself, all from the same reference
 * (GP_CLKIN), so the two keep a fixed ratio. The count at the ADC trigger
 * is the origin of the sample index, so an event's sample index is its tick
 * count times the sample rate over the timer clock. Two events exist:
 *  - PPS: a rising edge on the board's PPS pin,
 *  - SOF: the USB0 controller's Start-Of-Frame pulse, which the GIMA can
 *    only feed to T3_CAP2. The RFOne/R2 PPS pin P2_2 is T3_CAP2 as well, so
 *    there the PPS goes through the pin's CTIN_6 function to T3_CAP1
 *    instead and both capture at once. Should an input ever share a
 *    channel with the SOF, the SOF takes it over while it is enabled.
 */

#include <libopencm3/lpc43xx/timer.h>
#include <libopencm3/lpc43xx/ccu.h>
#include <libopencm3/lpc43xx/scu.h>
#include <libopencm3/lpc43xx/gima.h>
#include <libopencm3/lpc43xx/usb.h>
#include <libopencm3/lpc43xx/m4/nvic.h>
#include <libopencm3/cm3/nvic.h>

#include "lpc4370sdr_conf.h"
#include "lpc4370sdr_stream.h"
#include "timing.h"

#define TIMING_PPS (0)
#define TIMING_SOF (1)

/* FRINDEX: 11-bit frame number and, at high speed, the 3-bit microframe */
#define USB_FRAME_BITS (11)
#define USB_FRAME_MASK ((1 << USB_FRAME_BITS) - 1)
#define USB_UFRAME_MASK (7)

struct capture_input;

typedef struct
{
  uint32_t base;              /* TIMERn */
  uint32_t irq;
  volatile uint32_t* ccu_cfg; /* CCU1_CLK_M4_TIMERn_CFG, its STAT follows */
  volatile uint32_t wraps;    /* TC overflows since the timer started */
  uint64_t anchor;            /* 64-bit count at the ADC trigger */
  const struct capture_input* volatile routed[4]; /* event feeding each capture channel */
} timing_timer_t;

typedef struct capture_input
{
  timing_timer_t* timer;
  uint8_t channel;            /* capture channel of that timer */
  uint8_t kind;               /* TIMING_PPS or TIMING_SOF */
  uint8_t has_pin;
  uint32_t pin;
  uint32_t pin_func;
  volatile uint32_t* gima;    /* CAPn_m_IN multiplexer feeding the channel */
  uint32_t gima_select;
} capture_input_t;

static timing_timer_t timer0 = { TIMER0, NVIC_TIMER0_IRQ, &CCU1_CLK_M4_TIMER0_CFG, 0, 0, { 0, 0, 0, 0 } };
static timing_timer_t timer3 = { TIMER3, NVIC_TIMER3_IRQ, &CCU1_CLK_M4_TIMER3_CFG, 0, 0, { 0, 0, 0, 0 } };

/* PPS pin: P2_2 as CTIN_6 (GIMA CAP3_1_IN select 0), the Airspy Mini's LED pad P1_17 is T0_CAP3 */
static const capture_input_t pps_p2_2 = { &timer3, 1, TIMING_PPS, 1, P2_2,  SCU_CONF_FUNCTION5, &GIMA_CAP3_1_IN, 0 };
static const capture_input_t pps_mini = { &timer0, 3, TIMING_PPS, 1, P1_17, SCU_CONF_FUNCTION4, &GIMA_CAP0_3_IN, 1 };
/* USB0 SOF, an internal signal: GIMA CAP3_2_IN select 2 */
static const capture_input_t sof_usb0 = { &timer3, 2, TIMING_SOF, 0, 0, 0, &GIMA_CAP3_2_IN, 2 };

static const capture_input_t* pps;
static uint32_t timer_hz;   /* the timers' clock */
static uint32_t ratio_num;  /* samples per timer tick, as a fraction */
static uint32_t ratio_den;
static volatile uint8_t armed;
static uint32_t sof_divider;
static uint32_t sof_frame_last;
static uint32_t sof_frame_base; /* extends the controller's 11-bit frame number past its wrap */

static volatile lpc4370sdr_stream_state_t* const stream = LPC4370SDR_STREAM_STATE;

/* Timer 3 count minus Timer 0 count at the same instant, measured at every stream start: it derives
 * Timer 0's anchor from Timer 3's. Both run from the same clock and are never reset, so it is a
 * constant (a global, for reading it with the debug requests). */
volatile int32_t timing_timer3_minus_timer0;
volatile uint32_t timing_offset_span; /* ticks the best measurement took, its uncertainty */
#ifdef LPC4370SDR_TIMING_DEBUG
volatile int32_t timing_debug_old_skew; /* ticks the old sequential anchor reads were apart */
#endif

static inline uint32_t irq_lock(void)
{
  uint32_t primask;
  __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
  return primask;
}

static inline void irq_unlock(uint32_t primask)
{
  __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

static uint64_t timer_now64(timing_timer_t* t)
{
  uint32_t h1, h2, tc, pending;
  do
  {
    h1 = t->wraps;
    tc = TIMER_TC(t->base);
    pending = TIMER_IR(t->base) & TIMER_IR_MR0INT;
    h2 = t->wraps;
  } while(h1 != h2);
  if(pending && tc < 0x80000000)
    h1++;
  return ((uint64_t)h1 << 32) | tc;
}

static void timer_start(timing_timer_t* t)
{
  uint32_t i;

  t->ccu_cfg[0] |= 1;
  while((t->ccu_cfg[1] & 1) == 0);

  TIMER_TCR(t->base) = TIMER_TCR_CRST;
  TIMER_TCR(t->base) = 0;
  TIMER_PR(t->base) = 0;
  TIMER_MR0(t->base) = 0xFFFFFFFF;
  TIMER_MCR(t->base) = TIMER_MCR_MR0I;
  TIMER_CCR(t->base) = 0;
  TIMER_IR(t->base) = 0xFF;

  t->wraps = 0;
  t->anchor = 0;
  for(i = 0; i < 4; i++)
    t->routed[i] = 0;

  nvic_set_priority(t->irq, 0x20); /* below the ADC DMA interrupt */
  nvic_enable_irq(t->irq);
  TIMER_TCR(t->base) = TIMER_TCR_CEN;
}

/* Feed the event to its capture channel, replacing whatever was routed there */
static void capture_route(const capture_input_t* in)
{
  timing_timer_t* t = in->timer;
  uint32_t shift = 3 * in->channel;
  uint32_t primask;

  if(t->routed[in->channel] == in)
    return;

  primask = irq_lock();
  TIMER_CCR(t->base) &= ~(7u << shift);
  *in->gima = in->gima_select << 4;
  if(in->has_pin)
    scu_pinmux(in->pin, in->pin_func | SCU_CONF_EPD_EN_PULLDOWN | SCU_CONF_EPUN_DIS_PULLUP | SCU_CONF_EZI_EN_IN_BUFFER);
  TIMER_IR(t->base) = TIMER_IR_CR0INT << in->channel;
  t->routed[in->channel] = in;
  TIMER_CCR(t->base) |= (TIMER_CCR_CAP0RE | TIMER_CCR_CAP0I) << shift;
  irq_unlock(primask);
}

static void capture_unroute(const capture_input_t* in)
{
  timing_timer_t* t = in->timer;
  uint32_t primask;

  if(t->routed[in->channel] != in)
    return;

  primask = irq_lock();
  TIMER_CCR(t->base) &= ~(7u << (3 * in->channel));
  t->routed[in->channel] = 0;
  irq_unlock(primask);
}

/* The SOF wins the channel when it shares one with the PPS pin */
static void apply_routing(void)
{
  int shared = (pps->timer == sof_usb0.timer) && (pps->channel == sof_usb0.channel);

  if(sof_divider == 0)
    capture_unroute(&sof_usb0);
  if(!(shared && sof_divider))
    capture_route(pps);
  if(sof_divider)
    capture_route(&sof_usb0);
}

void timing_init(void)
{
  pps = LPC4370SDR_HW_MINI_PINS(lpc4370sdr_conf->conf_hw.hardware_type) ? &pps_mini : &pps_p2_2;
  /* the UART ISR (M0) stamps its bytes on the timer the PPS edges are captured on */
  stream->uart_timer_tc = (uint32_t)&TIMER_TC(pps->timer->base);
  /* RAM is not cleared at boot; the M0, which writes them, starts later */
  stream->uart_seq = 0;
  stream->uart_in = 0;
  stream->uart_tick = 0;

  armed = 0;
  sof_divider = 0;
  sof_frame_last = 0;
  sof_frame_base = 0;
  stream->pps_seq = 0;
  stream->pps_count = 0;
  stream->sof_seq = 0;
  stream->sof_count = 0;
  stream->sof_edges = 0;
  stream->sof_divider = 0;

  /* Both run from boot so a stream start can anchor them whatever is routed later */
  timer_start(&timer0);
  timer_start(&timer3);
  apply_routing();
}

static uint32_t gcd32(uint32_t a, uint32_t b)
{
  while(b)
  {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* Timer 0 is read before and after Timer 3: their midpoint stands for the moment Timer 3 was read.
 * The quickest of a few tries (an interrupt or bus stall widens the span) wins. What remains is the
 * difference in bus latency between the two timers, a tick or two. */
static void timing_measure_offset(void)
{
  uint32_t best_span = 0xFFFFFFFF;
  int32_t best = 0;
  uint32_t i, primask;

  for(i = 0; i < 16; i++)
  {
    uint32_t a, b, c, span;
    primask = irq_lock();
    a = TIMER_TC(TIMER0);
    b = TIMER_TC(TIMER3);
    c = TIMER_TC(TIMER0);
    irq_unlock(primask);
    span = c - a;
    if(span < best_span)
    {
      best_span = span;
      /* 32-bit difference: the timers started together at boot, far less than 2^31 ticks apart */
      best = (int32_t)(b - a - (span + 1) / 2);
    }
  }
  timing_timer3_minus_timer0 = best;
  timing_offset_span = best_span;
}

void timing_stream_start(uint32_t sample_rate_hz, uint32_t timer_clock_hz)
{
  uint32_t g = gcd32(sample_rate_hz, timer_clock_hz);
  timer_hz = timer_clock_hz;
  ratio_num = sample_rate_hz / g;
  ratio_den = timer_clock_hz / g;

  stream->pps_seq = (stream->pps_seq + 1) | 1;
  stream->pps_count = 0;
  stream->pps_index_lo = 0;
  stream->pps_index_hi = 0;
  stream->pps_fraction = 0;
  stream->pps_uart_in = 0;
  stream->pps_uart_quiet_us = 0;
  stream->pps_seq = stream->pps_seq + 1;

  stream->sof_seq = (stream->sof_seq + 1) | 1;
  stream->sof_count = 0;
  stream->sof_index_lo = 0;
  stream->sof_index_hi = 0;
  stream->sof_fraction = 0;
  stream->sof_frame = 0;
  stream->sof_seq = stream->sof_seq + 1;
  stream->sof_edges = 0;

  /* One anchor read, the other derived: reading both would take the second one some 20 to 30
     ticks after the first, an offset between PPS and SOF indices when they use different timers
     (the Mini) */
  timing_measure_offset();
#ifdef LPC4370SDR_TIMING_DEBUG
  {
    /* The anchors as the firmware used to read them: how far apart the two reads are */
    uint64_t a0 = timer_now64(&timer0);
    uint64_t a3 = timer_now64(&timer3);
    timing_debug_old_skew = (int32_t)(a3 - a0) - timing_timer3_minus_timer0;
  }
#endif
  timer3.anchor = timer_now64(&timer3);
  timer0.anchor = timer3.anchor - (uint64_t)(int64_t)timing_timer3_minus_timer0;
  armed = 1;
}

void timing_stream_stop(void)
{
  armed = 0;
}

void timing_set_sof_divider(uint32_t divider)
{
  sof_divider = divider;
  stream->sof_divider = divider;
  apply_routing();
}

static void ticks_to_sample(uint64_t ticks, uint64_t* index, uint32_t* fraction)
{
  uint64_t q = ticks / ratio_den;
  uint32_t r = (uint32_t)(ticks % ratio_den);
  uint64_t rn = (uint64_t)r * ratio_num; /* passes 2^32 for some pairs of rates */
  *index = q * ratio_num + rn / ratio_den;
  *fraction = (uint32_t)(((rn % ratio_den) << 32) / ratio_den);
}

/*
 * The auxiliary UART's bytes at the edge, for a GNSS module's announcement of the pulse: how many
 * had arrived, and how long the line had been quiet. The UART ISR (M0) counts each batch it takes
 * from the FIFO and then reads the timer, so a byte arrived no later than its stamp. This ISR runs
 * after the edge: a stamp past the edge means a byte came in between, or right before it - the line
 * was busy at the edge, and the count may hold bytes from after it (quiet 0).
 */
static void pps_uart(uint64_t count, uint32_t* in, uint32_t* quiet_us)
{
  uint32_t s1, s2, tick, tries = 16;
  int32_t ticks;

  do
  {
    s1 = stream->uart_seq;
    *in = stream->uart_in;
    tick = stream->uart_tick;
    s2 = stream->uart_seq;
  } while(((s1 != s2) || (s1 & 1)) && --tries);

  /* the timer wraps every 2^32 ticks: a line quiet for over 2^31 counts as busy, never quiet */
  ticks = (int32_t)((uint32_t)count - tick);
  if(*in == 0 && tries)
  {
    *quiet_us = 0xFFFF; /* nothing received since boot */
  } else if(ticks <= 0 || tries == 0 || timer_hz == 0)
  {
    *quiet_us = 0;
  } else
  {
    const uint64_t us = (uint64_t)ticks * 1000000u / timer_hz;
    *quiet_us = us > 0xFFFF ? 0xFFFF : (us ? (uint32_t)us : 1); /* a quiet line reads at least 1 */
  }
}

static void pps_event(uint64_t count)
{
  uint64_t index;
  uint32_t fraction, uart_in, uart_quiet;

  /* An edge captured before the stream start, serviced once its interrupts were enabled again */
  if(!armed || (int64_t)(count - pps->timer->anchor) < 0)
    return;
  ticks_to_sample(count - pps->timer->anchor, &index, &fraction);
  pps_uart(count, &uart_in, &uart_quiet);

  stream->pps_seq = (stream->pps_seq + 1) | 1;
  stream->pps_index_lo = (uint32_t)index;
  stream->pps_index_hi = (uint32_t)(index >> 32);
  stream->pps_fraction = fraction;
  stream->pps_uart_in = uart_in;
  stream->pps_uart_quiet_us = uart_quiet;
  stream->pps_count++;
  stream->pps_seq = stream->pps_seq + 1;
}

static void sof_event(uint64_t count, uint32_t frindex)
{
  uint64_t index;
  uint32_t fraction;
  uint32_t frame = (frindex >> 3) & USB_FRAME_MASK;

  if(frame < sof_frame_last)
    sof_frame_base += 1 << USB_FRAME_BITS;
  sof_frame_last = frame;
  frame += sof_frame_base;

  if(!armed)
    return;
  stream->sof_edges++;
  /* Only the first SOF of a frame (microframe 0): the one a full-speed bus sees */
  if((frindex & USB_UFRAME_MASK) != 0)
    return;
  if(sof_divider == 0 || (frame % sof_divider) != 0)
    return;
  if((int64_t)(count - sof_usb0.timer->anchor) < 0)
    return;
  ticks_to_sample(count - sof_usb0.timer->anchor, &index, &fraction);

  stream->sof_seq = (stream->sof_seq + 1) | 1;
  stream->sof_index_lo = (uint32_t)index;
  stream->sof_index_hi = (uint32_t)(index >> 32);
  stream->sof_fraction = fraction;
  stream->sof_frame = frame;
  stream->sof_count++;
  stream->sof_seq = stream->sof_seq + 1;
}

static void timer_isr(timing_timer_t* t)
{
  uint32_t ir = TIMER_IR(t->base);
  uint32_t ch;

  if(ir & TIMER_IR_MR0INT)
  {
    t->wraps++;
    TIMER_IR(t->base) = TIMER_IR_MR0INT;
  }

  for(ch = 0; ch < 4; ch++)
  {
    uint32_t bit = TIMER_IR_CR0INT << ch;
    const capture_input_t* in;
    uint32_t cap, frindex = 0, h;
    uint32_t passes = 0;

    if(!(ir & bit))
      continue;
    in = t->routed[ch];

    /* Take the newest event: a later one may land while this is read. Bounded, so a
     * bouncing input cannot hold the ISR */
    do
    {
      TIMER_IR(t->base) = bit;
      cap = MMIO32(t->base + 0x02C + 4 * ch);
      if(in && in->kind == TIMING_SOF)
      {
        /* give the USB controller time to load the frame index of this SOF */
        while((TIMER_TC(t->base) - cap) < 64);
        frindex = USB0_FRINDEX_D;
      }
      if(++passes >= 8)
        break;
    } while(TIMER_IR(t->base) & bit);

    if(!in)
      continue;

    h = t->wraps;
    if((ir & TIMER_IR_MR0INT) && cap >= 0x80000000)
      h--; /* captured before the wrap counted above */
    else if(!(ir & TIMER_IR_MR0INT) && (TIMER_IR(t->base) & TIMER_IR_MR0INT) && cap < 0x80000000)
      h++; /* captured after a wrap not counted yet */

    if(in->kind == TIMING_SOF)
      sof_event(((uint64_t)h << 32) | cap, frindex);
    else
      pps_event(((uint64_t)h << 32) | cap);
  }
}

void timer0_isr(void)
{
  timer_isr(&timer0);
}

void timer3_isr(void)
{
  timer_isr(&timer3);
}
