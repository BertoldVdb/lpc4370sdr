/* Serial pass-through for a GNSS module (or anything serial) */
#ifndef __LPC4370SDR_UART_H__
#define __LPC4370SDR_UART_H__

#include <stdint.h>

#define LPC4370SDR_UART_DEFAULT_BAUD (9600)
#define LPC4370SDR_UART_BAUD_MIN (300)
#define LPC4370SDR_UART_BAUD_MAX (1000000)

int lpc4370sdr_uart_available(void);
void lpc4370sdr_uart_init(uint32_t baud);
void lpc4370sdr_uart_flush_rx(void);
uint32_t lpc4370sdr_uart_read(uint8_t* dst, uint32_t max);  /* main loop only */
/* Bytes taken out of the receive ring since boot, read or dropped: with the stream state's uart_in
   (bytes stored) it numbers every byte, so a PPS edge can be placed among them */
uint32_t lpc4370sdr_uart_taken(void);
uint32_t lpc4370sdr_uart_write(const uint8_t* src, uint32_t len); /* all or nothing: returns len or 0 */
uint32_t lpc4370sdr_uart_baud(void);

#endif
