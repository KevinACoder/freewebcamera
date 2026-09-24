/*
 * @file   board_early.c
 * @brief  Fatal bring-up messages and driver probe reports, on the polled
 *         early UART.
 *
 * Board code (GICv3 bring-up in particular) has real failure modes that are
 * otherwise silent: a distributor RWP that never clears, a redistributor that
 * never wakes, ICC_SRE that refuses to set. Each of those hangs the board in
 * a way indistinguishable from dead hardware, so they must be reported.
 *
 * The sink is the POLLED early writer from startup.S, not the console
 * driver. That choice is load-bearing, not cosmetic: a spinlock may only be
 * held across a non-blocking call, and the console driver's Send path blocks
 * on a TX-complete semaphore. An earlier revision held the print lock across
 * console_print - under SMP two tasks printed concurrently, one blocked in
 * the driver holding the lock, and the whole machine went silent (the last
 * console line was two prints mashed into each other; the reference SMP
 * line's round-5-10 "intermittent freeze" is the same shape). Polled output
 * cannot block, so the lock discipline below is sound: held with IRQs
 * masked (a same-core ISR printing through here would otherwise spin on the
 * lock its own interrupted context holds - configASSERT does exactly that),
 * caller's DAIF restored on release, one line atomic against other
 * board_early_print callers.
 *
 * App console output (shell echo, task prints) still goes through the
 * console driver and its own blocking serialisation - that is correct
 * there, because nothing holds a spinlock across it.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "board.h"

/* The polled early writer from startup.S: available from the first
 * instruction, never blocks, no driver state. Board bring-up output goes
 * HERE - not through the console driver (see the lock comment below). */
extern void uart_early_puts(const char *s);

/* --- boot-relative timestamp ----------------------------------------------- */

/* NetBSD-shaped dmesg stamp: seconds since the first stamped print,
 * right-aligned in width 4, three fractional digits ("[   1.234] "). The
 * fractional precision is deliberately fixed at milliseconds - the console
 * renders about one character per 87 us, so finer digits would be fake
 * precision on the wire. */
#define BOARD_TS_STAMP_LEN 16u

/* Counter value at the first stamped print; 0 means "not captured yet".
 * Captured lazily because there is no single "C start" call before the
 * earliest board_log callers (GIC bring-up runs before board_main). Two
 * cores racing on the first call store near-identical values; the u64 store
 * is a single aligned STR, so the worst case is a one-tick offset in t0. */
static uint64_t log_t0;

