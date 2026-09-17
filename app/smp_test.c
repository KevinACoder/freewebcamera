/*
 * @file   smp_test.c
 * @brief  SMP acceptance: one task pinned per core, verified by hardware.
 *
 * The claim under test is not "the scheduler has affinity APIs" - it is
 * "a task pinned to core N executes on core N, and on no other core, while
 * three other tasks are pinned the same way on the neighbouring cores".
 * The only honest witness is the hardware: each task reads the core number
 * (board_smp_core_id, the one home of the MPIDR extraction) on every
 * iteration and counts any sample that does not match its bound core.
 *
 * Four tasks, same priority (with configRUN_MULTIPLE_PRIORITIES 0 only
 * equal-priority tasks run simultaneously, so all four genuinely overlap),
 * each pinned with attr->affinity_mask = 1 << i through the standard
 * CMSIS-RTOS2 attribute - no project-specific binding API exists, which is
 * the point of wiring affinity through osThreadNew.
 *
 * Completion is one event flag per task; the whole test is bounded by a
 * timeout so a wedged core surfaces as a FAIL with the observed state
 * instead of an eternal hang. Everything prints through the console (the
 * same sink the bring-up anchors use), and the shell command wrapper adds
 * the PASS/FAIL line.
 */

#include <stdint.h>

#include "board.h"
#include "cmsis_os2.h"

/* Iterations per task. Each iteration reads the hardware core number and
 * compares it against the bound core; every 32nd iteration yields, so the
 * test also exercises scheduler rotation while it runs. */
#define SMP_TEST_SAMPLES	20000U
#define SMP_TEST_YIELD_MASK	0x1FU

/* Bound on the whole test. The four tasks need well under a second each at
 * 1 ms granularity; the slack is for the storage/network tasks that share
 * the priority bands. */
#define SMP_TEST_TIMEOUT_MS	10000U

/* Per-core results, indexed by logical core number. Written only by the
 * task bound to that core; read by the shell task after the completion
 * flags give the happens-before. */
static uint32_t sample_count[BOARD_SMP_CORES];
static uint32_t mismatch_count[BOARD_SMP_CORES];
static uint32_t mismatch_mpidr[BOARD_SMP_CORES];

static inline uint32_t read_mpidr(void)
{
	uint64_t mpidr;

	__asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
	return (uint32_t)mpidr;
}

static void smp_test_task(void *argument)
{
	uint32_t core = (uint32_t)(uintptr_t)argument;
	uint32_t bad = 0U;
	uint32_t i;

	for (i = 0U; i < SMP_TEST_SAMPLES; i++) {
		uint32_t mpidr = read_mpidr();

		/* The logical number, extracted per the board's conf (RK3568
		 * carries it in MPIDR Aff1, so a raw `mpidr & 0xff` read Aff0
		 * - which is 0 for every core there - and misreported every
		 * task except core 0). */
		if (board_smp_core_id() != core) {
			bad++;
			mismatch_mpidr[core] = mpidr;
		}

		if ((i & SMP_TEST_YIELD_MASK) == 0U) {
			osDelay(1U);
		}
	}

	sample_count[core] = SMP_TEST_SAMPLES;
	__atomic_store_n(&mismatch_count[core], bad, __ATOMIC_RELEASE);

	(void)osThreadTerminate(osThreadGetId());
}

int smp_selftest(void)
{
	osEventFlagsId_t done;
	uint32_t wait_all = 0U;
	uint32_t core;
	uint32_t failures = 0U;

	done = osEventFlagsNew(NULL);
	if (done == NULL) {
		board_early_print("smp: cannot create completion flags\n");
		return -1;
	}

	for (core = 0U; core < BOARD_SMP_CORES; core++) {
		osThreadAttr_t attr = { 0 };

		sample_count[core] = 0U;
		mismatch_count[core] = 0U;
		mismatch_mpidr[core] = 0U;

		attr.name = (core == 0U) ? "smp0" : (core == 1U) ? "smp1"
			   : (core == 2U) ? "smp2" : "smp3";
		attr.stack_size = 1024U;
		attr.priority = osPriorityNormal;
		attr.affinity_mask = 1U << core;

		if (osThreadNew(smp_test_task, (void *)(uintptr_t)core,
				&attr) == NULL) {
			board_log("smp: task %u create FAILED", (unsigned)core);
			failures++;
		}
		wait_all |= (1U << core);
	}

	if (failures != 0U) {
		(void)osEventFlagsDelete(done);
		return -1;
	}

	{
		uint32_t flags = osEventFlagsWait(done, wait_all,
						  osFlagsWaitAll,
						  SMP_TEST_TIMEOUT_MS);

		(void)osEventFlagsDelete(done);

		if ((flags & 0x80000000U) != 0U) {
			/* Timed out (or the call failed): report how far the
			 * test got from the sample counters themselves. */
			uint32_t ran = 0U;
			uint32_t c;

			for (c = 0U; c < BOARD_SMP_CORES; c++) {
				ran += (sample_count[c] != 0U) ? 1U : 0U;
			}
			board_log("smp: TIMEOUT, %u/4 tasks reported",
				  (unsigned)ran);
			return -1;
		}
	}

	for (core = 0U; core < BOARD_SMP_CORES; core++) {
		uint32_t bad = __atomic_load_n(&mismatch_count[core],
					       __ATOMIC_ACQUIRE);

		board_log("smp: core%u bound 1<<%u samples %u wrong-core %u"
			  " (mpidr 0x%x)",
			  (unsigned)core, (unsigned)core,
			  (unsigned)sample_count[core], (unsigned)bad,
			  (unsigned)mismatch_mpidr[core]);

		if (bad != 0U || sample_count[core] != SMP_TEST_SAMPLES) {
			failures++;
		}
	}

	return (failures == 0U) ? 0 : -1;
}
