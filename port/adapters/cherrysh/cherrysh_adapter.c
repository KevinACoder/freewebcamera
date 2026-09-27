/*
 * @file   cherrysh_adapter.c
 * @brief  CherrySH on this board: console wiring, a shell task, and commands.
 *
 * CherrySH is vendored byte-identical in third-party/cherrysh and is not
 * modified. Everything the board has to supply lives here.
 *
 * ---------------------------------------------------------------------------
 * WHAT CHERRYSH ACTUALLY REQUIRES
 *
 * Very little, which is why it was chosen. Its entire platform surface is two
 * function pointers in chry_shell_init_t:
 *
 *     sput(rl, buf, size)  -> write bytes to the console
 *     sget(rl, buf, size)  -> read bytes, returning 0 when none are available
 *
 * There is no OS abstraction struct, no malloc, no stdio, no threading model.
 * A previous plan assumed an "os_ops" adaptation layer; there is no such thing
 * in this library, and inventing one would have been building on a fiction.
 *
 * ---------------------------------------------------------------------------
 * THE DESIGN, AND WHY IT IS NOT THE OBVIOUS ONE
 *
 * The obvious shell task is:
 *
 *     for (;;) { chry_shell_task_repl(&sh); }      // with a BLOCKING sget
 *
 * That is wrong here, and the reason is worth stating because it is the kind of
 * bug that only shows up once there is real work in the system. With a blocking
 * sget the task spins in chry_readline_waitkey() whenever there is no input,
 * and because it never blocks, a lower-priority task never runs. The shell
 * would appear to work perfectly while quietly starving everything else - the
 * shell itself being the highest-priority thing in an otherwise idle system
 * hides it completely.
 *
 * So the shell runs CONFIG_CSH_NOBLOCK=1 and the task blocks on a thread flag
 * instead:
 *
 *     for (;;) {
 *         osThreadFlagsWait(..., osWaitForever);
 *         chry_shell_task_repl(&sh);    // returns 1 immediately if no line
 *     }
 *
 * The UART RX interrupt sets the flag (osThreadFlagsSetFromISR). The flag
 * latches, so input that arrives while the task is inside the REPL is not lost,
 * and the task is genuinely blocked when idle, so it costs nothing.
 *
 * sget returns 0 when the ring is empty, which is exactly the "no data right
 * now" contract readline's noblock mode expects - the ring buffer and the
 * noblock REPL are made for each other.
 *
 * ---------------------------------------------------------------------------
 * CHERRYRB, AND ITS BARRIER BOUNDARY
 *
 * The RX ring is CherryRB. It is a kfifo with no OS dependency, which is what
 * makes it usable from an interrupt. It also has NO MEMORY BARRIERS (its own
 * documentation is explicit about this), so the producer/consumer handoff needs
 * one added by us:
 *
 *   - the ISR writes bytes, then the `dsb` inside the ISR-exit path orders them
 *     before the flag it sets;
 *   - the consumer sees the flag, which is a synchronising event.
 *
 * That ordering is why the flag is set AFTER the ring write, and why
 * chry_ringbuffer_write_byte()'s single-producer/single-consumer design is
 * relied on rather than assumed: exactly one ISR writes, exactly one task reads.
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

#include "Driver_USART.h"
#include "board.h"
#include "cmsis_os2.h"
#include "cmsis_os2_ext.h"
#include "gicv3_its.h"
#include "shell.h"
#include "chry_ringbuffer.h"

/* Its own header: shell_start()'s declaration and the CSH_FROM_ARGV convention
 * used by every command below. */
#include "cherrysh_adapter.h"
/* csh.h, not chry_shell.h: csh.h pulls in csh_config.h (our shadow config) and
 * then the shell itself, and it is what defines the CSH_CMD_EXPORT* macros and
 * the chry_syscall_t/chry_sysvar_t types. Including chry_shell.h directly
 * leaves CONFIG_CSH_* undefined. */
#include "csh.h"

/* Console handle from the interface, never from the driver's header. */
extern ARM_DRIVER_USART Driver_USART_Console;

/* Post-mortem and debug entry points. its_dump_cmd lives next to the ITS
 * driver (port/aarch64/itsdump.c); the console rebind lives in the UART
 * driver and is how a wrong-INTID hypothesis gets tested at runtime
 * instead of with a rebuild. All declared here, like the console handle
 * above, rather than reaching into driver headers. board_console_break_hook
 * is the gdb stub's seam - see the RX path below. */
