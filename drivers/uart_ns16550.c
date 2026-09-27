/*
 * @file   uart_ns16550.c
 * @brief  UART2 as a CMSIS ARM_DRIVER_USART instance (DW-APB, polling).
 *
 * The RK3568 console UART is a DesignWare APB UART behind a 32-bit bus.
 * Register access is therefore 32-bit with a byte-offset stride of 4 - it is
 * NOT a byte-wide 16550, and byte accesses read/write garbage here.
 *
 * Divisor: the UART clock is 24 MHz and 24e6 / (16 * 115200) = 13, so DLL
 * holds 13. The latch is selected through LCR.DLAB and both halves must be
 * written with that bit set, then LCR restored.
 *
 * Polling, not interrupts, is deliberate at M0. The console must work from
 * the first instructions of the boot path (before the GIC exists) and must
 * never be the thing that starves other tasks. The shell does not spin on
 * this driver: it reads through an interrupt-fed ring buffer in
 * port/adapters/cherrysh, which is what keeps a blocked shell from parking
 * the CPU. Send here is synchronous and blocking, which is honest about what
 * a polled UART is.
 *
 * CMSIS contract notes for this implementation:
 *  - Send is blocking: it returns ARM_DRIVER_OK only after the last byte has
 *    been handed to the FIFO, so GetTxCount equals the requested count on
 *    return and no ARM_USART_EVENT_SEND_COMPLETE is raised. Blocking in a
 *    polled driver is preferable to pretending to be asynchronous.
 *  - Receive is unsupported (-ARM_DRIVER_ERROR_UNSUPPORTED): there is no RX
 *    interrupt path at M0, and the shell owns RX via its own ring buffer.
 *    Callers wanting a byte should use uart_early_try_getc or the shell path.
 *  - Control accepts only the subset actually implemented; anything else
 *    returns -ARM_DRIVER_ERROR_UNSUPPORTED rather than silently succeeding.
 */

#include <stdint.h>
#include <stdio.h>

#include "Driver_USART.h"
#include "board.h"
#include "irq_ctrl.h"
#include "regs.h"
#include "uart_ns16550.h"

/* Byte offsets; each is shifted by 2 when accessed through the 32-bit view. */
#define REG_RBR		0x00	/* receive buffer (read) */
#define REG_THR		0x00	/* transmit holding (write) */
#define REG_DLL		0x00	/* divisor latch low, DLAB=1 */
#define REG_IER		0x04	/* interrupt enable */
#define REG_DLH		0x04	/* divisor latch high, DLAB=1 */
#define REG_IIR		0x08	/* interrupt identification (read) */
#define REG_FCR		0x08	/* FIFO control (write) */
#define REG_LCR		0x0c	/* line control */
#define REG_MCR		0x10	/* modem control */
#define REG_LSR		0x14	/* line status */
#define REG_MSR		0x18	/* modem status */
#define REG_SCR		0x1c	/* scratch */

#define LSR_DATA_READY	(1u << 0)
#define LSR_OVERRUN	(1u << 1)
#define LSR_PARITY_ERR	(1u << 2)
#define LSR_FRAMING_ERR	(1u << 3)
#define LSR_BREAK	(1u << 4)
#define LSR_THR_EMPTY	(1u << 5)
#define LSR_RX_ERRORS	(LSR_OVERRUN | LSR_PARITY_ERR | LSR_FRAMING_ERR | LSR_BREAK)

/* Interrupt enable bits (IER) and identification codes (IIR). */
#define IER_RX_AVAILABLE	(1u << 0)
#define IIR_ID_MASK		0x0fu
#define IIR_ID_RX_AVAILABLE	0x04u
#define IIR_ID_RX_TIMEOUT	0x0cu
#define IIR_FIFO_ENABLED	(0xc0u)

#define LCR_8N1		0x03	/* WLS = 8 data bits, no parity, 1 stop bit */
#define LCR_DLAB	(1u << 7)

#define FCR_ENABLE	(1u << 0)
#define FCR_CLEAR_RX	(1u << 1)
#define FCR_CLEAR_TX	(1u << 2)
#define FCR_TRIGGER_14	(3u << 6)

#define MSR_CTS		(1u << 4)
#define MSR_DSR		(1u << 5)
#define MSR_DCD		(1u << 7)
#define MSR_RI		(1u << 8)

#define USART_DRIVER_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)
#define USART_API_VERSION    ARM_DRIVER_VERSION_MAJOR_MINOR(2, 3)

/* --- register access ------------------------------------------------------ */

/* The constants above are already BYTE offsets, not register indices: the part
 * has reg-shift=2 / reg-io-width=4, so 16550 register index N sits at byte
 * offset 4*N (LSR, index 5, is at 0x14). No further shift belongs here.
 *
 * An earlier revision shifted again (base + (offset << 2)), which pointed
 * every access 4x too high. THR is at offset 0 - the one value where shifting
 * changes nothing - and that is the *only* reason the console appeared to
 * work: it could transmit, because the part had already been programmed to
 * 115200 8N1 by U-Boot and we never noticed that our own LCR/IER/MCR writes
 * were landing on the wrong registers.
 *
 * The visible symptom was a console that printed and then stopped dead. The
 * UART FIFO is 64 bytes deep, so short bursts (a banner, a dozen characters)
 * went out without ever needing to poll; the first write that had to wait for
 * room read LSR from the wrong address, never saw THR_EMPTY, and spun forever.
 * It looked exactly like a hang in whatever code printed the *next* message. */
static inline uint32_t reg_read(uint32_t offset)
{
	return reg_rd32(UART_CONSOLE_BASE + offset);
}

static inline void reg_write(uint32_t offset, uint32_t value)
{
	reg_wr32(UART_CONSOLE_BASE + offset, value);
}

/* --- early path state ----------------------------------------------------- */

static bool early_ready;

/* Program the UART for polled 8N1. Shared by uart_early_init and the driver's
 * PowerControl, so both paths cannot drift apart. Returns false when the
 * scratch register does not read back, which catches a wrong base address or
 * a gated clock - otherwise a dead UART looks like a working one that prints
 * nothing. */
