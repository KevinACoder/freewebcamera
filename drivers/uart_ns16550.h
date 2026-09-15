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

/* The GIC INTID of this UART's interrupt.
 *
 * Established from three independent device trees that agree (uart2's node
 * carries GIC_SPI 118; and uart1..uart9 map to SPI 117..125, so the sequence is
 * self-consistent), and confirmed equal in the vendor SoC header. SPI N is
 * INTID N+32, hence 150.
 *
 * NOTE: the lab's FreeBSD logs report `irq 66` for this same base address. That
 * discrepancy is unresolved. It is recorded here rather than silently ignored,
 * because getting the INTID wrong produces a console that simply never
 * receives - the exact silent failure this file spends so long warning about -
 * and the first thing to try if RX does not work is the other number.
 */
#define UART_CONSOLE_INTID	150U

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
