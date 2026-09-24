/*
 * @file   dbg_scenario.c
 * @brief  gdb-lane diagnostics (app layer): the breakpoint probe targets.
 *
 * The trunk image boots into cherrysh; these hooks exist for the gdb lane.
 * `dbg` (cherrysh adapter) calls dbg_probe_breakpoint(): the BRK enters a
 * stub session stopped at the probe, which is exactly the Z0 software-
 * breakpoint acceptance stop - and any gdb session can `call
 * dbg_probe_step()` / set a breakpoint on either symbol afterwards.
 *
 * The old UP carrier's service scenarios (net/usb/fs/sdio/wlan) return with
 * their feat/xxx lines; the probe below is what remains once they are gone.
 */

#include "dbg_scenario.h"

volatile unsigned long dbg_probe_word;

__attribute__((noinline)) void dbg_probe_breakpoint(void)
{
	__asm__ __volatile__("nop" ::: "memory");
	dbg_probe_word = 0x3568UL;
}

__attribute__((noinline)) void dbg_probe_step(void)
{
	__asm__ __volatile__("nop\n\tnop\n\tnop" ::: "memory");
}