/* Shadow of what program_uart last wrote. The driver is the only writer of
 * these registers, so a match means the part is already programmed exactly
 * as requested - and the write sequence can be skipped entirely.
 *
 * WHY SKIP INSTEAD OF REWRITE (board-proven 2026-09-21, NOTICK probe
 * round): the divisor latch write itself starts the console storm. With
 * DLAB=1, offset 0 aliases IER onto DLL, and the interrupt evaluator sees
 * DLL's bits as interrupt enables: DLL=13 (0x0d) exposes RX|MS enable at
 * the exact moment the MSR already holds change-of-state bits from this
 * same sequence's MCR write - the line asserts, the GIC latches INTID 150
 * (pending latches from the line even while the INTID is disabled), and
 * the 16 kHz spurious storm follows. The first invocation survives only
 * because it enters the window with U-Boot's DLL=0 and a clean MSR. A
 * CMSIS Initialize->PowerControl(FULL) pair therefore rewrites identical
 * values through the one window that must not be reopened; comparing
 * against the shadow is what honours PowerControl's contract without it. */
static bool uart_prog_valid;
static uint8_t uart_prog_ier;
static uint8_t uart_prog_lcr;
static uint8_t uart_prog_dll;
static uint8_t uart_prog_dlh;
static uint8_t uart_prog_fcr;
static uint8_t uart_prog_mcr;

static bool program_uart(void)
{
	uint32_t divisor = UART_CONSOLE_CLOCK_HZ / (16u * UART_CONSOLE_BAUD);
	const uint8_t ier = 0x00u;
	const uint8_t lcr_dlab = LCR_DLAB;
	const uint8_t lcr_8n1 = LCR_8N1;
	const uint8_t dll = divisor & 0xffu;
	const uint8_t dlh = (divisor >> 8) & 0xffu;
	const uint8_t fcr = FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX |
			    FCR_TRIGGER_14;
	const uint8_t mcr = 0x03u;	/* DTR and RTS, what the console expects */

	if (uart_prog_valid && uart_prog_ier == ier &&
	    uart_prog_lcr == lcr_8n1 && uart_prog_dll == dll &&
	    uart_prog_dlh == dlh && uart_prog_fcr == fcr &&
	    uart_prog_mcr == mcr) {
		/* Already programmed to exactly these values; reopening the
		 * divisor-latch window would re-arm the line-assertion bug
		 * documented above for zero gain. */
		early_ready = true;
		return true;
	}

	/* Interrupts stay off: this console is polled, and IER=0 guarantees a
	 * stray interrupt cannot fire before the GIC is programmed. */
	reg_write(REG_IER, ier);

	reg_write(REG_LCR, lcr_dlab);
	reg_write(REG_DLL, dll);
	reg_write(REG_DLH, dlh);
	reg_write(REG_LCR, lcr_8n1);

	reg_write(REG_FCR, fcr);

	reg_write(REG_MCR, mcr);

	uart_prog_ier = ier;
	uart_prog_lcr = lcr_8n1;
	uart_prog_dll = dll;
	uart_prog_dlh = dlh;
	uart_prog_fcr = fcr;
	uart_prog_mcr = mcr;
	uart_prog_valid = true;

	/* No self-test here. An earlier revision wrote a pattern to the
	 * scratch register and refused to transmit if it did not read back -
	 * but the scratch register is optional in 16550-compatible parts and
	 * is not implemented on this DW-APB UART, so a perfectly working
	 * console declared itself dead and stayed silent. There is no cheap
	 * way to prove a UART works other than transmitting. */

	early_ready = true;
	return true;
}

void uart_early_init(void)
{
	(void)program_uart();
}

/* Wait for transmit room.
 *
 * Note this is an unbounded spin, deliberately: on this part THR_EMPTY behaves
 * as expected, and a bounded version was actively harmful - a timeout that
 * merely *writes anyway* still costs a full timeout per byte, which on
 * uncached Device reads is slow enough that the boot log appeared to stop
 * mid-message. The original spin has no such cost because it exits as soon as
 * the bit is set, which is the normal case.
 *
 * The earlier "silent hang" that motivated a bound was misdiagnosed: the real
 * cause was a bad GIC register offset elsewhere in bring-up (see
 * port/aarch64/gicv3.c), not a stuck transmit-ready bit. */
static void polled_putc(char c)
{
	if (!early_ready) {
		return;
	}

	if (c == '\n') {
		while ((reg_read(REG_LSR) & LSR_THR_EMPTY) == 0u) {
		}
		reg_write(REG_THR, '\r');
	}

	while ((reg_read(REG_LSR) & LSR_THR_EMPTY) == 0u) {
	}
	reg_write(REG_THR, (uint32_t)(uint8_t)c);
}

/* Only THR_EMPTY (bit 5) is polled here. Whether the transmitter has fully
 * drained (TEMT, bit 6) is deliberately NOT used: an attempt to drain on it
 * never succeeded on this part, which latched a permanent "stalled" state and
 * silenced the console from the first message onward - indistinguishable from
 * the board resetting. */

void uart_early_putc(char c)
{
	polled_putc(c);
}

void uart_early_puts(const char *s)
{
	if (s == 0) {
		return;
	}
	while (*s != '\0') {
		polled_putc(*s++);
	}
}

void uart_early_put_u32(uint32_t value)
{
	char buf[11];
	int i = (int)sizeof(buf) - 1;

	buf[i] = '\0';
	do {
		buf[--i] = (char)('0' + (char)(value % 10u));
		value /= 10u;
	} while (value != 0u && i > 0);

	uart_early_puts(&buf[i]);
}

void uart_early_put_u64(uint64_t value)
{
	char buf[21];
	int i = (int)sizeof(buf) - 1;

	buf[i] = '\0';
	do {
		buf[--i] = (char)('0' + (char)(value % 10u));
		value /= 10u;
	} while (value != 0u && i > 0);

	uart_early_puts(&buf[i]);
}

void uart_early_put_hex32(uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	char buf[9];
	int i;

	for (i = 7; i >= 0; i--) {
		buf[i] = digits[value & 0xfu];
		value >>= 4;
	}
	buf[8] = '\0';
	uart_early_puts(buf);
}

bool uart_early_try_getc(char *out)
{
	if (!early_ready || out == 0) {
		return false;
	}
	if ((reg_read(REG_LSR) & LSR_DATA_READY) == 0u) {
		return false;
	}
	*out = (char)(reg_read(REG_RBR) & 0xffu);
	return true;
}

