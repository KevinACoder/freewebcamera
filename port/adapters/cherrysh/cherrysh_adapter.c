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
/* csh.h, not chry_shell.h: csh.h pulls in csh_config.h (our shadow config) and
 * then the shell itself, and it is what defines the CSH_CMD_EXPORT* macros and
 * the chry_syscall_t/chry_sysvar_t types. Including chry_shell.h directly
 * leaves CONFIG_CSH_* undefined. */
#include "csh.h"

/* Console handle from the interface, never from the driver's header. */
extern ARM_DRIVER_USART Driver_USART_Console;

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

/* Number of bytes the driver should hand us per reception. 1 byte keeps echo
 * latency at one character; the cost is an interrupt per character, which at
 * 115200 baud is at most ~11.5 kHz and well within what this core handles. */
#define SHELL_RX_CHUNK		1U

static uint8_t rx_chunk[SHELL_RX_CHUNK];

static void shell_rearm_rx(void)
{
	(void)Driver_USART_Console.Receive(rx_chunk, SHELL_RX_CHUNK);
}

/* ARM_USART_SignalEvent_t: called from the RX interrupt. */
static void shell_usart_event(uint32_t event)
{
	if ((event & ARM_USART_EVENT_RECEIVE_COMPLETE) == 0U) {
		return;
	}

	/* Pull whatever arrived into the ring. The driver hands us one byte per
	 * reception, but reading the count it actually stored is more robust
	 * than assuming, and costs nothing. */
	{
		uint32_t got = Driver_USART_Console.GetRxCount();

		for (uint32_t i = 0; i < got; i++) {
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
	return (uint16_t)got;
}

/* --- shell task ----------------------------------------------------------- */

static chry_shell_t shell;

/* 1KiB of history, 1KiB of line buffer: enough to scroll back through a session
 * of bring-up commands without being worth measuring. */
static char shell_history[1024];
static char shell_prompt_buf[64];
static char shell_line_buf[CONFIG_CSH_LNBUFF_SIZE];

/* Section bounds provided by the link script. CherrySH finds its commands by
 * walking these, which is why they must be KEEP()ed there. */
extern const chry_syscall_t __fsymtab_start;
extern const chry_syscall_t __fsymtab_end;
extern const chry_sysvar_t __vsymtab_start;
extern const chry_sysvar_t __vsymtab_end;

static void shell_task(void *argument)
{
	(void)argument;

	for (;;) {
		/* Block until the RX interrupt says there is something to read.
		 * This is the whole reason the shell does not starve the system:
		 * while idle it is genuinely asleep, not spinning. */
		(void)osThreadFlagsWait(SHELL_INPUT_FLAG, osFlagsWaitAny,
					osWaitForever);

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

/* CherrySH passes the shell pointer as argv[argc+1] by convention (see
 * builtin/help.c). These two helpers keep that detail in one place. */
#define CSH_FROM_ARGV(argc, argv) ((chry_shell_t *)(void *)(argv)[(argc) + 1])

static int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	csh_printf(CSH_FROM_ARGV(argc, argv),
		   "freewebcamera\r\n"
		   "  repo      freewebcamera (BSD-2)\r\n"
		   "  target    RK3568 E4AP5G1-ITX, cortex-a55\r\n"
		   "  kernel    FreeRTOS V11.3.1 (ARM_AARCH64_SRE)\r\n"
		   "  api       CMSIS-RTOS2\r\n");
	return 0;
}

/* Uptime straight from the kernel tick count, so it is also a live check that
 * the tick is still running. */
static int cmd_uptime(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint32_t ticks = osKernelGetTickCount();
	uint32_t hz = osKernelGetTickFreq();
	uint32_t secs = (hz != 0U) ? (ticks / hz) : 0U;

	csh_printf(csh, "uptime: %u.%03u s (%u ticks at %u Hz)\r\n",
		   (unsigned)secs,
		   (unsigned)((hz != 0U) ? ((ticks % hz) * 1000U / hz) : 0U),
		   (unsigned)ticks, (unsigned)hz);
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

CSH_CMD_EXPORT_FULL(cmd_version, "version", "print build and target info");
CSH_CMD_EXPORT_FULL(cmd_uptime, "uptime", "show time since boot");
CSH_CMD_EXPORT_FULL(cmd_tick, "tick", "check the RTOS tick is advancing");
CSH_CMD_EXPORT_FULL(cmd_rtos, "rtos", "round-trip CMSIS-RTOS2 primitives");
CSH_CMD_EXPORT_FULL(cmd_its, "its", "run the ITS/LPI self-test ladder");

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
	(void)Driver_USART_Console.Initialize(shell_usart_event);
	if (Driver_USART_Console.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
		return -1;
	}

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

	/* Start receiving LAST, and only now.
	 *
	 * Everything above has to be in place first: the driver's ISR drains the
	 * FIFO and calls our callback, which writes into the ring and wakes the
	 * task. Enabling the interrupt before the task exists would have that
	 * callback run with nothing to wake.
	 *
	 * Note this goes through the standard CMSIS control code rather than a
	 * driver-specific call: the adapter keeps working with any ARM_DRIVER_USART
	 * that implements ARM_USART_CONTROL_RX, and never needs the driver's
	 * header.
	 *
	 * The chain is: IRQ -> driver drains FIFO -> shell_usart_event() (us)
	 * -> ring + wake the shell task. */
	if (Driver_USART_Console.Control(ARM_USART_CONTROL_RX, 1U) != ARM_DRIVER_OK) {
		return -1;
	}

	return 0;
}

int shell_start(void)
{
	return cherrysh_init();
}
