/*
 * @file   tx_vectors.S
 * @brief  ThreadX runtime vector table + the SPSel entry stubs.
 *
 * The ThreadX ARMv8-A SMP port runs EVERYTHING at EL1 with SPSel=0: threads,
 * the kernel, and ISRs share the one architecturally visible stack pointer
 * (SP_EL0), while the "system stack" is a software variable
 * (_tx_thread_system_stack_ptr) that _tx_thread_context_save switches to
 * before ISR processing. Two consequences shape this file:
 *
 *  - The board hands control over with SPSel=1 (startup.S / smp_secondary.S
 *    discipline, shared with the FreeRTOS image). The two stubs below switch
 *    to SPSel=0 while keeping sp pointing at the same physical stack - the
 *    boot stack slice this core already owns - before entering the kernel.
 *    From that point on nothing ever returns past the switch.
 *
 *  - The IRQ first-level handler must restore the interrupted context's
 *    stack view before saving the frame: `msr spsel, 0` - identical to the
 *    Arm example build's irqFirstLevelHandler, which defines the port's
 *    calling contract (x29/x30 pushed, then _tx_thread_context_save, the C
 *    dispatcher, _tx_thread_context_restore).
 *
 * Every synchronous exception is a fault: the port yields with a direct
 * branch (_tx_thread_system_return ends in B _tx_thread_schedule), never
 * with SVC, so there is no legitimate sync-exception user of this table.
 * FIQ belongs to the secure world (OP-TEE owns Group 0) and is expected
 * never to arrive here; it parks silently - printing could deadlock on the
 * console spinlock the FIQ just preempted.
 */

	.arch	armv8-a
	.section .vectors.threadx, "ax"
	.align	11

	.globl	_threadx_vector_table
	.type	_threadx_vector_table, %object
_threadx_vector_table:

	/* --- current EL with SP0 --- */
	b	tx_fault_park_sp0		/* 0x000 sync  */
	.balign 0x80
	b	tx_irq_first_level_handler	/* 0x080 irq   */
	.balign 0x80
	b	tx_fiq_park			/* 0x100 fiq   */
	.balign 0x80
	b	tx_fault_park_sp0		/* 0x180 serror*/

	/* --- current EL with SPx --- */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x200 sync  */
	.balign 0x80
	b	tx_irq_first_level_handler	/* 0x280 irq   */
	.balign 0x80
	b	tx_fiq_park			/* 0x300 fiq   */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x380 serror*/

	/* --- lower EL, AArch64 --- */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x400 sync  */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x480 irq   */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x500 fiq   */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x580 serror*/

	/* --- lower EL, AArch32 --- */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x600 sync  */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x680 irq   */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x700 fiq   */
	.balign 0x80
	b	tx_fault_park_spx		/* 0x780 serror*/

	.size	_threadx_vector_table, . - _threadx_vector_table

	.text
	.align	4

/* IRQ: the port's contract, mirrored from the Arm example build. Interrupts
 * are masked by exception entry. The x29/x30 push is part of the frame
 * contract - _tx_thread_context_save saves x0..x19 itself and its idle path
 * pops this push along with its own stores. */
	.globl	tx_irq_first_level_handler
	.type	tx_irq_first_level_handler, %function
tx_irq_first_level_handler:
	msr	spsel, 0
	stp	x29, x30, [sp, #-16]!
	bl	_tx_thread_context_save
	bl	tx_irq_handler
	b	_tx_thread_context_restore
	.size	tx_irq_first_level_handler, . - tx_irq_first_level_handler

/* Fault parks: report through the polled early UART and spin. The frame
 * kind names the vector entry that caught it. SP0/SPx differ only in the
 * label - either way the kernel state is unrecoverable. */
tx_fault_park_sp0:
	mov	x0, #1
	b	tx_fault_park

tx_fault_park_spx:
	mov	x0, #2
	b	tx_fault_park

tx_fiq_park:
	/* No print: a FIQ here preempted the console spinlock holder, and
	 * uart_early_puts would spin forever on it. A silent hang IS the
	 * diagnostic - it means the secure world routed a FIQ at us. */
1:	wfe
	b	1b

	.text

/* Entry stubs - see the file comment for the SPSel story. */

/* Core 0: called by osKernelStart() after the secondaries are released.
 * Keeps the current stack (the 64 KiB boot region top, __stack_top) and
 * hands it to the kernel as SP_EL0. tx_kernel_enter never returns. The
 * branch targets _tx_initialize_kernel_enter directly: from assembly there
 * is no tx_api.h to expand the tx_kernel_enter macro. */
	.globl	tx_kernel_enter_spsel0
	.type	tx_kernel_enter_spsel0, %function
tx_kernel_enter_spsel0:
	mov	x0, sp
	msr	spsel, 0
	mov	sp, x0
	b	_tx_initialize_kernel_enter
	.size	tx_kernel_enter_spsel0, . - tx_kernel_enter_spsel0

/* Secondary hand-off: smp_secondary.S calls kernel_secondary_main with
 * SPSel=1 and this core's 16 KiB boot-stack slice in sp. Switch the view,
 * then let C do GIC bring-up / report-in and park forever inside
 * _tx_thread_smp_initialize_wait. */
	.globl	kernel_secondary_main
	.type	kernel_secondary_main, %function
kernel_secondary_main:
	mov	x0, sp
	msr	spsel, 0
	mov	sp, x0
	b	tx_secondary_main
	.size	kernel_secondary_main, . - kernel_secondary_main