void uart_early_panic(const char *message)
{
	/* Re-program unconditionally: a fault may fire before any init, and a
	 * fault handler that cannot speak is indistinguishable from a dead
	 * board. */
	(void)program_uart();
	uart_early_puts((char *)(uintptr_t)message);
	board_early_print_raw("uart: parked; power-cycle to recover\n");
}

/* --- CMSIS ARM_DRIVER_USART ----------------------------------------------- */

static ARM_USART_SignalEvent_t usart_callback;
static volatile uint32_t usart_tx_count;
static volatile ARM_POWER_STATE usart_power = ARM_POWER_OFF;

static ARM_DRIVER_VERSION usart_get_version(void)
{
	ARM_DRIVER_VERSION v;

	v.api = USART_API_VERSION;
	v.drv = USART_DRIVER_VERSION;
	return v;
}

static ARM_USART_CAPABILITIES usart_get_capabilities(void)
{
	ARM_USART_CAPABILITIES caps;

	/* Only asynchronous mode, and none of the modem/flow-control lines are
	 * wired to anything usable on this board. Claiming them would be worse
	 * than reporting zero. */
	caps.asynchronous = 1;
	caps.synchronous_master = 0;
	caps.synchronous_slave = 0;
	caps.single_wire = 0;
	caps.irda = 0;
	caps.smart_card = 0;
	caps.smart_card_clock = 0;
	caps.flow_control_rts = 0;
	caps.flow_control_cts = 0;
	caps.event_tx_complete = 0;
	caps.event_rx_timeout = 0;
	caps.rts = 1;	/* pins are driven, they are just not usable for flow control */
	caps.cts = 1;
	caps.dtr = 1;
	caps.dsr = 1;
	caps.dcd = 1;
	caps.ri = 1;
	caps.event_cts = 0;
	caps.event_dsr = 0;
	caps.event_dcd = 0;
	caps.event_ri = 0;
	caps.reserved = 0;

	return caps;
}

/* The console interrupt is registered here rather than by an upper layer.
 *
 * The alternative - having the shell or the app call IRQ_SetHandler with this
 * driver's ISR - would require every consumer of the console to know the
 * driver's internals, and would put the driver's interrupt priority in someone
 * else's hands. The driver knows both; nothing else needs to.
 *
 * Priority is the API-call level because the event path this raises ends up
 * calling a FromISR RTOS API in the shell. The port asserts that any interrupt
 * calling one runs at or below configMAX_API_CALL_INTERRUPT_PRIORITY. */
static int32_t usart_initialize(ARM_USART_SignalEvent_t cb_event)
{
	usart_callback = cb_event;
	usart_tx_count = 0;
	usart_power = ARM_POWER_OFF;
	return ARM_DRIVER_OK;
}

static int32_t usart_uninitialize(void)
{
	usart_callback = 0;
	usart_power = ARM_POWER_OFF;
	return ARM_DRIVER_OK;
}

static int32_t usart_power_control(ARM_POWER_STATE state)
{
	switch (state) {
	case ARM_POWER_FULL:
		if (usart_power == ARM_POWER_FULL) {
			return ARM_DRIVER_OK;
		}
		if (!program_uart()) {
			return ARM_DRIVER_ERROR;
		}
		usart_power = ARM_POWER_FULL;
		return ARM_DRIVER_OK;

	case ARM_POWER_OFF:
	case ARM_POWER_LOW:
		/* Mask the source before dropping power, so a byte arriving
		 * afterwards cannot raise an interrupt at a driver that is no
		 * longer expecting one. */
		reg_write(REG_IER, 0x00u);
		usart_power = state;
		return ARM_DRIVER_OK;

	default:
		return ARM_DRIVER_ERROR_PARAMETER;
	}
}

static int32_t usart_send(const void *data, uint32_t num)
{
	const char *p = (const char *)data;
	uint32_t i;

	if (usart_power != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR;
	}
	if (data == 0 && num != 0u) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	for (i = 0; i < num; i++) {
		polled_putc(p[i]);
	}
	usart_tx_count += num;

	/* Blocking implementation: the bytes are already in the FIFO, so there
	 * is no completion event to signal. Returning the count via GetTxCount
	 * is what the CMSIS contract expects for synchronous behaviour. */
	return ARM_DRIVER_OK;
}

/* Non-interrupt-context state for the RX path: the destination the caller
 * handed to Receive() and how much of it is still to come. Written by the ISR,
 * read by the ISR and by Receive/Control, so it is volatile.
 *
 * The arm count equals the FIFO depth: the completion callback re-arms, and
 * re-arm drains whatever arrived in between - with a buffer as deep as the
 * FIFO that drain can never be forced to discard. */
#define SHELL_RX_ARM_COUNT	64U
static uint8_t rx_first_byte[SHELL_RX_ARM_COUNT];

/* The GIC INTID the console RX interrupt is currently bound to. Defaults to
 * the board constant; uart_console_irq_rebind() can move it at runtime, which
 * is how a wrong-INTID hypothesis gets tested without a reflash. */
static unsigned int console_intid = BOARD_CONSOLE_INTID;

static volatile uint8_t *rx_buf;
static volatile uint32_t rx_remaining;
static volatile uint32_t rx_completed;
static volatile uint8_t  rx_active;

/* The buffer the CMSIS client last armed with Receive(). A full re-arm
 * (boot, storm-defence kick, uartint rebind) resumes THAT buffer, not the
 * driver-internal one: the completion callback delivers whatever rx_buf
 * holds, and the client reads what it armed - they must agree even when
 * the re-arm was not client-initiated. */
static volatile uint8_t *rx_client_buf;
static uint32_t rx_client_len;

static void usart_rx_drain(void);
static int32_t usart_rx_start(void);
static uint32_t console_icfgr_word(uint32_t intid);
static uint32_t console_intid_bit(uint32_t word_offset);