extern int its_dump_cmd(int argc, char **argv);
extern int uart_console_irq_rebind(unsigned int intid);
extern unsigned int uart_console_irq_id(void);

/* the system heap (port/adapters/threadx/heap.c) */
extern size_t xPortGetFreeHeapSize(void);
extern void wlan_heap_census(void);
extern int uart_console_rx_down(void);
extern int uart_console_rx_kick(void);
/* The gdb probe bodies (app/dbg_scenario.c), via externs like everything
 * else in this block - app/ headers are not adapter surface. */
extern void dbg_probe_breakpoint(void);
extern volatile unsigned long dbg_probe_word;
/* D41 window probe: GIC/console state at a named point of this bring-up. */
extern void uart_console_window_probe(const char *tag);

/* Freestanding: minilibc.c provides the definition. */
extern int atoi(const char *s);

/* --- console wiring ------------------------------------------------------- */

/* RX ring: 512 bytes, which is well past the UART's own FIFO and gives a
 * person typing faster than the shell can echo plenty of slack. Power of two
 * because CherryRB requires it. */
static uint8_t rx_ring_storage[512];
static chry_ringbuffer_t rx_ring;

/* The shell task's handle, set once the task exists. The ISR needs it to set
 * the wake flag, and reading a handle that may still be NULL is the one hazard
 * on this path - so it is checked, and a byte that arrives before the task
 * exists is simply left in the ring for the first poll. */
static osThreadId_t shell_task_id;

/* Flag bit meaning "there is input waiting". One bit is enough: the ring is the
 * real buffer, this only says "come and look". */
#define SHELL_INPUT_FLAG	0x1U

/* How often the shell task wakes anyway, in ticks (= ms at 1000 Hz): the
 * storm-defence poll. Cost when RX is healthy is one timed wait per half
 * second. */
#define SHELL_RX_POLL_MS	500U

/* Number of bytes the driver should hand us per reception. Arming the FIFO's
 * full depth (64) means a burst that arrives as one FIFO fill is handed over
 * in one completion - arming 1 and re-arming per byte made the driver's
 * re-arm drain discard every byte queued behind the first, which is how a
 * pasted line once reached the shell as "p". Echo latency stays fine: the
 * driver completes the reception as soon as the FIFO goes idle, i.e. within
 * a couple of character times of the last byte. */
#define SHELL_RX_CHUNK		64U

static uint8_t rx_chunk[SHELL_RX_CHUNK];

static void shell_rearm_rx(void)
{
	(void)Driver_USART_Console.Receive(rx_chunk, SHELL_RX_CHUNK);
}

/* Forensics counters for the console-input chain (avmon prints them):
 * event_calls/event_bytes sit at the USART callback -> ring end,
 * read_bytes at the ring -> readline end.  A byte counted in the uart
 * driver's counters but not here died before the ring; one counted here
 * but never echoed died in the readline/echo half. */
volatile unsigned shell_task_wakes;
volatile unsigned shell_event_calls;
volatile unsigned shell_event_bytes;
volatile unsigned shell_read_bytes;

/* ARM_USART_SignalEvent_t: called from the RX interrupt. */
static void shell_usart_event(uint32_t event)
{
	if ((event & ARM_USART_EVENT_RECEIVE_COMPLETE) == 0U) {
		return;
	}
	shell_event_calls++;

		/* Pull whatever arrived into the ring. The driver hands us one byte per
		 * reception, but reading the count it actually stored is more robust
		 * than assuming, and costs nothing. */
		{
			uint32_t got = Driver_USART_Console.GetRxCount();

			shell_event_bytes += got;

			for (uint32_t i = 0; i < got; i++) {
				/* 0x03 is the gdb stub's break-in byte (board_console_break_hook,
				 * weak here meaning "no stub"; strong in tx_gdb_glue.c). Consumed
				 * there, delivered to readline here - where it just cancels the
				 * current line, its ordinary meaning. */
				if (rx_chunk[i] == 0x03U && board_console_break_hook()) {
					continue;
				}
				/* Overwrite rather than drop: if a person pastes faster than
				 * the shell drains, losing the OLDEST bytes keeps the most
				 * recent input, which is what someone typing wants. The
				 * non-overwriting variant would silently discard what they
				 * just typed. */
				(void)chry_ringbuffer_overwrite_byte(&rx_ring, rx_chunk[i]);
			}
		}

	/* Order the ring writes before the wake-up. CherryRB has no barriers of
	 * its own (see the file header), so without this the consumer can be
	 * woken and read stale ring state - the classic single-producer failure
	 * that shows up as occasional dropped characters. */
	__asm__ __volatile__("dsb sy" ::: "memory");

	if (shell_task_id != NULL) {
		(void)osThreadFlagsSetFromISR(shell_task_id, SHELL_INPUT_FLAG);
	}

	/* Re-arm for the next byte. Must happen after the ring write, or a byte
	 * arriving during the copy could be overwritten before we read it. */
	shell_rearm_rx();
}

