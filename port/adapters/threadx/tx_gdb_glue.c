/*
 * @file   tx_gdb_glue.c
 * @brief  ThreadX-adapter wiring for the serial GDB stub (D56).
 *
 * Trunk-baseline console discipline: the cherrysh shell and the stub share
 * the one UART, arbitrated by an explicit owner.
 *
 *   - SHELL owns RX by default. RX is interrupt-driven (cherrysh adapter,
 *     FIFO -> ring -> shell task); the stub never touches the FIFO. The
 *     shell's RX path snoopes every 0x03 and hands it to
 *     board_console_break_hook(), whose strong definition here consumes the
 *     byte and raises the BRK that enters a session.
 *   - STUB owns RX for the length of a session. gdb_arch raises
 *     gdb_dbgport_intr_ctrl(1) at trap entry; that claims the line (GIC-level
 *     mask of the console INTID - the CMSIS reception stays armed but its ISR
 *     cannot run, so the stub's polled port has the FIFO to itself) and on
 *     session exit intr_ctrl(0) hands the line back, whereupon the armed
 *     reception completes on the next byte and the shell resumes untouched.
 *
 * The tick watcher drains the RX FIFO only while the stub owns the line
 * (post-claim, pre-attach and between stops): that is where a Ctrl-C typed
 * through the bridge breaks into a running target. While the shell owns the
 * line the watcher does nothing - a scan here would race the shell's ISR for
 * the same FIFO.
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

/* Who drains the console RX FIFO (see the file comment). */
#define GDB_RX_OWNER_SHELL	0
#define GDB_RX_OWNER_STUB	1

static int gdb_rx_owner = GDB_RX_OWNER_SHELL;

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
	/* Session boundary = ownership handover. Mask at the GIC only: the
	 * CMSIS console driver keeps its reception armed, so on release the
	 * next byte completes it and the shell's event path picks up where
	 * it left off. Bytes that arrived during the session were consumed
	 * by the stub; that is the session's traffic, not the shell's. */
	if (on) {
		gdb_rx_owner = GDB_RX_OWNER_STUB;
		IRQ_Disable((IRQn_ID_t)BOARD_CONSOLE_INTID);
	} else {
		IRQ_Enable((IRQn_ID_t)BOARD_CONSOLE_INTID);
		gdb_rx_owner = GDB_RX_OWNER_SHELL;
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

/* Strong definition of board_early.c's weak break hook (declaration in
 * port/board/board.h): the shell's RX path saw 0x03. Consume the byte and
 * raise the BRK on the spot - the trap freezes the interrupted RX context
 * exactly as the tick-poll break does, and on session exit the ERET lands
 * back in the shell's event callback, which keeps draining its chunk. */
int board_console_break_hook(void)
{
	gdb_break();
	return 1;
}

void gdb_break(void)
{
	__asm__ __volatile__("dsb sy");
	__asm__ __volatile__("brk %0" :: "i"(GDB_BRK_IMM));
}

/* The polled Ctrl-C watcher, called from the tick dispatch (IRQ context).
 * Runs only while the stub owns the line and no session is live yet: the
 * session flag is checked before any read (inside a session the stub's own
 * getc owns the FIFO), and the owner check keeps this from racing the
 * shell's RX ISR while the shell is the console. A 0x03 breaks into the
 * stub: the BRK raises a sync exception on the spot, the whole session runs
 * inside this IRQ with the interrupted world frozen, and continue ERETs
 * back so the tick handler carries on. */
void tx_gdb_tick_poll(void)
{
	if (gdb_session_active || gdb_rx_owner != GDB_RX_OWNER_STUB) {
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