static int32_t usart_receive(void *data, uint32_t num)
{
	/* Start an interrupt-driven reception of `num` bytes.
	 *
	 * This is the CMSIS shape: Receive() starts the transfer and the driver
	 * raises ARM_USART_EVENT_RECEIVE_COMPLETE when the count is satisfied.
	 * An earlier revision returned UNSUPPORTED because there was no RX
	 * interrupt path at M0 - which was honest then, and wrong now that the
	 * shell depends on reception.
	 *
	 * The shell needs a stream, not fixed-size blocks, so it re-arms with
	 * num=1 from the completion callback. That keeps per-byte latency low
	 * without inventing a "stream to callback" API CMSIS does not have. */
	if (usart_power != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR;
	}
	if (data == 0 || num == 0u) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (rx_active) {
		return ARM_DRIVER_ERROR_BUSY;
	}

	rx_buf = (volatile uint8_t *)data;
	rx_remaining = num;
	rx_completed = 0u;
	rx_active = 1u;
	rx_client_buf = (volatile uint8_t *)data;
	rx_client_len = num;

	/* Drain anything already sitting in the FIFO before unmasking, so a
	 * byte that arrived while RX was off is not stranded until the next
	 * one arrives. */
	usart_rx_drain();

	/* Unmask receive-data-available. Line-status (error) interrupts stay
	 * masked: the errors are visible in GetStatus(), and an error
	 * interrupt would need its own handling path for no benefit here. */
	reg_write(REG_IER, IER_RX_AVAILABLE);

	return ARM_DRIVER_OK;
}

/* Called from the RX interrupt (through the board's IRQ dispatch).
 *
 * The handler asks the part WHY it is asserting before touching anything:
 * IIR 0x4 is "FIFO reached the trigger level" and IIR 0xC is the character
 * timeout. Any other identification means this interrupt is not receive
 * data - reporting it, bounded, is the difference between "console RX is
 * dead for an unknown reason" and a one-line answer in the boot log.
 *
 * Classification, per the 2026-09-19 issue (symptom B) and the stamped-log
 * board sessions of 2026-09-20:
 *  - IIR bit 0 SET reads as "no interrupt pending" (the boot window's
 *    iir=0x07/lsr=0x00 signature). That is a level re-entry the part no
 *    longer backs - a spurious GIC delivery, NOT a line-status event. The
 *    old heuristic counted these as bad hits (and double-counted them),
 *    so three phantom re-entries in the first seconds of every boot
 *    permanently disabled RX and killed the shell on EVERY boot, 4-core
 *    and single-core alike. They are now counted separately, logged at a
 *    bounded rate, and ignored by the storm defence.
 *  - Any other unexpected identification (0x0 modem, 0x2 THR empty, 0x6
 *    line status) is a real wrong-source signal. After a bounded number,
 *    the defence arms RX OFF (IER=0, buffer disarmed) and leaves the GIC
 *    line enabled: IER=0 stops the assertion so the core cannot livelock,
 *    and a re-arm recovers without touching the GIC. The previous response
 *    here was a permanent IRQ_Disable() - "noisy line" became "deaf
 *    console" with no recovery path, which is what the 09-19 issue was.
 *    (An even earlier revision rebounded RX to the fallback INTID 66 -
 *    66 is DEAD on this board (board_conf.h) and the rebind silently
 *    deafened the shell while looking like progress. Console RX stays on
 *    BOARD_CONSOLE_INTID, full stop.)
 * It never moves the console to another INTID. */
#define CONSOLE_BADHIT_LIMIT	5U
#define CONSOLE_SPURIOUS_PRINT	3U
#define CONSOLE_LINE_LIMIT	1000U

/* Storm-defence state, file scope so the kick path can reset it. */
static volatile uint8_t console_rx_down;	/* armed off, awaiting re-arm */
static uint32_t console_spurious;		/* benign re-entries this boot */
static uint32_t console_line_hits;		/* latch re-set from the line */

/* Armed-off: stop the UART asserting (IER gates ALL 16550 interrupt
 * sources), keep the GIC line enabled, and say so once. */
static void console_rx_arm_off(uint32_t intid)
{
	char line[80];

	reg_write(REG_IER, 0x00u);
	rx_active = 0u;
	if (console_rx_down == 0u) {
		console_rx_down = 1u;
		(void)snprintf(line, sizeof(line),
			       "uart: console RX armed off on intid %u"
			       " (storm defence; re-arm pending)\n",
			       (unsigned)intid);
		board_early_print_raw(line);
	}
}

/* RX-path liveness counters that outlive the three-entry boot trace and
 * need no console input to observe: the avmon thread prints them.  An
 * input byte that never shows here never reached the part (host/serial
 * side); a byte counted here that the shell never echoes died in the
 * handoff. */
volatile unsigned uart_rx_isr_entries;
volatile unsigned uart_rx_bytes_seen;

