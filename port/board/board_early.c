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
 */

#include <stddef.h>

#include "board.h"

void (*board_early_print_hook)(const char *message);

void board_early_print(const char *message)
{
	if (board_early_print_hook != NULL) {
		board_early_print_hook(message);
	}
}
