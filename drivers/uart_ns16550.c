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
static bool program_uart(void)
{
	uint32_t divisor = UART_CONSOLE_CLOCK_HZ / (16u * UART_CONSOLE_BAUD);

	/* Interrupts stay off: this console is polled, and IER=0 guarantees a
	 * stray interrupt cannot fire before the GIC is programmed. */
	reg_write(REG_IER, 0x00);

	reg_write(REG_LCR, LCR_DLAB);
	reg_write(REG_DLL, divisor & 0xffu);
	reg_write(REG_DLH, (divisor >> 8) & 0xffu);
	reg_write(REG_LCR, LCR_8N1);

	reg_write(REG_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX |
			   FCR_TRIGGER_14);

	/* DTR and RTS asserted, matching what the board's console expects. */
	reg_write(REG_MCR, 0x03);

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
 * port/board/gicv3.c), not a stuck transmit-ready bit. */
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
	uart_early_puts("[board] parked; power-cycle to recover\n");
}

/* --- CMSIS ARM_DRIVER_USART ----------------------------------------------- */

static ARM_USART_SignalEvent_t usart_callback;
static volatile uint32_t usart_tx_count;
static volatile uint32_t usart_rx_count;
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
	usart_rx_count = 0;
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
 * read by the ISR and by Receive/Control, so it is volatile. */
#define SHELL_RX_ARM_COUNT	1U
static uint8_t rx_first_byte[SHELL_RX_ARM_COUNT];

static volatile uint8_t *rx_buf;
static volatile uint32_t rx_remaining;
static volatile uint32_t rx_completed;
static volatile uint8_t  rx_active;

static void usart_rx_drain(void);

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
 * dead for an unknown reason" and a one-line answer in the boot log. */
void usart_rx_irq_handler(void)
{
	static uint8_t unexpected_reports;
	uint32_t guard = 0u;
	uint32_t iir = reg_read(REG_IIR) & IIR_ID_MASK;

	if (iir != IIR_ID_RX_AVAILABLE && iir != IIR_ID_RX_TIMEOUT) {
		if (unexpected_reports < 3U) {
			unexpected_reports++;
			uart_early_puts("[uart] interrupt without RX data: iir=");
			uart_early_put_hex32(iir);
			uart_early_puts(" lsr=");
			uart_early_put_hex32(reg_read(REG_LSR));
			uart_early_puts("\n");
		}
		return;
	}

	while ((reg_read(REG_LSR) & LSR_DATA_READY) != 0u) {
		uint8_t byte = (uint8_t)(reg_read(REG_RBR) & 0xffu);

		if (rx_active && rx_remaining > 0u) {
			rx_buf[rx_completed] = byte;
			rx_completed++;
			rx_remaining--;
		}
		/* If nobody asked for the byte it is still consumed: leaving it in
		 * the FIFO would keep the interrupt asserted forever. */

		if (++guard > 64u) {
			/* The FIFO cannot hold more than this; a larger count here
			 * would mean the data-ready bit is stuck, and looping on it
			 * would wedge the interrupt. */
			break;
		}
	}

	if (rx_active && rx_remaining == 0u) {
		reg_write(REG_IER, 0x00u);
		rx_active = 0u;
		if (usart_callback != 0) {
			usart_callback(ARM_USART_EVENT_RECEIVE_COMPLETE);
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
static int32_t usart_rx_start(void)
{
	if (usart_power != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR;
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
	(void)IRQ_SetHandler((IRQn_ID_t)BOARD_CONSOLE_INTID,
			     (IRQHandler_t)usart_rx_irq_handler);
	(void)IRQ_SetPriority((IRQn_ID_t)BOARD_CONSOLE_INTID,
			      BOARD_IRQ_PRIORITY_API_CALL_RAW);

	rx_active = 1u;
	rx_completed = 0u;
	rx_buf = rx_first_byte;
	rx_remaining = SHELL_RX_ARM_COUNT;

	/* Unmask the source, then clear anything that was pending from before
	 * the line was ours. Draining the FIFO is what actually drops a
	 * level-triggered RX condition; leaving a pre-existing byte in it would
	 * re-assert the moment the line is enabled. */
	reg_write(REG_IER, IER_RX_AVAILABLE);
	usart_rx_drain();

	(void)IRQ_Enable((IRQn_ID_t)BOARD_CONSOLE_INTID);
	return ARM_DRIVER_OK;
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

static uint32_t usart_get_rx_count(void)
{
	return usart_rx_count;
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
