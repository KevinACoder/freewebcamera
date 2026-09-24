/*
 * @file   dbg_scenario.c
 * @brief  The UP carrier's scenario runner (D56): shell commands, called
 *         from main() instead of a shell.
 *
 * Why this exists: the cherrysh console and the gdb stub fight over the one
 * UART (interrupt-driven RX vs the stub's polled port), and the carrier's
 * whole point is a debug model with as little machinery as possible. So the
 * carrier has no console input at all - IER stays 0, nothing competes with
 * the stub - and the scenarios below are the boot-time equivalent of the
 * mainline's *start tasks, invoked on demand instead of all at boot.
 *
 * Selection is a variable, not an argv: there is no console to type into.
 * The host sets it from the stub, which is also the natural place to set
 * breakpoints inside the scenario's workers before they run:
 *
 *     (gdb) b net_start
 *     (gdb) set var dbg_scenario = 1
 *     (gdb) continue
 *
 * Scenario bodies mirror app/main.c's task_* functions one for one - same
 * calls, same order, same log wording. Deliberately NOT here: the operator
 * ladder (wpa/dhcp/ping). include/wlan.h freezes only boot-time state into
 * the interface; scan/join/supplicant stay shell-side by decision, and the
 * carrier reaches them the gdb way instead - `call wpa_port_start()` etc.
 * from the break this task sits in (a thread context, so blocking calls
 * are safe).
 */

#include <stdint.h>

#include "board.h"
#include "gdb/gdb.h"

#include "dbg_scenario.h"
#include "fs.h"
#include "net.h"
#include "sdio.h"
#include "usb.h"
#include "wlan.h"

volatile int dbg_scenario;
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

/* --- scenarios: mirrors of main.c's task bodies --------------------------- */

static void scenario_none(void)
{
	/* Pure attach-and-inspect carrier: nothing to run. */
}

static void scenario_net(void)
{
	if (net_start() != 0) {
		board_log("net: FAIL\n");
		return;
	}
	board_log("net: READY\n");
}

static void scenario_sdio(void)
{
	if (sdio_start() != 0) {
		board_log("sdio: FAIL\n");
		return;
	}
	board_log("sdio: READY\n");

	/* claim the RTL8189FTV for the wlan lane now that the card is up
	 * (no-op unless wlan_start() already ran; usb scenario probes too) */
	if (wlan_sdio_probe() != 0) {
		board_log("wlan: no sdio adapter claimed\n");
	}
}

static void scenario_usb(void)
{
	/* The wlan services (locks, sleeps, firmware registry) must exist
	 * before enumeration: the CherryUSB class hook attaches a matched
	 * adapter on the hub thread, inside usb_start(). The radio itself
	 * stays down until driven (shell lane: `wlan scan`/`wpa start`;
	 * carrier: gdb calls). */
	if (wlan_start() != 0) {
		board_log("wlan: FAIL\n");
	}
	/* the SDIO slot may have enumerated before the wlan services came
	 * up; the probe no-ops until both sides exist (the sdio scenario
	 * also probes after sdio_start()) */
	if (wlan_sdio_probe() != 0) {
		board_log("wlan: no sdio adapter yet\n");
	}

	if (usb_start() != 0) {
		board_log("usb: FAIL\n");
		return;
	}
	board_log("usb: READY\n");
}

static void scenario_fs(void)
{
	if (fs_start() != 0) {
		board_log("fs: FAIL\n");
		return;
	}
	board_log("fs: READY\n");
}

/* The mainline boot set in one run: services first, enumerators last -
 * wlan_start before usb_start (wlan.h contract), usb behind everything
 * because it is the slowest and the noisiest. */
static void scenario_all(void)
{
	if (wlan_start() != 0) {
		board_log("wlan: FAIL\n");
	}
	if (net_start() != 0) {
		board_log("net: FAIL\n");
	}
	if (sdio_start() != 0) {
		board_log("sdio: FAIL\n");
	}
	(void)wlan_sdio_probe();
	if (fs_start() != 0) {
		board_log("fs: FAIL\n");
	}
	if (usb_start() != 0) {
		board_log("usb: FAIL\n");
		return;
	}
	board_log("usb: READY\n");
}

static void scenario_debug_probe(void)
{
	dbg_probe_breakpoint();
	dbg_probe_step();
}

typedef void (*scenario_fn)(void);

static const scenario_fn scenarios[] = {
	scenario_none,	/* 0 */
	scenario_net,	/* 1 */
	scenario_sdio,	/* 2 */
	scenario_usb,	/* 3 */
	scenario_fs,	/* 4 */
	scenario_all,	/* 5 */
	scenario_debug_probe,	/* 6 */
};

#define SCENARIO_COUNT	((int)(sizeof(scenarios) / sizeof(scenarios[0])))

/* --- the carrier thread ----------------------------------------------------- */

void dbg_scenario_task(void *argument)
{
	(void)argument;

	board_log("dbg: carrier ready - gdb attach, 'set var dbg_scenario=N',"
		  " continue\n");

	for (;;) {
		int which;

		/* The gate. The first break lands here before anything else
		 * runs - it doubles as the "is the stub alive" heartbeat when
		 * the tick poll is not an option (e.g. tick wedged). */
		gdb_break();

		which = dbg_scenario;
		if (which < 0 || which >= SCENARIO_COUNT) {
			board_log("dbg: unknown scenario %d (0..%d)\n", which,
				  SCENARIO_COUNT - 1);
			dbg_scenario = 0;
			continue;
		}

		board_log("dbg: scenario %d start\n", which);
		scenarios[which]();
		board_log("dbg: scenario %d done\n", which);
	}
}