static uint64_t read_cntvct(void)
{
	uint64_t v;

	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static uint64_t read_cntfrq(void)
{
	uint64_t v;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* 64/32 restoring division. This image links no libgcc, and a runtime
 * divisor (cntfrq comes from a register) cannot be constant-folded into the
 * compiler's umulh reciprocal trick the way minilibc's literal bases are.
 * 64 iterations is far below the per-line cost of a 115200 console. */
static uint64_t udiv64(uint64_t n, uint32_t d)
{
	uint64_t q = 0u;
	uint64_t r = 0u;
	int i;

	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		if (r >= (uint64_t)d) {
			r -= (uint64_t)d;
			q |= (1ULL << i);
		}
	}
	return q;
}

/* Render the stamp for "now" into buf (BOARD_TS_STAMP_LEN capacity) and
 * return its length. */
int board_uptime_stamp(char buf[BOARD_TS_STAMP_LEN])
{
	uint64_t delta, freq, sec;
	uint32_t frac;

	if (log_t0 == 0u) {
		log_t0 = read_cntvct();
		if (log_t0 == 0u) {
			log_t0 = 1u;	/* counter really was 0: don't re-capture */
		}
	}

	delta = read_cntvct() - log_t0;
	freq = read_cntfrq();
	if (freq == 0u) {
		freq = 1u;
	}
	sec = udiv64(delta, (uint32_t)freq);
	frac = (uint32_t)udiv64((delta - sec * freq) * 1000u,
				(uint32_t)freq);

	return snprintf(buf, BOARD_TS_STAMP_LEN, "[%4u.%03u] ",
			(unsigned int)sec, frac);
}

/* Boot-relative uptime split out for callers that want the numbers (the
 * shell's uptime command cross-checks this clock against the OS tick). */
void board_uptime_parts(uint32_t *sec, uint32_t *ms)
{
	uint64_t delta, freq, s;

	if (log_t0 == 0u) {
		log_t0 = read_cntvct();
		if (log_t0 == 0u) {
			log_t0 = 1u;
		}
	}

	delta = read_cntvct() - log_t0;
	freq = read_cntfrq();
	if (freq == 0u) {
		freq = 1u;
	}
	s = udiv64(delta, (uint32_t)freq);
	if (sec != NULL) {
		*sec = (uint32_t)s;
	}
	if (ms != NULL) {
		*ms = (uint32_t)udiv64((delta - s * freq) * 1000u,
				       (uint32_t)freq);
	}
}

/* --- one-line print lock --------------------------------------------------- */

static volatile unsigned int print_lock_held;

/* LDAXR/STXR pair, inline: __atomic_test_and_set would call into libatomic,
 * which a -nostdlib image does not have. Returns 0 when the lock was taken. */
static inline unsigned int print_try_lock(void)
{
	unsigned int status;
	unsigned int wanted = 1u;

	__asm__ __volatile__(
		"	ldaxr	%w0, %2\n"
		"	cbnz	%w0, 1f\n"
		"	stxr	%w0, %w1, %2\n"
		"1:\n"
		: "=&r"(status)
		: "r"(wanted), "Q"(*(volatile unsigned int *)&print_lock_held)
		: "memory");

	return status;
}

static void print_lock_take(uint64_t *saved_daif)
{
	/* Mask IRQs for the whole hold, and restore the caller's DAIF after
	 * release. The reason is same-core re-entrancy, measured on the
	 * reference SMP line as days of intermittent freeze: a task holds
	 * this lock mid-line, takes a tick/SGI on the same core, and the ISR
	 * path (configASSERT and the fatal hooks print through here) spins
	 * forever on the lock its own interrupted context is holding - the
	 * holder can never run to release it. With IRQs masked for the hold,
	 * no same-core ISR can even start. Cross-core contention is safe
	 * without this: the holder is running and will release. */
	__asm__ __volatile__("mrs %0, daif" : "=r"(*saved_daif));
	__asm__ __volatile__("msr daifset, #2" ::: "memory");

	while (print_try_lock() != 0u) {
		__asm__ __volatile__("yield" ::: "memory");
	}
}

static void print_lock_give(uint64_t saved_daif)
{
	/* STLR: store with release semantics, the pairing half of LDAXR. */
	__asm__ __volatile__("stlr %w1, %0"
			     : "=Q"(print_lock_held)
			     : "r"(0u)
			     : "memory");

	/* Restore the caller's interrupt state exactly (whole DAIF, not just
	 * the I bit: callers in ISR context arrive with IRQs masked and must
	 * stay that way). */
	__asm__ __volatile__("msr daif, %0" ::"r"(saved_daif) : "memory");
}

/* Line-ending normalisation for the stamped sinks. Vendored components log
 * in their own house style and the shadow mappings append their own tail:
 * fsl_sdmmc's SDMMC_LOG fmts carry "\r\n" while the shadow adds "\n" (so a
 * single entry would hit the wire as "\n\n" - a blank line per log line),
 * CherryUSB appends "\r\n", and the CR itself reads as a line break on
 * terminals that count CR. Rule: CRs are dropped, and a second '\n'
 * directly after one already emitted is dropped too - every entry ends as
 * exactly one "\r\n" on the wire, fragment-style logs (no trailing
 * newline) still concatenate, and nothing else changes. */
static void puts_strip_cr(const char *s)
{
	char prev = 0;

	for (; *s != '\0'; s++) {
		if (*s == '\r') {
			continue;
		}
		if (*s == '\n' && prev == '\n') {
			continue;
		}
		uart_early_putc(*s);
		prev = *s;
	}
}

void board_early_print(const char *message)
{
	uint64_t saved_daif;
	char ts[BOARD_TS_STAMP_LEN];

	(void)board_uptime_stamp(ts);
	print_lock_take(&saved_daif);
	uart_early_puts(ts);
	puts_strip_cr(message);
	print_lock_give(saved_daif);
}

void board_console_write(const char *message)
{
	uint64_t saved_daif;

	print_lock_take(&saved_daif);
	puts_strip_cr(message);
	print_lock_give(saved_daif);
}

/* Same sink and stamp, WITHOUT the print lock: for contexts that must not
 * spin on it. The lock is taken with IRQs masked, so a non-fatal print from
 * an ISR could deadlock against the very context it interrupted (the holder
 * would never run to release); the SMP bring-up window and the secondary
 * descent have the same shape. Lines from here may interleave mid-line with
 * locked output - the accepted cost of being callable from anywhere. */
void board_early_print_raw(const char *message)
{
	char ts[BOARD_TS_STAMP_LEN];

	(void)board_uptime_stamp(ts);
	uart_early_puts(ts);
	puts_strip_cr(message);
}

/* Same sink, with numbers: drivers that report what they found (register
 * versions, PHY ids, negotiated speed) would otherwise each carry their own
 * formatter. The stamp is composed first so the message budget shrinks by
 * its length. The buffer is deliberately small - this is bring-up output on
 * a polled 115200 console, not a logging system, and a driver that wants to
 * print per packet has picked the wrong mechanism. */
void board_log(const char *fmt, ...)
{
	char line[128];
	va_list ap;
	uint64_t saved_daif;
	int off;
	int i;
	int out;

	off = board_uptime_stamp(line);
	va_start(ap, fmt);
	(void)vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
	va_end(ap);

	/* Line-ending normalisation - see puts_strip_cr: drop CRs, collapse
	 * consecutive newlines (the shadow's "\n" plus fsl_sdmmc's own
	 * "\r\n" would otherwise emit a blank line after every entry). */
	for (i = off, out = off; line[i] != '\0'; i++) {
		if (line[i] == '\r') {
			continue;
		}
		if (line[i] == '\n' && out > off && line[out - 1] == '\n') {
			continue;
		}
		line[out++] = line[i];
	}
	line[out] = '\0';

	print_lock_take(&saved_daif);
	uart_early_puts(line);
	print_lock_give(saved_daif);
}
