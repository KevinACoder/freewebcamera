/*
 * @file   startup.S
 * @brief  Boot entry: normalise to EL1, install vectors, clear BSS, enter C.
 *
 * U-Boot's `go` may hand over at EL3, EL2 or EL1. The kernel port requires
 * EL1 (it is built with -DGUEST, which makes port.c assert CurrentEL == EL1),
 * so every path converges on the same EL1 landing point.
 *
 * Two board facts drive the EL2 sequence, both established on this hardware:
 *
 *  - The loader arrives with the EL2 MMU and caches possibly still enabled.
 *    SCTLR_EL2 is cleared before anything else so subsequent table walks and
 *    cache operations behave predictably.
 *
 *  - CNTHCTL_EL2.EL1PCEN / EL1PCTEN must be set (bits 0 and 1) or EL1 cannot
 *    reach the physical counter and every access traps into the old EL2
 *    vector table. Note the tick does NOT rely on this: it uses the virtual
 *    timer CNTV precisely because PPI26 (the EL2 physical timer) is unusable
 *    here. The bits are set anyway so counter reads behave from EL1.
 *
 * CurrentEL encodes the level as 0x4 / 0x8 / 0xC (not 1 / 2 / 3) - this is a
 * classic source of misrouted boot paths.
 *
 * .S files run the preprocessor, so board_conf.h (data-only #defines) is
 * includable directly - the same source of truth the C files use.
 */

#include "board_conf.h"

	.arch	armv8-a
	.section .text.boot, "ax"

	.globl	_start
	.type	_start, %function
_start:
	/* Mask everything until EL1 is set up. */
	msr	daifset, #0xf

	/* This path is boot-core-only. A secondary running here would clear
	 * the BSS under the running kernel and re-derive state that must only
	 * exist once - the kind of failure that looks like random memory
	 * corruption, not a wrong turn. Secondaries enter through PSCI at
	 * kernel_secondary_entry (smp_secondary.S); if one lands here
	 * anyway, park it where it cannot do harm. The logical core number
	 * comes from BOARD_MPIDR_CORE_SHIFT (board_conf.h: Aff1 on RK3568,
	 * Aff0 on QEMU virt). */
	mrs	x1, mpidr_el1
	lsr	x1, x1, #BOARD_MPIDR_CORE_SHIFT
	and	x1, x1, #0xff
	cbz	x1, 1f
2:	wfe
	b	2b
1:

	mrs	x0, CurrentEL
	and	x0, x0, #0xc
	cmp	x0, #0xc			/* EL3 */
	b.eq	from_el3
	cmp	x0, #0x8			/* EL2 */
	b.eq	from_el2
	b	el1_entry			/* already EL1 */

from_el3:
	/* Give EL2 a clean AArch64 world and hand over to the EL2 path. */
	mov	x0, #0
	msr	sctlr_el2, x0
	msr	hcr_el2, xzr
	mov	x0, #0x30			/* SCR_EL3: NS | IRQ | FIQ */
	msr	scr_el3, x0
	adr	x1, from_el2
	msr	elr_el3, x1
	mov	x0, #0x3c9			/* SPSR_EL3: EL2h, DAIF masked */
	msr	spsr_el3, x0
	isb
	eret

from_el2:
	/* Stack: EL2 has its own SP_EL2; set it before any helper call. */
	ldr	x0, =__stack_top
	mov	sp, x0

	/* The loader may leave the EL2 MMU and caches on; turn them off
	 * before touching memory that was mapped behind our back. */
	dsb	sy
	msr	sctlr_el2, xzr
	isb

	/* Let EL1 run AArch64, reach the counters, and take no traps. */
	mov	x0, #0x80000000			/* HCR_EL2.RW = 1 */
	msr	hcr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3			/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x0
	msr	cptr_el2, xzr			/* no FP/SIMD trapping */
	msr	hstr_el2, xzr
	msr	vttbr_el2, xzr

	adr	x1, el1_entry
	msr	elr_el2, x1
	mov	x0, #0x3c5			/* SPSR_EL2: EL1h, DAIF masked */
	msr	spsr_el2, x0
	isb
	eret