void usart_rx_irq_handler(void)
{
	static uint32_t unexpected_reports;
	static uint8_t unexpected_printed;
	static uint8_t traced_entries;
	uint32_t guard = 0u;
	uint32_t intid = console_intid;
	uint32_t iir = reg_read(REG_IIR) & IIR_ID_MASK;

	uart_rx_isr_entries++;

	/* M0 bring-up trace: make ISR activity observable even when the
	 * reason turns out to be benign. Three entries, then silence.
	 * Stamped raw writer: this is ISR context and must not spin on the
	 * print lock. */
	if (traced_entries < 3U) {
		char line[64];

		traced_entries++;
		(void)snprintf(line, sizeof(line),
			       "uart: rx irq entry: intid=%u iir=%02x lsr=%02x\n",
			       (unsigned)intid, (unsigned)iir,
			       (unsigned)reg_read(REG_LSR));
		board_early_print_raw(line);
	}

	if ((iir & 0x01u) != 0u) {
		/* "No pending" per the part, yet the GIC delivered this
		 * INTID: the pending state is either a STALE LATCH (software
		 * visible, line idle) or the LINE is genuinely held (by
		 * something that is not this UART - the part just denied it).
		 * The two worlds are told apart in one operation: clear the
		 * latch, re-read. Stale latch -> this write ends the storm
		 * (recovery, not just defence). Line held -> ICPENDR re-latches
		 * from the line, the re-read proves it, and the flood guard
		 * below eventually falls back to IRQ_Disable so the machine
		 * stays usable. */
		uint32_t latch_before = IRQ_GetPending((IRQn_ID_t)intid);

		IRQ_ClearPending((IRQn_ID_t)intid);

		console_spurious++;
		if (console_spurious == 1u) {
			/* NOTE: ICC_CTLR_EL1 (S3_0_C12_C11_4, would tell
			 * EOImode for the stuck-ACTIVE question) is NOT
			 * readable here - the mrs traps as a synchronous
			 * exception (ESR=0x02000000, EC=0) on this partition,
			 * same family as the ICC_IAR0 EL3-reset lesson. */
			char line[96];

			(void)snprintf(line, sizeof(line),
				       "uart: first spurious: intid=%u"
				       " latch=%u ier=%02x lcr=%02x"
				       " icfgr=%08x\n",
				       (unsigned)intid,
				       (unsigned)latch_before,
				       (unsigned)reg_read(REG_IER),
				       (unsigned)reg_read(REG_LCR),
				       (unsigned)console_icfgr_word(intid));
			board_early_print_raw(line);
		} else if ((console_spurious & 0x7fu) == 0u) {
			/* GIC state dump, sampled: is the console line the
			 * ONLY thing latched, or is a PPI (tick INTID 27/30)
			 * pending/active behind this - i.e. the storm is a
			 * mis-delivered tick line that never deactivates?
			 * SGI/PPI pending/active live in the redistributor's
			 * SGI_base frame (RD_base + 0x10000); the storm runs
			 * on core 0, whose frame is BOARD_GICR_BASE. */
			char line[112];
			uint32_t gicr_pend = reg_rd32(
				BOARD_GICR_BASE + 0x10000u + 0x200u);
			uint32_t gicr_act = reg_rd32(
				BOARD_GICR_BASE + 0x10000u + 0x300u);
			uint32_t gicd_act150 =
				console_intid_bit(0x300u);

			(void)snprintf(line, sizeof(line),
				       "uart: storm n=%u l150=%u"
				       " gicr_pend=%08x gicr_act=%08x"
				       " gicd_act150=%u\n",
				       (unsigned)console_spurious,
				       (unsigned)IRQ_GetPending(
					       (IRQn_ID_t)intid),
				       (unsigned)gicr_pend,
				       (unsigned)gicr_act,
				       (unsigned)gicd_act150);
			board_early_print_raw(line);
		}

		if (IRQ_GetPending((IRQn_ID_t)intid) != 0u) {
			/* The latch re-set from the line: this is a real line
			 * storm the part denies owning. Fall back to disabling
			 * the INTID - the pre-20260920 behaviour - so the core
			 * cannot livelock; the kick path re-enables. */
			console_line_hits++;
			if (console_line_hits >= CONSOLE_LINE_LIMIT) {
				console_rx_arm_off(intid);
				(void)IRQ_Disable((IRQn_ID_t)intid);
				console_line_hits = 0u;
			}
		}
		return;
	}

	if (iir != IIR_ID_RX_AVAILABLE && iir != IIR_ID_RX_TIMEOUT) {
		if (unexpected_printed < 3U) {
			char line[64];

			unexpected_printed++;
			(void)snprintf(line, sizeof(line),
				       "uart: interrupt without RX data:"
				       " intid=%u iir=%02x lsr=%02x\n",
				       (unsigned)intid, (unsigned)iir,
				       (unsigned)reg_read(REG_LSR));
			board_early_print_raw(line);
		}

		/* Counted ONCE per entry (the old code double-counted the
		 * first three, making the effective limit 3). */
		if ((++unexpected_reports >= CONSOLE_BADHIT_LIMIT) &&
		    (console_rx_down == 0u)) {
			console_rx_arm_off(intid);
		}
		return;
	}

	/* A pending interrupt can re-enter with nobody armed (between a
	 * completion and the consumer's re-arm). Mask the source; the
	 * re-arm path drains what was left. */
	if (rx_active == 0u) {
		reg_write(REG_IER, 0x00u);
		return;
	}

	/* Store bytes until the FIFO is empty or the armed buffer is full,
	 * then complete the reception ONCE.
	 *
	 * Completing per byte would call the consumer mid-drain, and the
	 * consumer's re-arm path drains the FIFO re-entrantly - it would
	 * eat the bytes queued behind the one being delivered (a pasted
	 * line reached the shell as "p" through exactly that). Completing
	 * on FIFO-empty means the re-arm always finds an empty FIFO.
	 * Bytes are still not lost when the buffer fills first: the
	 * buffer is armed at the FIFO's depth, and the re-arm drain picks
	 * up whatever arrived during the callback. */
	guard = 0u;
	while ((reg_read(REG_LSR) & LSR_DATA_READY) != 0u) {
		uint8_t byte = (uint8_t)(reg_read(REG_RBR) & 0xffu);

		if (++guard > 64u) {
			/* The FIFO cannot hold more than this; a larger count
			 * here would mean the data-ready bit is stuck, and
			 * looping on it would wedge the interrupt. */
			break;
		}
		if (rx_active == 0u) {
			/* The consumer did not re-arm. The byte is dropped to
			 * keep the level line clear; delivery resumes from
			 * the next arm. */
			continue;
		}

		rx_buf[rx_completed] = byte;
		uart_rx_bytes_seen++;
		rx_completed++;
		rx_remaining--;

		if ((rx_remaining == 0u) ||
		    ((reg_read(REG_LSR) & LSR_DATA_READY) == 0u)) {
			/* Buffer full, or the line has gone idle with the
			 * FIFO drained: hand over what was received. */
			reg_write(REG_IER, 0x00u);
			rx_active = 0u;
			if (usart_callback != 0) {
				static uint32_t done_reports;

				/* UP bring-up probe: did the drain complete? */
				if (done_reports < 3U) {
					char line[48];

					done_reports++;
					(void)snprintf(line, sizeof(line),
						       "uart: rx done n=%u\n",
						       (unsigned)rx_completed);
					board_early_print_raw(line);
				}
				usart_callback(
					ARM_USART_EVENT_RECEIVE_COMPLETE);
			}
		}
	}
}

/* Pull whatever is in the FIFO. Used at the start of Receive(): a byte that
 * arrived before RX was armed would otherwise sit there until the *next* byte
 * arrived to raise the interrupt. */
static void usart_rx_drain(void)
{
	uint32_t guard = 0u;

	while ((reg_read(REG_LSR) & LSR_DATA_READY) != 0u && guard++ < 64u) {
		uint8_t byte = (uint8_t)(reg_read(REG_RBR) & 0xffu);

		if (rx_active && rx_remaining > 0u) {
			rx_buf[rx_completed] = byte;
		uart_rx_bytes_seen++;
			rx_completed++;
			rx_remaining--;
		}
	}
}

