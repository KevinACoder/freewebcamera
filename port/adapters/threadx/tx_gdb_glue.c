/*
 * @file   tx_gdb_glue.c
 * @brief  ThreadX-adapter wiring for the serial GDB stub (D56).
 *
 * The carrier's console discipline: transmit is polled early-print, receive
 * has no owner but the stub. Nothing ever arms the UART's RX interrupt
 * (IER stays 0), so the stub's polled port is uncontested and the whole
 * storm-defense machinery of the interrupt-driven console stays out of the
 * debug model. The one asynchronous entry is the tick: the tick dispatch
 * drains the RX FIFO, and a Ctrl-C byte there becomes a stub break-in -
 * from IRQ context, so it works even when every thread is wedged.
 */

#include <stdint.h>

#include "board.h"
#include "irq_ctrl.h"

#include "gdb/gdb.h"
#include "gdb/gdb_arch.h"
#include "tx_gdb_glue.h"

/* Raw 16550, 32-bit stride - same map as drivers/uart_ns16550.c. */
#define GDB_REG_RBR	0x00
#define GDB_REG_THR	0x00
#define GDB_REG_LSR	0x14

#define LSR_DR		0x01U
#define LSR_THRE	0x20U

static inline unsigned int dbg_rd(unsigned int off)
{
	return *(volatile unsigned int *)(BOARD_UART_BASE + off);
}

static inline void dbg_wr(unsigned int off, unsigned int v)
{
	*(volatile unsigned int *)(BOARD_UART_BASE + off) = v;
}

static void dbg_putc(char c)
{
	while ((dbg_rd(GDB_REG_LSR) & LSR_THRE) == 0U) {
	}
	dbg_wr(GDB_REG_THR, (unsigned int)(unsigned char)c);
}

static unsigned long dbg_cntvct(void)
{
	unsigned long v;

	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static int dbg_getc(void)
{
	unsigned long deadline = dbg_cntvct() + 5UL * 24000UL; /* 5 s */

	/* Block until a byte is there. The bridge trickles bytes in with
	 * millisecond gaps; returning -1 on an empty FIFO would NAK a
	 * frame mid-read and tear the session down (board-proven). */
	while ((dbg_rd(GDB_REG_LSR) & LSR_DR) == 0U) {
		if (dbg_cntvct() > deadline) {
			return -1;
		}
	}
	return (int)(dbg_rd(GDB_REG_RBR) & 0xffU);
}

static void dbg_intr_ctrl(int on)
{
	/* Defensive only: this image never enables the console INTID, so the
	 * session's mask/unmask around the trap loop is a no-op - unless a
	 * diagnostic re-arms RX, in which case this keeps the session from
	 * racing the driver's handler for the FIFO. */
	if (on) {
		IRQ_Disable((IRQn_ID_t)BOARD_CONSOLE_INTID);
	} else {
		IRQ_Enable((IRQn_ID_t)BOARD_CONSOLE_INTID);
	}
}

static const struct gdb_dbgport dbgport = {
	.putc = dbg_putc,
	.getc = dbg_getc,
	.intr_ctrl = dbg_intr_ctrl,
};

void tx_gdb_init(void)
{
	gdb_dbgport_register(&dbgport);
	gdb_arch_setup();
}

/* Strong definition of board_early.c's weak gate ask (declaration in
 * port/board/board.h): inside a session the locked console sinks drop
 * their output so task logs cannot land in the middle of the RSP
 * stream. */
int board_console_muted(void)
{
	return gdb_session_active;
}

void gdb_break(void)
{
	__asm__ __volatile__("dsb sy");
	__asm__ __volatile__("brk %0" :: "i"(GDB_BRK_IMM));
}

/* The polled Ctrl-C watcher, called from the tick dispatch (IRQ context).
 * The session flag is checked BEFORE any read: inside a session the stub
 * owns the FIFO, and a poll here would eat its bytes. Outside a session
 * every byte is consumed - there is no console reader to hand them to -
 * and 0x03 breaks into the stub. The BRK raises a sync exception on the
 * spot, so the whole session runs inside this IRQ with the interrupted
 * world frozen; continue ERETs back and the tick handler carries on. */
void tx_gdb_tick_poll(void)
{
	if (gdb_session_active) {
		return;
	}
	while ((dbg_rd(GDB_REG_LSR) & LSR_DR) != 0U) {
		if ((dbg_rd(GDB_REG_RBR) & 0xffU) == 0x03U) {
			board_early_print_raw("gdb: Ctrl-C - entering stub\n");
			gdb_break();
			break;
		}
	}
}
