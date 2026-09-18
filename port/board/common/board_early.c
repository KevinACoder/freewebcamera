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

void board_early_print(const char *message)
{
	uint64_t saved_daif;

	print_lock_take(&saved_daif);
	uart_early_puts(message);
	print_lock_give(saved_daif);
}

/* Same sink, with numbers: drivers that report what they found (register
 * versions, PHY ids, negotiated speed) would otherwise each carry their own
 * formatter. The buffer is deliberately small - this is bring-up output on a
 * polled 115200 console, not a logging system, and a driver that wants to
 * print per packet has picked the wrong mechanism. */
void board_log(const char *fmt, ...)
{
	char line[128];
	va_list ap;
	uint64_t saved_daif;

	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	print_lock_take(&saved_daif);
	uart_early_puts(line);
	print_lock_give(saved_daif);
}
