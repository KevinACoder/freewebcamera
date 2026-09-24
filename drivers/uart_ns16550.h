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

/* The calibration lives with the board's other coordinates
 * (port/board/<board>/board_conf.h), reached through board.h; the names
 * below are this driver's historical aliases. */
#include "board.h"

#define UART_CONSOLE_BASE	BOARD_UART_BASE
#define UART_CONSOLE_CLOCK_HZ	BOARD_UART_CLOCK_HZ
#define UART_CONSOLE_BAUD	BOARD_UART_BAUD
#define UART_CONSOLE_INTID	BOARD_CONSOLE_INTID

/* No functions are declared here. Everything this driver offers is reachable
 * through the CMSIS ARM_DRIVER_USART vtable, which is the point of implementing
 * that interface:
 *
 *   Send / Receive / Transfer      data movement
 *   Control(ARM_USART_CONTROL_RX)  start receiving (installs the console
 *                                  interrupt and arms the first byte)
 *   Control(ARM_USART_CONTROL_TX)  transmit gating (already-on line)
 *
 * Reception is reported through the ARM_USART_SignalEvent_t given to
 * Initialize(), and starting it is deliberately separate from
 * PowerControl(ARM_POWER_FULL): the boot path powers the console up before the
 * scheduler exists, and an interrupt enabled then would have no task to wake.
 *
 * There is intentionally NO driver-specific entry point to include, so nothing
 * above the driver ever needs this header - only the handle and the standard
 * header. */

#endif /* FREEWEBCAMERA_UART_NS16550_H */
