/*
 * @file   smp.c
 * @brief  SMP bring-up: PSCI secondary-core release, per-core report flags.
 *
 * One job, done the only way this board allows: release cores 1..3 from
 * BL31's park loop with PSCI CPU_ON and wait until each has run its own GIC
 * bring-up and reported in.
 *
 * Why PSCI and nothing else: the secondary cores sit inside BL31 (the same
 * resident firmware that owns PPI 29 as a Group 0 interrupt), so a spin-table
 * release address cannot reach them. PSCI is also what the shipped boot chain
 * and mainline Linux both use on this SoC (enable-method = "psci"), which
 * makes the SMC conduit (smc #0 from non-secure EL1 straight into BL31) the
 * board-proven path rather than an inference.
 *
 * Coherency, and why there is no cache flush before CPU_ON: the page tables
 * the secondary will walk are built by board_mmu_enable() while the boot
 * core's caches are still off (startup.S clears SCTLR first), so they are
 * already in DRAM. The image itself arrived by DMA. Everything the cores
 * share after their MMUs come on is inner-shareable Normal memory inside one
 * DynamIQ cluster, which the hardware keeps coherent.
 */

#include <stdint.h>

#include "board.h"

/* PSCI 1.x function ids (SMC calling convention, ARM DEN 0022). VERSION is
 * SMC32-only; CPU_ON exists in both widths and the SMC64 form covers every
 * address this image can produce. */
#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_ON_AARCH64	0xC4000003UL

#define PSCI_SUCCESS		0L
#define PSCI_ALREADY_ON		(-4L)

/* Bounded wait for the up-reports. A core that never reports must surface as
 * a printed timeout, not a silent boot hang: the anchor line is what the
 * acceptance criteria read. */
#define SMP_UP_WAIT_LIMIT	50000000u

static volatile uint32_t core_up[BOARD_SMP_CORES];

/* SMCCC call through the SMC conduit. Four argument registers in, one result
 * out; BL31 clobbers nothing else the AAPCS holds live across the call. */
static long psci_call(uint64_t fid, uint64_t a0, uint64_t a1, uint64_t a2)
{
	register uint64_t r0 __asm__("x0") = fid;
	register uint64_t r1 __asm__("x1") = a0;
	register uint64_t r2 __asm__("x2") = a1;
	register uint64_t r3 __asm__("x3") = a2;

	__asm__ __volatile__("smc #0"
			     : "+r"(r0)
			     : "r"(r1), "r"(r2), "r"(r3)
			     : "memory", "cc");
	return (long)r0;
}

uint32_t board_smp_up_count(void)
{
	uint32_t count = 0u;
	uint32_t i;

	for (i = 0u; i < BOARD_SMP_CORES; i++) {
		count += __atomic_load_n(&core_up[i], __ATOMIC_ACQUIRE);
	}
	return count;
}

void board_smp_mark_core_up(uint32_t core)
{
	if (core < BOARD_SMP_CORES) {
		__atomic_store_n(&core_up[core], 1u, __ATOMIC_RELEASE);
		__asm__ __volatile__("dsb sy" ::: "memory");
		/* Wake the boot core's poll loop early. */
		__asm__ __volatile__("sev" ::: "memory");
	}
}

/* Returns the BCD-ish PSCI version (0x10001 = 1.1) or 0 when the conduit is
 * dead. Reported at boot: it is the cheapest possible probe of "is BL31
 * listening on SMC" before any core is released. */
uint32_t board_smp_psci_version(void)
{
	long ret = psci_call(PSCI_VERSION, 0ul, 0ul, 0ul);

	if (ret < 0l) {
		return 0u;
	}
	return (uint32_t)ret;
}

void board_smp_start_secondaries(void)
{
	extern char freertos_secondary_entry[];
	uint32_t core;
	uint32_t i;
	uint64_t entry = (uint64_t)(uintptr_t)freertos_secondary_entry;
	uint32_t psci_ver = board_smp_psci_version();

	for (core = 1u; core < BOARD_SMP_CORES; core++) {
		/* The target MPIDR is the Aff0 number: this SoC keeps all four
		 * cores in Aff0 of one cluster with the higher affinity fields
		 * at zero. Entry state per the PSCI spec: the firmware drops
		 * the core at the given address, interrupts masked, MMU and
		 * caches off - smp_secondary.S handles the rest. */
		long ret = psci_call(PSCI_CPU_ON_AARCH64,
				     (uint64_t)core, entry, 0ul);

		if (ret != PSCI_SUCCESS && ret != PSCI_ALREADY_ON) {
			board_log("smp: psci cpu_on core %u failed (%ld)",
				  (unsigned)core, ret);
		}
	}

	/* Wait for the secondaries' report-in, bounded. */
	for (i = 0u; i < SMP_UP_WAIT_LIMIT; i++) {
		if (board_smp_up_count() == (BOARD_SMP_CORES - 1u)) {
			break;
		}
		__asm__ __volatile__("wfe" ::: "memory");
	}

	board_log("smp: %u/%u cores up, psci %u.%u",
		  (unsigned)(board_smp_up_count() + 1u),
		  (unsigned)BOARD_SMP_CORES,
		  (unsigned)((psci_ver >> 16) & 0xffffu),
		  (unsigned)(psci_ver & 0xffffu));

	if (i == SMP_UP_WAIT_LIMIT) {
		board_early_print("smp: TIMEOUT waiting for secondary cores\n");
	}
}
