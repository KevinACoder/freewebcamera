/*
 * @file   lwip_diag.c
 * @brief  lwIP's platform diagnostic and assertion hooks for this board.
 *
 * lwIP's defaults for both call printf()/abort(); this image is freestanding
 * with no libc behind it, so the hooks are wired to the console driver
 * instead. They are reached from lwip/arch.h (see arch/cc.h) - the assertion
 * path in particular is live, because lwIP compiles LWIP_ASSERT in unless
 * LWIP_NOASSERT is defined, and a silently compiled-out assertion is exactly
 * the kind of thing that costs a day on the board.
 *
 * Output is a single line per event, on the same console the shell uses. The
 * console driver is polled and slow, so these are debug aids, not a logging
 * facility - the network path must never print per packet (DESIGN G18).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdarg.h>
#include <stdint.h>

#include "Driver_USART.h"

extern ARM_DRIVER_USART Driver_USART_Console;

/* Set once the console has been initialized. Before that the driver would
 * write into an unprogrammed UART, which looks like a working console that
 * prints nothing. */
static int diag_console_ready;

/* One printf-style buffer, reused: these calls are rare and never reentered
 * from an interrupt at the same time as a thread on this single-core target. */
static char diag_line[192];

static void diag_emit(const char *s)
{
	uint32_t n = 0;

	if (!diag_console_ready) {
		return;
	}
	while (s[n] != '\0') {
		n++;
	}
	(void)Driver_USART_Console.Send(s, n);
}

/* Called by the adapter once the console is initialized and printing is
 * meaningful. */
void lwip_diag_set_console_ready(void)
{
	diag_console_ready = 1;
}

void lwip_arch_diag(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(diag_line, sizeof(diag_line), fmt, ap);
	va_end(ap);
	diag_emit(diag_line);
}

/* lwIP treats a failed assertion as fatal. Print where it happened and park:
 * a spinning board with the message on the console is diagnosable, a
 * continuing board with corrupted state is not. */
void lwip_arch_assert(const char *message, const char *file, int line)
{
	(void)snprintf(diag_line, sizeof(diag_line), "lwip assert: %s (%s:%d)\n",
		       message, file, line);
	diag_emit(diag_line);

	for (;;) {
		__asm__ __volatile__("wfe");
	}
}