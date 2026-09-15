/*
 * @file   shell_stub.c
 * @brief  Null shell for the kernel-replacement build (K4).
 *
 * Part of the same exercise as cmsis_os2_stub.c, and it makes a stronger point
 * than that file does on its own.
 *
 * The application calls shell_start() through include/shell.h and knows nothing
 * else about the console. That is only meaningful if the app really does not
 * depend on which shell is behind the interface - so this build swaps CherrySH
 * out entirely and still links. If app/ ever reached into the CherrySH adapter
 * for something (a shell handle, a command-table symbol, a config macro), this
 * would stop linking.
 *
 * So K4 now demonstrates two independent seams at once:
 *
 *   - the RTOS seam:   app/ and drivers/ build with no kernel
 *   - the shell seam:  app/ builds with no shell
 *
 * Neither body runs; linking is the result.
 */

#include "shell.h"

int shell_start(void)
{
	/* No shell in this build. Returning failure is the honest answer, and it
	 * also exercises the app's error path. */
	return -1;
}

int shell_is_running(void)
{
	return 0;
}
