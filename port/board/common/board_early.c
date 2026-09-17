/*
 * @file   board_early.c
 * @brief  Indirection for fatal bring-up messages.
 *
 * Board code (GICv3 bring-up in particular) has real failure modes that are
 * otherwise silent: a distributor RWP that never clears, a redistributor that
 * never wakes, ICC_SRE that refuses to set. Each of those hangs the board in
 * a way indistinguishable from dead hardware, so they must be reported.
 *
 * The reporting sink is installed by whoever owns the console, because the
 * console is a CMSIS ARM_DRIVER_USART and does not exist until it is
 * initialised. Before that the hook is NULL and reports are dropped - which is
 * correct, not a limitation: writing to an unprogrammed UART produces nothing
 * and would look like a working console quietly eating output.
 *
 * SMP: with secondaries running, several cores can reach these functions at
 * once (per-core GIC bring-up lines are the live example, three cores within
 * milliseconds of each other). The console driver underneath the hook is not
 * multi-core safe, so the hook call is serialized with a spinlock. The lock
 * is held for one line's worth of polled UART output; no interrupt context
 * prints through this path, so there is no same-core re-entrancy to worry
 * about. The interleaving of bytes on the wire disappears with the lock;
 * without it, three concurrent writes produced the shredded lines the SMP
 * bring-up logs show.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "board.h"

void (*board_early_print_hook)(const char *message);

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

static void print_lock_take(void)
{
	/* Pure spin, no WFE: this runs before the tick exists, so a lost
	 * SEV (event signalled before this core's WFE executed) would sleep
	 * forever - there is no interrupt to break it. Print lines are
	 * short; spinning costs nothing at boot. */
	while (print_try_lock() != 0u) {
		__asm__ __volatile__("yield" ::: "memory");
	}
}

static void print_lock_give(void)
{
	/* STLR: store with release semantics, the pairing half of LDAXR. */
	__asm__ __volatile__("stlr %w1, %0"
			     : "=Q"(print_lock_held)
			     : "r"(0u)
			     : "memory");
}

void board_early_print(const char *message)
{
	if (board_early_print_hook != NULL) {
		print_lock_take();
		board_early_print_hook(message);
		print_lock_give();
	}
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

	if (board_early_print_hook == NULL) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	print_lock_take();
	board_early_print_hook(line);
	print_lock_give();
}