/* Begin receiving: install the console interrupt and arm the first byte.
 *
 * Reachable two ways, both standard: through the CMSIS control code
 * ARM_USART_CONTROL_RX (the portable way, and what an adapter should use), or
 * via the USART's own Receive() once the interrupt is up.
 *
 * Deliberately NOT part of PowerControl(ARM_POWER_FULL). Powering a console up
 * and taking interrupts on it are different intentions: the boot path powers the
 * console up before the scheduler exists, and an interrupt enabled at that
 * moment has no task to wake. Separating them lets the integrator start
 * receiving once there is somewhere for the bytes to go.
 *
 * Idempotent. Returns ARM_DRIVER_OK. */
/* Word of GICD_ICFGR holding this INTID's trigger configuration (16 INTIDs
 * per word, 2 bits each). Read-only diagnostic: if the firmware left the
 * console line EDGE-triggered instead of LEVEL, every level-line theory in
 * this file is wrong and the ICFGR readback says so directly. */
static uint32_t console_icfgr_word(uint32_t intid)
{
	return reg_rd32(BOARD_GICD_BASE + 0xC00u + 4u * (intid / 16u));
}

/* Word of GICD_ISENABLER holding this INTID's enable bit (32 INTIDs per
 * word). Read-only diagnostic companion to the icfgr readback: pend=1 with
 * the enable bit 0 would mean a software set-pending, not a line the GIC
 * was ever watching. */
static uint32_t console_isen_word(uint32_t intid)
{
	return reg_rd32(BOARD_GICD_BASE + 0x100u + 4u * (intid / 32u));
}

/* Boot-sequence window probe: what the GIC and the part think at a named
 * point of the shell adapter's bring-up. The D41 attribution question is
 * WHICH bring-up step first shows INTID 150 pending; this is the read that
 * answers it. No IIR read here on purpose - an IIR read consumes the
 * pending identification, erasing exactly the evidence a probe is for. */
void uart_console_window_probe(const char *tag)
{
	char line[112];

	(void)snprintf(line, sizeof(line),
		       "uart: w %s: pend=%u ier=%02x lcr=%02x isen=%08x\n",
		       tag,
		       (unsigned)IRQ_GetPending((IRQn_ID_t)console_intid),
		       (unsigned)reg_read(REG_IER),
		       (unsigned)reg_read(REG_LCR),
		       (unsigned)console_isen_word(console_intid));
	board_early_print_raw(line);
}

static int32_t usart_rx_start(void)
{
	if (usart_power != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR;
	}

	/* Pre-arm snapshot, stamped raw: the boot arm prints the pending
	 * state BEFORE the line is enabled, which pins down when a held
	 * assertion started (pend=1 here means the GIC input was already
	 * high before we had anything armed - the assertor predates the
	 * console driver entirely). */
	{
		char line[112];

		(void)snprintf(line, sizeof(line),
			       "uart: arm: intid=%u pend=%u ier=%02x"
			       " lcr=%02x icfgr=%08x isen=%08x\n",
			       (unsigned)console_intid,
			       (unsigned)IRQ_GetPending(
				       (IRQn_ID_t)console_intid),
			       (unsigned)reg_read(REG_IER),
			       (unsigned)reg_read(REG_LCR),
			       (unsigned)console_icfgr_word(console_intid),
			       (unsigned)console_isen_word(console_intid));
		board_early_print_raw(line);
	}

	/* Everything that the handler depends on goes FIRST, and the interrupt
	 * line is enabled LAST.
	 *
	 * This ordering is the whole point of the function. An interrupt can
	 * arrive the instant the line is enabled - a byte already sitting in the
	 * FIFO is enough, and there may be one from before the console was ours.
	 * If the handler runs before the receive state exists it finds
	 * rx_active=0, consumes nothing, reports nothing, and the level-triggered
	 * line stays asserted: the handler re-enters forever and the system
	 * wedges in an interrupt storm with no output at all. That is exactly
	 * what an earlier revision did, and it is indistinguishable from a wrong
	 * INTID - both look like "the console interrupt is not working".
	 *
	 * So: handler, priority, state, FIFO drain, IER - then enable. */
	(void)IRQ_SetHandler((IRQn_ID_t)console_intid,
			     (IRQHandler_t)usart_rx_irq_handler);
	(void)IRQ_SetPriority((IRQn_ID_t)console_intid,
			      BOARD_IRQ_PRIORITY_API_CALL_RAW);

	rx_active = 1u;
	rx_completed = 0u;
	if (rx_client_buf != NULL) {
		/* Resume the client's Receive so completions keep delivering
		 * into the buffer the client actually reads. */
		rx_buf = rx_client_buf;
		rx_remaining = rx_client_len;
	} else {
		rx_buf = rx_first_byte;
		rx_remaining = SHELL_RX_ARM_COUNT;
	}

	/* Drain the FIFO BEFORE unmasking the source. Draining is what actually
	 * drops a level-triggered RX condition; a byte left in the FIFO would
	 * re-assert the line the instant IER stops masking it - the same order
	 * usart_receive() already applies. (Until 2026-09-21 this armed IER
	 * first and drained second, which is the D41 experiment's write-order
	 * candidate for the phantom pend=1 at arm.) */
	usart_rx_drain();
	reg_write(REG_IER, IER_RX_AVAILABLE);

	/* A successful full arm clears the storm defence: this is the
	 * recovery path (shell auto-kick, uartint rebind) as well as the
	 * boot arm. */
	console_rx_down = 0u;

	(void)IRQ_Enable((IRQn_ID_t)console_intid);
	return ARM_DRIVER_OK;
}

/* Re-bind the console RX interrupt to a different INTID at runtime.
 *
 * Debug tool for the one board fact this driver cannot establish by itself:
 * which GIC line the UART actually sits on. Three device trees and the
 * vendor header say 150; the lab's FreeBSD logs said 66. With this, a
 * wrong-number hypothesis costs a shell command, not a rebuild.
 *
 * Runs the same handler -> priority -> state -> drain -> IER -> enable
 * sequence as rx_start, after masking the source and disabling the old
 * line, so neither the old nor the new INTID can storm mid-switch.
 * Returns 0 on success. */