/* CherrySH output callback. Blocking Send is honest for this UART and keeps
 * command output from being interleaved by another task mid-line. */
static uint16_t shell_sput(chry_readline_t *rl, const void *data, uint16_t size)
{
	(void)rl;
	if (data == NULL || size == 0U) {
		return 0U;
	}
	(void)Driver_USART_Console.Send(data, (uint32_t)size);
	return size;
}

/* CherrySH input callback. Returns the number of bytes copied, and 0 when
 * nothing is available - the "no data right now" answer that readline's
 * noblock mode is built around. It must NOT wait here. */
static uint16_t shell_sget(chry_readline_t *rl, void *data, uint16_t size)
{
	uint32_t got;

	(void)rl;
	if (data == NULL || size == 0U) {
		return 0U;
	}

	got = chry_ringbuffer_read(&rx_ring, data, size);
	shell_read_bytes += got;
	return (uint16_t)got;
}

/* --- shell task ----------------------------------------------------------- */

static chry_shell_t shell;

/* 1KiB of history, 1KiB of line buffer: enough to scroll back through a session
 * of bring-up commands without being worth measuring. */
static char shell_history[1024];
static char shell_prompt_buf[64];
static char shell_line_buf[CONFIG_CSH_LNBUFF_SIZE];


static void shell_task(void *argument)
{
	static uint8_t rearm_reported;
	(void)argument;

	for (;;) {
		/* Block until the RX interrupt says there is something to read.
		 * This is the whole reason the shell does not starve the system:
		 * while idle it is genuinely asleep, not spinning. The timeout
		 * exists only for the storm-defence poll below - when RX is
		 * healthy this wake costs one flag call and goes straight back
		 * to sleep. */
		(void)osThreadFlagsWait(SHELL_INPUT_FLAG, osFlagsWaitAny,
					SHELL_RX_POLL_MS);
		shell_task_wakes++;

		/* Auto-recovery for the uart driver's storm defence: when the
		 * heuristic has armed RX off (real line-status storm, not the
		 * benign no-pending re-entries), re-arm from here instead of
		 * waiting for a human to type `uartint` on a console that
		 * cannot hear them. The driver counts bad hits afresh, so a
		 * line that is STILL storming re-disarms after one more hit -
		 * bounded flipping, each episode logged. */
		if (uart_console_rx_down()) {
			if (rearm_reported == 0U) {
				rearm_reported = 1U;
				board_log("shell: console RX in storm defence,"
					  " re-arming");
			}
			(void)uart_console_rx_kick();
		} else {
			rearm_reported = 0U;
		}

		/* With noblock enabled this returns 1 immediately when no complete
		 * line has been assembled yet, so a single byte of input costs one
		 * cheap call and the task goes back to sleep. Input that arrived
		 * during the call re-sets the flag, so nothing is missed. */
		(void)chry_shell_task_repl(&shell);

		/* Loop rather than block again straight away: if the flag was set
		 * more than once while we were working, osThreadFlagsWait would
		 * consume the latch and we would sleep with input still queued. */
	}
}

/* --- commands ------------------------------------------------------------- */

/* Section bounds provided by the link script. CherrySH finds its commands by
 * walking these, which is why they must be KEEP()ed there. (The netutils
 * telnetd re-declares these for its second shell instance - anonymous
 * typedefs cannot be forward-declared in the shared header.) */
extern const chry_syscall_t __fsymtab_start;
extern const chry_syscall_t __fsymtab_end;
extern const chry_sysvar_t __vsymtab_start;
extern const chry_sysvar_t __vsymtab_end;

/* CSH_FROM_ARGV (CherrySH hands the shell pointer over as argv[argc+1]) is
 * defined in cherrysh_adapter.h, because other adapters export commands into
 * the same table and need the same convention. */

