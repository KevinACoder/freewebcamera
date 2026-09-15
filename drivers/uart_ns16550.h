/*
 * @file   uart_ns16550.h
 * @brief  Board coordinates for the console UART (DW-APB 16550-compatible).
 *
 * The interface is CMSIS ARM_DRIVER_USART (include/Driver_USART.h); this
 * header only carries the board constants the implementation needs. Nothing
 * above the driver should include it - take the handle instead:
 *
 *     extern ARM_DRIVER_USART Driver_USART_Console;
 *
 * Register access is 32-bit with a byte-offset stride of 4. This is a
 * DesignWare APB UART on a 32-bit bus, not a byte-wide 16550: byte accesses
 * read and write garbage here.
 */

#ifndef FREEWEBCAMERA_UART_NS16550_H
#define FREEWEBCAMERA_UART_NS16550_H

/* UART2 is the console on this board. */
#define UART_CONSOLE_BASE	0xfe660000UL
#define UART_CONSOLE_CLOCK_HZ	24000000UL
#define UART_CONSOLE_BAUD	115200UL

#endif /* FREEWEBCAMERA_UART_NS16550_H */