int uart_console_irq_rebind(unsigned int intid)
{
	if (usart_power != ARM_POWER_FULL) {
		return -1;
	}
	if ((intid == 0U) || (intid > 1019U) ||
	    (intid >= 8192U && intid < 8192U + 256U)) {
		return -1;
	}
	if (intid == console_intid) {
		return 0;
	}

	reg_write(REG_IER, 0x00u);
	(void)IRQ_Disable((IRQn_ID_t)console_intid);
	(void)IRQ_SetHandler((IRQn_ID_t)console_intid, NULL);

	console_intid = intid;
	return (usart_rx_start() == ARM_DRIVER_OK) ? 0 : -1;
}

/* The INTID console RX is currently bound to (for reporting). */
unsigned int uart_console_irq_id(void)
{
	return console_intid;
}

/* 1 when the storm defence has armed RX off and a re-arm is pending. The
 * shell adapter polls this (kernel-free driver: it exposes state and
 * primitives, the kernel-aware adapter owns any timing). */
int uart_console_rx_down(void)
{
	return (console_rx_down != 0u) ? 1 : 0;
}

/* Pending / active bit of the console INTID from the Distributor (SPIs
 * live in the GICD_ISPENDR / GICD_ISACTIVER words of 32; word_offset is
 * 0x200 / 0x300). */
static uint32_t console_intid_bit(uint32_t word_offset)
{
	return (reg_rd32(BOARD_GICD_BASE + word_offset +
			 4u * (console_intid / 32u)) >>
		(console_intid % 32u)) & 1u;
}

/* One-stamped-line snapshot of the console interrupt path, callable from
 * any task context. The boot path probes before the tick is armed and
 * again before the kernel starts, bracketing WHEN the INTID 150 line goes
 * high (see the 20260920 console-storm evidence). */
void uart_console_line_probe(const char *tag)
{
	char line[112];

	(void)snprintf(line, sizeof(line),
		       "uart: probe %s: intid=%u pend=%u act=%u"
		       " ier=%02x lsr=%02x iir=%02x\n",
		       (tag != NULL) ? tag : "?",
		       (unsigned)console_intid,
		       (unsigned)console_intid_bit(0x200u),
		       (unsigned)console_intid_bit(0x300u),
		       (unsigned)reg_read(REG_IER),
		       (unsigned)reg_read(REG_LSR),
		       (unsigned)reg_read(REG_IIR));
	board_log("%s", line);
}

/* Re-arm console RX after a storm defence - the same full
 * handler -> priority -> state -> drain -> IER -> enable sequence as the
 * boot arm, which also clears the down state. This is what `uartint` and
 * the shell adapter's auto-recovery both land on. Returns 0 on success. */
int uart_console_rx_kick(void)
{
	static uint8_t kicks;

	/* Post-EOI ACTIVE sample, taken in TASK context: the INTID is
	 * disabled here and the last storm handler's EOI has long been
	 * written - the reading the in-handler dump cannot give (there the
	 * active bit reads 1 trivially, the ack just happened). First
	 * three kicks only. */
	if (kicks < 3u) {
		char line[96];

		kicks++;
		(void)snprintf(line, sizeof(line),
			       "uart: pre-kick %u: pend=%u act=%u ier=%02x\n",
			       (unsigned)kicks,
			       (unsigned)console_intid_bit(0x200u),
			       (unsigned)console_intid_bit(0x300u),
			       (unsigned)reg_read(REG_IER));
		board_early_print_raw(line);
	}

	return (usart_rx_start() == ARM_DRIVER_OK) ? 0 : -1;
}

/* --- M0 console RX bring-up probe ------------------------------------------
 *
 * One-shot diagnostic, run from the application's report task. It answers,
 * in a single boot, every remaining "why does typing do nothing" question:
 *
 *   1. do typed bytes reach the FIFO at all?   (polled LSR.DR count)
 *   2. is the UART configured to assert?       (IER/LCR/IIR readback)
 *   3. does the assert reach the distributor, and on which INTID?
 *      (GICD_ISPENDR snapshot before/after the window; a level SPI held
 *      asserted is recorded in ISPENDR even while disabled, so this finds
 *      the true line without enabling anything on a guess)
 *   4. if that line is not the one we armed, rebind to it immediately.
 *
 * The UART is NOT read during the window, so its IRQ output stays held and
 * the pending bit is visible. */
void uart_rx_probe(void)
{
	uint32_t before[12];
	uint32_t after[12];
	uint32_t freq;
	uint32_t start;
	uint32_t now;
	uint32_t polls = 0U;
	int candidate = -1;
	int w;
	int b;

	board_log("uart: rxprobe: console intid=%u ier=%02x lcr=%02x"
		  " mcr=%02x iir=%02x lsr=%02x",
		  (unsigned)console_intid,
		  (unsigned)reg_read(REG_IER),
		  (unsigned)reg_read(REG_LCR),
		  (unsigned)reg_read(REG_MCR),
		  (unsigned)reg_read(REG_IIR),
		  (unsigned)reg_read(REG_LSR));
	board_log("uart: rxprobe: type now (3s window)");

	for (w = 0; w < 12; w++) {
		before[w] = reg_rd32(BOARD_GICD_BASE + 0x200u +
				     4u * (uint32_t)w);
	}

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(start));
	do {
		if ((reg_read(REG_LSR) & LSR_DATA_READY) != 0u) {
			polls++;
		}
		__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(now));
	} while ((now - start) < (freq * 3ULL));

	for (w = 0; w < 12; w++) {
		after[w] = reg_rd32(BOARD_GICD_BASE + 0x200u +
				    4u * (uint32_t)w);
	}

	{
		char line[128];
		int off;

		off = snprintf(line, sizeof(line),
			       "uart: rxprobe: bytes-arrived=%u iir=%02x"
			       " lsr=%02x pending:",
			       (unsigned)polls,
			       (unsigned)reg_read(REG_IIR),
			       (unsigned)reg_read(REG_LSR));
		for (w = 0; w < 12 && off > 0 &&
		     off < (int)sizeof(line) - 1; w++) {
			uint32_t set = after[w] & ~before[w];

			if (after[w] != 0u) {
				off += snprintf(line + off,
						sizeof(line) - (size_t)off,
						" w%u=%02x", (unsigned)w,
						(unsigned)after[w]);
			}
			for (b = 0; b < 32; b++) {
				if (((set & (1u << b)) != 0u) &&
				    (candidate < 0)) {
					candidate = (w * 32) + b;
				}
			}
		}
		board_log("%s", line);
	}

	/* Drain whatever the window left, so a level condition cannot stay
	 * held on a line we are about to rebind. */
	while ((reg_read(REG_LSR) & LSR_DATA_READY) != 0u) {
		(void)reg_read(REG_RBR);
	}

	if (candidate > 31) {
		if ((unsigned int)candidate == console_intid) {
			board_log("uart: rxprobe: uart asserts on the armed"
				  " intid; irq delivery path is the suspect");
		} else if (candidate <= 1019) {
			board_log("uart: rxprobe: uart asserts on intid %u;"
				  " rebinding console", (unsigned)candidate);
			(void)uart_console_irq_rebind((unsigned int)candidate);
		}
	} else {
		board_log("uart: rxprobe: no new distributor pending bit;"
			  " the uart irq is not reaching the gic");
	}
}