static int cmd_version(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	const char *kernel = "Eclipse ThreadX 6.5.1 (cortex_a55 UP)";
	(void)argc;
	(void)argv;
	csh_printf(csh,
		   "freewebcamera\r\n"
		   "  repo      freewebcamera (BSD-2)\r\n"
		   "  target    RK3568 E4AP5G1-ITX, cortex-a55\r\n"
		   "  kernel    %s\r\n"
		   "  api       CMSIS-RTOS2\r\n",
		   kernel);
	return 0;
}

/* Uptime from TWO clocks, deliberately: CNTVCT (board_uptime_parts - the
 * same clock the "[   s.mmm]" log stamps use) and the kernel tick. The boot
 * log and the shell therefore always agree on "when", and a divergence
 * between the two numbers is direct evidence of a dead or racing tick. */
static int cmd_uptime(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint32_t ticks = osKernelGetTickCount();
	uint32_t hz = osKernelGetTickFreq();
	uint32_t usec = 0U;
	uint32_t ums = 0U;

	board_uptime_parts(&usec, &ums);
	csh_printf(csh,
		   "uptime: %u.%03u s (cntvct); tick %u at %u Hz = %u.%03u s\r\n",
		   (unsigned)usec, (unsigned)ums,
		   (unsigned)ticks, (unsigned)hz,
		   (unsigned)((hz != 0U) ? (ticks / hz) : 0U),
		   (unsigned)((hz != 0U) ? ((ticks % hz) * 1000U / hz) : 0U));
	return 0;
}

static int cmd_tick(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint32_t t0 = osKernelGetTickCount();
	uint32_t t1;

	/* Sleep a known interval and measure the tick delta. If the tick were not
	 * running, this would hang - which is itself the answer. */
	osDelay(100U);
	t1 = osKernelGetTickCount();
	csh_printf(csh, "tick advanced %u in 100 ms of osDelay\r\n",
		   (unsigned)(t1 - t0));
	if ((t1 - t0) == 0U) {
		csh_printf(csh, "TICK NOT RUNNING\r\n");
		return -1;
	}
	return 0;
}

/* Exercise the CMSIS-RTOS2 seam from the shell, so it can be re-checked without
 * a reboot. */
static int cmd_rtos(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	osSemaphoreId_t sem = osSemaphoreNew(1U, 0U, NULL);
	osMessageQueueId_t mq = osMessageQueueNew(2U, sizeof(uint32_t), NULL);
	osMutexId_t mtx = osMutexNew(NULL);
	uint32_t v = 0x5a5a5a5aU;
	uint32_t out = 0U;
	int32_t rc = 0;

	if (sem == NULL || mq == NULL || mtx == NULL) {
		csh_printf(csh, "rtos: object creation failed\r\n");
		rc = -1;
		goto done;
	}
	if (osSemaphoreRelease(sem) != osOK ||
	    osSemaphoreAcquire(sem, 10U) != osOK) {
		csh_printf(csh, "rtos: semaphore round trip failed\r\n");
		rc = -1;
		goto done;
	}
	if (osMessageQueuePut(mq, &v, 0U, 0U) != osOK ||
	    osMessageQueueGet(mq, &out, NULL, 0U) != osOK ||
	    out != v) {
		csh_printf(csh, "rtos: queue round trip failed\r\n");
		rc = -1;
		goto done;
	}
	if (osMutexAcquire(mtx, 10U) != osOK || osMutexRelease(mtx) != osOK) {
		csh_printf(csh, "rtos: mutex round trip failed\r\n");
		rc = -1;
		goto done;
	}
	csh_printf(csh, "rtos: semaphore/queue/mutex OK\r\n");

done:
	if (sem != NULL) {
		(void)osSemaphoreDelete(sem);
	}
	if (mq != NULL) {
		(void)osMessageQueueDelete(mq);
	}
	if (mtx != NULL) {
		(void)osMutexDelete(mtx);
	}
	return rc;
}

/* The self-test ladder from the ITS module. Reporting which rung failed is the
 * point (see port/board/gicv3_its.c). */
static int cmd_its(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint32_t delivered = 0U;

	csh_printf(csh, "its: running the ladder...\r\n");
	if (its_selftest(&delivered) != 0) {
		csh_printf(csh, "its: FAIL (see the boot log for the rung)\r\n");
		return -1;
	}
	csh_printf(csh, "its: LPI OK, %u deliveries observed\r\n",
		   (unsigned)delivered);
	return 0;
}

/* Post-mortem dump of the ITS delivery state: registers, DTE/ITE of every
 * attached device, prop/pend of the LPI window. Extra arguments are
 * arbitrary device ids to look up anyway. This is the tool that turns
 * "LPIs are silently dropped" into which table is wrong. */
