#ifndef AD9518_PORT_STM32_H
#define AD9518_PORT_STM32_H

#include "ad9518.h"
#include "ad9613.h"

/*
 * Initialise the GPIO pins used for the AD9518 serial port and fill in the
 * device structure. Call once after MX_GPIO_Init() and MX_USART1_UART_Init().
 *
 * The SPI1 peripheral is deliberately NOT used - the port bit-bangs both
 * directions so that half-duplex turnaround is fully under software control.
 * You may leave MX_SPI1_Init() in main.c; it simply goes unused.
 */
void ad9518_port_init(ad9518_dev_t *d);

/* Adjust the bit-bang half period. Larger is slower and more tolerant of
 * series resistors, long traces and slow turnaround. Default is set for a
 * conservative bring-up speed. */
void ad9518_port_set_bit_delay(uint32_t loops);

/* Toggle CS, then SCLK, then SDIO one at a time, slowly, announcing each on
 * the UART. Probe at the AD9518's pins to confirm continuity. Never returns. */
void ad9518_port_pin_walk(void);

/* Pulse the AD9518's active-low hardware RESET and release it. Called
 * automatically by ad9518_port_init(); exposed for manual retries. */
void ad9518_port_hw_reset(void);

/* Fill in the AD9613 device struct. Call after ad9518_port_init(), which
 * configures the shared SCLK/SDIO pins. */
void ad9613_port_init(ad9613_dev_t *d);

#endif /* AD9518_PORT_STM32_H */