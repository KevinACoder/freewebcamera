/*
 * @file   reset.c
 * @brief  System reset through the board's PSCI conduit.
 *
 * The board discipline prefers a warm reset over a power cycle: the UStone
 * switch is the only cold path, and switching power is the slowest and most
 * wearing way back to U-Boot, so a running image should be able to put itself
 * back at the firmware prompt.  On this SoC that is PSCI SYSTEM_RESET - the
 * same SMC conduit smp.c already uses for CPU_ON, and the same path U-Boot's
 * own `reset` command takes (CONFIG_SYSRESET + sysreset_psci triggers
 * PSCI_0_2_FN_SYSTEM_RESET).
 *
 * Task-context callable (the shell's reboot command).  A reset that works
 * never returns; a return value therefore means the firmware REFUSED the
 * call, which is a real case on this board (too early in boot, or a firmware
 * that does not implement the function).  Callers print it and stop.
 *
 * The call runs with IRQs masked (daifset): between the SMC and the SoC
 * reset the CPU must not take an interrupt, or a console RX interrupt would
 * re-enter the shell adapter on a system that is already going down.  Console
 * draining is the caller's job - this layer has no kernel clock to wait on.
 *
 * @author zhugengyu
 * @date   28.09.2026
 */

#include <stdint.h>

#include "board.h"

/* PSCI 1.x function ids (SMC calling convention, ARM DEN 0022), same
 * encoding policy as smp.c: SYSTEM_RESET exists only in the SMC32 form. */
#define PSCI_SYSTEM_RESET	0x84000009UL

/* SMCCC call through the board's conduit (BOARD_PSCI_INSN: "smc #0" into
 * BL31 on this SoC).  One argument register in, one result out.  Kept
 * separate from smp.c's static helper on purpose - that file's CPU_ON
 * sequence is board-proven evidence and stays untouched - duplicated in the
 * same shape so the two cannot diverge in encoding. */
static long psci_call_reset(uint64_t fid)
{
	register uint64_t r0 __asm__("x0") = fid;

	__asm__ __volatile__("msr daifset, #2" ::: "memory");
	__asm__ __volatile__(BOARD_PSCI_INSN
			     : "+r"(r0)
			     :
			     : "memory", "cc");
	return (long)r0;
}

/* 0 on an accepted call (though the board is already resetting at that point
 * in the successful case), or the firmware's refusal: PSCI_NOT_SUPPORTED for
 * an unimplemented function. */
int board_system_reset(void)
{
	return (int)psci_call_reset(PSCI_SYSTEM_RESET);
}