static int cmd_itsdump(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	csh_printf(csh, "itsdump:\r\n");
	(void)its_dump_cmd(argc, argv);
	return 0;
}

/* Move the console RX interrupt to another INTID without a rebuild: the
 * one board fact this driver cannot establish by itself. */
static int cmd_uartint(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	if (argc < 2) {
		csh_printf(csh, "uartint: console RX on INTID %u"
			   " (usage: uartint <intid>)\r\n",
			   uart_console_irq_id());
		return 0;
	}

	{
		int intid = atoi(argv[1]);

		if ((intid <= 0) || (uart_console_irq_rebind((unsigned int)intid) != 0)) {
			csh_printf(csh, "uartint: rebind to %d failed\r\n",
				   intid);
			return -1;
		}
	}
	csh_printf(csh, "uartint: console RX moved to INTID %u - type"
		   " to test\r\n",
		   uart_console_irq_id());
	return 0;
}

/* Interrupt-path snapshot on the calling core: PMR (this core's priority
 * mask - a value parked below the console priority starves exactly those
 * lines while tick/SGI keep delivering: the "anchors green, shell deaf"
 * split), RPR (the running priority - non-idle means the GIC still sees a
 * claimed interrupt), and the console RX state. This is the discriminator
 * between "the line is masked" and "the kernel never ran the handler". */
static int cmd_gicdiag(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint32_t pmr = 0U;
	uint32_t rpr = 0U;

	board_gicv3_diag(&pmr, &rpr);
	csh_printf(csh,
		   "gicdiag: PMR=%02x RPR=%02x console-intid=%u rx-down=%d\r\n",
		   (unsigned)pmr, (unsigned)rpr,
		   (unsigned)BOARD_CONSOLE_INTID,
		   uart_console_rx_down());
	return 0;
}

/* gdb-lane entry (app/dbg_scenario.c): the BRK enters a stub session
 * stopped at the probe - the Z0 acceptance stop; `continue` comes back and
 * prints the evidence word. Also the no-rebuild way to answer "is the stub
 * alive" from the shell alone. */
static int cmd_dbg(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	(void)argc;
	(void)argv;
	csh_printf(csh, "dbg: BRK - gdb 'continue' resumes\r\n");
	dbg_probe_breakpoint();
	csh_printf(csh, "dbg: probe word = 0x%lx\r\n", dbg_probe_word);
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_dbg, dbg, "dbg",
			  "enter the gdb stub via the breakpoint probe");

/* System heap census. The networking world (iwm's 256 x 4.4 KB RX ring,
 * the USB pools) lives in the same 4 MB region, and it is the allocations
 * that are still live that explain an "out of memory", not the free count:
 * the census lists every live block >= 2 KB with its first words, which is
 * enough to recognize a leaked mbuf. */
static int cmd_heap(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	(void)argc;
	(void)argv;
	csh_printf(csh, "heap free=%lu B\r\n",
		   (unsigned long) xPortGetFreeHeapSize());
	wlan_heap_census();
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_heap, heap, "heap",
			  "system heap: free bytes + live blocks >= 2 KB");

CSH_CMD_EXPORT_ALIAS_FULL(cmd_version, version, "version",
			  "print build and target info");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_uptime, uptime, "uptime",
			  "show time since boot");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_tick, tick, "tick",
			  "check the RTOS tick is advancing");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_rtos, rtos, "rtos",
			  "round-trip CMSIS-RTOS2 primitives");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_its, its, "its",
			  "run the ITS/LPI self-test ladder");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_itsdump, itsdump, "itsdump",
			  "dump ITS/LPI delivery state");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_uartint, uartint, "uartint",
			  "show/move console RX INTID");
	CSH_CMD_EXPORT_ALIAS_FULL(cmd_gicdiag, gicdiag, "gicdiag",
				  "interrupt-path snapshot: PMR/RPR/console state");

#ifdef KTEST_BUILD
/* Kernel-test image only: the suite is not linked into the main image, so
 * neither the command nor the support function behind it may exist there.
 * Body in port/adapters/freertos/tests/ktest_support.c. */
extern void ktest_report(void);

static int cmd_ktest(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	(void)csh;
	ktest_report();
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_ktest, ktest, "ktest",
			  "kernel test suite: live per-suite status");
#endif

/* cherrysh resolves every command name against a PATH variable from the
 * variable table; with an empty variable table PATH is NULL and every
 * command reports "not found" no matter how correctly it was exported.
 * Register "/bin" - the section all the exports above land in - as a
 * read-only variable, through the macro so it lands in VSymTab where the
 * linker script's __vsymtab bounds expect it. */