static int32_t usart_transfer(const void *data_out, void *data_in, uint32_t num)
{
	(void)data_in;
	/* Half-duplex transfer (TX only) is expressible as Send; RX is not
	 * supported, so a full-duplex Transfer cannot be honoured. */
	if (data_in != 0) {
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}
	return usart_send(data_out, num);
}

static uint32_t usart_get_tx_count(void)
{
	return usart_tx_count;
}

/* Bytes of the current (or just-completed) reception. This is the value the
 * completion callback reads to know how much to consume - a previous
 * revision returned a counter that was never incremented, so every byte the
 * ISR correctly delivered was dropped by the consumer, which looks exactly
 * like "console RX is dead" while the interrupt path works the whole time. */
static uint32_t usart_get_rx_count(void)
{
	return rx_completed;
}

static int32_t usart_control(uint32_t control, uint32_t arg)
{
	if (usart_power != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR;
	}

	switch (control & ARM_USART_CONTROL_Msk) {
	case ARM_USART_MODE_ASYNCHRONOUS:
		if (arg != UART_CONSOLE_BAUD) {
			/* The divisor is fixed at build time; claiming to
			 * accept another rate would silently produce framing
			 * errors on the wire. */
			return ARM_USART_ERROR_BAUDRATE;
		}
		if ((control & ARM_USART_DATA_BITS_Msk) != ARM_USART_DATA_BITS_8 ||
		    (control & ARM_USART_PARITY_Msk) != ARM_USART_PARITY_NONE ||
		    (control & ARM_USART_STOP_BITS_Msk) != ARM_USART_STOP_BITS_1) {
			return ARM_USART_ERROR_DATA_BITS;
		}
		if ((control & ARM_USART_FLOW_CONTROL_Msk) != ARM_USART_FLOW_CONTROL_NONE) {
			return ARM_USART_ERROR_FLOW_CONTROL;
		}
		return ARM_DRIVER_OK;

	case ARM_USART_CONTROL_TX:
		/* Nothing to gate: Send() is synchronous and the transmitter is
		 * always available once powered. */
		return ARM_DRIVER_OK;

	case ARM_USART_CONTROL_RX:
		/* arg=0 disables reception (mask the source, keep the driver
		 * state consistent); non-zero starts it. This is where the
		 * console interrupt is installed - see usart_rx_start(). */
		if (arg == 0u) {
			reg_write(REG_IER, 0x00u);
			rx_active = 0u;
			return ARM_DRIVER_OK;
		}
		return usart_rx_start();

	case ARM_USART_ABORT_SEND:
	case ARM_USART_ABORT_RECEIVE:
	case ARM_USART_ABORT_TRANSFER:
		/* Nothing is ever in flight: Send is synchronous. */
		return ARM_DRIVER_OK;

	default:
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}
}

static ARM_USART_STATUS usart_get_status(void)
{
	ARM_USART_STATUS status;
	uint32_t lsr = reg_read(REG_LSR);

	status.tx_busy = (lsr & LSR_THR_EMPTY) ? 0u : 1u;
	status.rx_busy = 0;
	status.tx_underflow = 0;
	status.rx_overflow = (lsr & LSR_OVERRUN) ? 1u : 0u;
	status.rx_break = (lsr & LSR_BREAK) ? 1u : 0u;
	status.rx_framing_error = (lsr & LSR_FRAMING_ERR) ? 1u : 0u;
	status.rx_parity_error = (lsr & LSR_PARITY_ERR) ? 1u : 0u;
	status.reserved = 0;

	return status;
}

static int32_t usart_set_modem_control(ARM_USART_MODEM_CONTROL control)
{
	uint32_t mcr = reg_read(REG_MCR);

	switch (control) {
	case ARM_USART_RTS_CLEAR:
		mcr &= ~(1u << 1);
		break;
	case ARM_USART_RTS_SET:
		mcr |= (1u << 1);
		break;
	case ARM_USART_DTR_CLEAR:
		mcr &= ~(1u << 0);
		break;
	case ARM_USART_DTR_SET:
		mcr |= (1u << 0);
		break;
	default:
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	reg_write(REG_MCR, mcr);
	return ARM_DRIVER_OK;
}

static ARM_USART_MODEM_STATUS usart_get_modem_status(void)
{
	ARM_USART_MODEM_STATUS status;
	uint32_t msr = reg_read(REG_MSR);

	status.cts = (msr & MSR_CTS) ? 1u : 0u;
	status.dsr = (msr & MSR_DSR) ? 1u : 0u;
	status.dcd = (msr & MSR_DCD) ? 1u : 0u;
	status.ri = (msr & MSR_RI) ? 1u : 0u;
	status.reserved = 0;

	return status;
}

/* The single console USART instance. Upper layers take this handle; they must
 * not include uart_ns16550.h. */
ARM_DRIVER_USART Driver_USART_Console = {
	usart_get_version,
	usart_get_capabilities,
	usart_initialize,
	usart_uninitialize,
	usart_power_control,
	usart_send,
	usart_receive,
	usart_transfer,
	usart_get_tx_count,
	usart_get_rx_count,
	usart_control,
	usart_get_status,
	usart_set_modem_control,
	usart_get_modem_status
};