el1_entry:
	/* Use SP_EL1 (the loader may hand over with SPSEL=0) and clear SCTLR
	 * so MMU and caches are off regardless of what the loader left. */
	msr	spsel, #1
	msr	sctlr_el1, xzr
	ic	iallu
	dsb	sy
	isb

	/* Enable FP/SIMD access so an accidental use traps cleanly rather
	 * than being silently UNDEFINED. The build is -mgeneral-regs-only and
	 * the port saves no FP state, so no FP instructions should appear. */
	mrs	x0, cpacr_el1
	orr	x0, x0, #(3 << 20)
	msr	cpacr_el1, x0

	/* Boot vector table (the kernel swaps in the FreeRTOS one later). */
	adr	x0, boot_vectors
	msr	vbar_el1, x0
	isb

	ldr	x0, =__stack_top
	mov	sp, x0

	/* Clear BSS: the C runtime relies on zero-initialised statics. */
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:

	/* Identity-mapped MMU with caches on. Needed before C code runs:
	 * unaligned Normal access must work, and the ITS table
	 * flush/invalidate discipline requires a real data cache. */
	bl	board_mmu_enable

	bl	board_main

3:	wfe
	b	3b
	.size	_start, . - _start

/*
 * Boot vector table. Present so a fault taken before the scheduler starts
 * lands somewhere diagnosable instead of being ignored. Each entry reports
 * the exception kind through the console, then parks.
 *
 * The running kernel installs its own table (_freertos_vector_table) when the
 * scheduler starts; this one only covers the window before that.
 */
	.section .vectors, "ax"
	.align	11
	.globl	boot_vectors
boot_vectors:
	/* Current EL with SP_EL0 */
	.p2align 7
	b	boot_sync_fault
	.p2align 7
	b	boot_irq_fault
	.p2align 7
	b	boot_fiq_fault
	.p2align 7
	b	boot_serr_fault
	/* Current EL with SP_ELx */
	.p2align 7
	b	boot_sync_fault
	.p2align 7
	b	boot_irq_fault
	.p2align 7
	b	boot_fiq_fault
	.p2align 7
	b	boot_serr_fault
	/* Lower EL, AArch64 */
	.p2align 7
	b	boot_sync_fault
	.p2align 7
	b	boot_irq_fault
	.p2align 7
	b	boot_fiq_fault
	.p2align 7
	b	boot_serr_fault
	/* Lower EL, AArch32 */
	.p2align 7
	b	boot_sync_fault
	.p2align 7
	b	boot_irq_fault
	.p2align 7
	b	boot_fiq_fault
	.p2align 7
	b	boot_serr_fault

/* Each fault path prints a distinguishable marker, then parks. The console
 * is polled and needs no interrupt, so it is usable this early.
 *
 * The synchronous path additionally reports ESR_EL1 and FAR_EL1: during SMP
 * bring-up a sync fault could come from any core and several candidate
 * causes, and the exception class + faulting address are what turn a coin
 * toss into a diagnosis. (uart_early_panic re-programs the UART first, so
 * these prints work even when the fault precedes console init.) */
	.type	boot_sync_fault, %function
boot_sync_fault:
	ldr	x0, =msg_sync
	bl	uart_early_panic
	ldr	x0, =msg_esr
	bl	uart_early_puts
	mrs	x0, esr_el1
	bl	uart_early_put_hex32
	ldr	x0, =msg_far
	bl	uart_early_puts
	mrs	x0, far_el1
	bl	uart_early_put_hex32
	ldr	x0, =msg_elr
	bl	uart_early_puts
	mrs	x0, elr_el1
	bl	uart_early_put_hex32
	ldr	x0, =msg_newline
	bl	uart_early_puts
	b	boot_fault_common
	.type	boot_irq_fault, %function
boot_irq_fault:
	ldr	x0, =msg_irq
	b	boot_fault_common
	.type	boot_fiq_fault, %function
boot_fiq_fault:
	ldr	x0, =msg_fiq
	b	boot_fault_common
	.type	boot_serr_fault, %function
boot_serr_fault:
	ldr	x0, =msg_serr
	b	boot_fault_common

boot_fault_common:
	/* Report, then park with interrupts masked. Deliberately does not try
	 * to recover: before the scheduler exists there is no context to
	 * return to. */
	bl	uart_early_panic
1:	wfe
	b	1b

	.section .rodata, "a"
	.globl	msg_sync
msg_sync:	.asciz	"\n[carrier] FAULT: synchronous exception\n"
	.globl	msg_irq
msg_irq:	.asciz	"\n[carrier] FAULT: IRQ before scheduler\n"
	.globl	msg_fiq
msg_fiq:	.asciz	"\n[carrier] FAULT: FIQ\n"
	.globl	msg_serr
msg_serr:	.asciz	"\n[carrier] FAULT: SError\n"
msg_esr:	.asciz	"[fault] ESR_EL1="
msg_far:	.asciz	" FAR_EL1="
msg_elr:	.asciz	" ELR_EL1="
msg_newline:	.asciz	"\n"
