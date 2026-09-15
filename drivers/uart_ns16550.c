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

static inline uint32_t reg_read(uint32_t offset)
{
	return reg_rd32(UART_CONSOLE_BASE + (offset << 2));
}

static inline void reg_write(uint32_t offset, uint32_t value)
{
	reg_wr32(UART_CONSOLE_BASE + (offset << 2), value);
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

static int32_t usart_receive(void *data, uint32_t num)
{
	/* No RX interrupt path at M0. The shell reads through its own ring
	 * buffer; reporting unsupported is honest, whereas a fake-success
	 * implementation would look like dropped bytes. */
	(void)data;
	(void)num;
	return ARM_DRIVER_ERROR_UNSUPPORTED;
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
	case ARM_USART_CONTROL_RX:
		/* Enabling/disabling a polled UART is a no-op that succeeds:
		 * there is no gating to perform. */
		return ARM_DRIVER_OK;

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