static const char csh_path_value[] = "/bin";
CSH_RVAR_EXPORT(csh_path_value, PATH, sizeof(csh_path_value));

/* --- bring-up ------------------------------------------------------------- */

static int shell_running;

int shell_is_running(void)
{
	return shell_running;
}

/* Implements include/shell.h. Named shell_start() there because that is what the
 * application is doing; the CherrySH-specific name lives in the adapter header. */
int cherrysh_init(void)
{
	chry_shell_init_t init = { 0 };
	osThreadId_t id;

	/* RX ring first: the interrupt can fire as soon as it is enabled, and it
	 * writes into this. */
	if (chry_ringbuffer_init(&rx_ring, rx_ring_storage,
				 sizeof(rx_ring_storage)) != 0) {
		return -1;
	}

	/* Register our completion callback, then bring the console up.
	 *
	 * ORDER MATTERS, and getting it wrong is silent. CMSIS sequences a
	 * driver as Initialize() -> PowerControl(FULL). Initialize() resets the
	 * driver's power state to OFF, and Send() refuses to transmit unless the
	 * state is FULL - so calling Initialize() on an already-powered driver
	 * (board_main did power it up earlier) switches the console off. Output
	 * simply stops, with no error anywhere, which is how this was found:
	 * markers around this call printed before it and not after.
	 *
	 * PowerControl(FULL) after Initialize() restores the state and is what
	 * installs the console interrupt. */
	uart_console_window_probe("w0-pre-init");
	(void)Driver_USART_Console.Initialize(shell_usart_event);
	if (Driver_USART_Console.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
		return -1;
	}
	uart_console_window_probe("w1-post-pwr");

	/* Install the console callbacks and the command tables. */
	init.sput = shell_sput;
	init.sget = shell_sget;
	init.command_table_beg = &__fsymtab_start;
	init.command_table_end = &__fsymtab_end;
	init.variable_table_beg = &__vsymtab_start;
	init.variable_table_end = &__vsymtab_end;
	init.prompt_buffer = shell_prompt_buf;
	init.prompt_buffer_size = sizeof(shell_prompt_buf);
	init.history_buffer = shell_history;
	init.history_buffer_size = sizeof(shell_history);
	init.line_buffer = shell_line_buf;
	init.line_buffer_size = sizeof(shell_line_buf);
	init.host = "rk3568";
	init.user[0] = "root";

	if (chry_shell_init(&shell, &init) != 0) {
		return -1;
	}

	/* The shell task: below the drivers' own work but above idle. It is
	 * blocked almost all the time, so its priority barely matters - what
	 * matters is that it can be preempted. */
	id = osThreadNew(shell_task, NULL,
			 &(osThreadAttr_t){ .name = "shell",
					    .stack_size = 4096,
					    .priority = osPriorityNormal });
	if (id == NULL) {
		return -1;
	}
	shell_task_id = id;
	shell_running = 1;
	uart_console_window_probe("w2-post-csh");

	/* Start receiving LAST, and only now.
	 *
	 * Everything above has to be in place first: the driver's ISR drains the
	 * FIFO and calls our callback, which writes into the ring and wakes the
	 * task. Enabling the interrupt before the task exists would have that
	 * callback run with nothing to wake.
	 *
	 * Receive() goes BEFORE the RX control code, on purpose: a CMSIS
	 * reception fills the buffer Receive() was given, and the completion
	 * callback reads that same buffer (rx_chunk). With the order reversed,
	 * the boot arm filled the driver's internal scratch instead - the count
	 * said 5, the bytes were somewhere else, and the first line a person
	 * typed after every boot silently vanished (board-proven 2026-09-21 on
	 * both kernels; the D42 session only looked interactive because its
	 * first command was sacrificed the same way). Arming the client buffer
	 * first is what makes the Control(RX,1) arm below resume it, so the
	 * very first keystroke already lands where the callback reads.
	 *
	 * The chain is: IRQ -> driver drains FIFO into rx_chunk ->
	 * shell_usart_event() (us) -> ring + wake the shell task. */
	shell_rearm_rx();
	uart_console_window_probe("w3-post-recv");
	if (Driver_USART_Console.Control(ARM_USART_CONTROL_RX, 1U) != ARM_DRIVER_OK) {
		return -1;
	}

	return 0;
}

int shell_start(void)
{
	return cherrysh_init();
}
