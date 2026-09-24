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
#ifdef THREADX_UP_BUILD
	b	tx_sync_dispatch_sp0	/* 0x000 sync  */
#else
	b	tx_fault_park_sp0	/* 0x000 sync  */
#endif
	.balign 0x80
	b	tx_irq_first_level_handler	/* 0x080 irq   */
	.balign 0x80
	b	tx_fiq_park			/* 0x100 fiq   */
	.balign 0x80
	b	tx_fault_park_sp0		/* 0x180 serror*/

	/* --- current EL with SPx --- */
	.balign 0x80
#ifdef THREADX_UP_BUILD
	b	tx_sync_dispatch_spx	/* 0x200 sync  */
#else
	b	tx_fault_park_spx	/* 0x200 sync  */
#endif
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

#ifdef THREADX_UP_BUILD
/* Sync dispatch with the gdb stub tap (D56). The UP debug carrier has ONE
 * synchronous-exception sink: gdb_debug_interrupt receives the frame for
 * debug AND fault exceptions alike - the C side picks the signal, real
 * faults become host stops instead of fatal parks. The frame layout is
 * gdb_arch.h's struct gdb_trapframe: sp,lr,elr,spsr,esr,far,x[0..29]
 * (288 bytes).
 *
 * SPSel: the exception entry forced SPSel=1; the port's world runs on
 * SP_EL0, so switch back first - the frame then lands on the interrupted
 * context's own stack, same contract as the IRQ save path. With SPSel back
 * to 0 and before the frame is carved, sp IS the interrupted context's sp:
 * the stubs store it into frame[0] (the trapframe's sp slot) before it is
 * lost to the frame arithmetic. This holds because SPSR.M is EL1t here -
 * the invariant that nothing in this image ever runs with SPSel=1 (see the
 * file comment); an EL1h exception would have its real SP_EL1 beyond
 * reach, and is not a case this port can produce.
 *
 * Register budget of the stubs: x8/x9 are saved to their frame slots FIRST
 * so the sp scratch (x8) can clobber x8; the common body stores every
 * register EXCEPT x8/x9, which are already in. */
tx_sync_dispatch_sp0:
	msr	spsel, 0
	sub	sp, sp, #288
	stp	x8, x9, [sp, #112]
	add	x8, sp, #288
	str	x8, [sp, #0]
	b	tx_sync_dispatch

tx_sync_dispatch_spx:
	msr	spsel, 0
	sub	sp, sp, #288
	stp	x8, x9, [sp, #112]
	add	x8, sp, #288
	str	x8, [sp, #0]

tx_sync_dispatch:
	str	x30, [sp, #8]
	stp	x0, x1, [sp, #48]
	stp	x2, x3, [sp, #64]
	stp	x4, x5, [sp, #80]
	stp	x6, x7, [sp, #96]
	stp	x10, x11, [sp, #128]
	stp	x12, x13, [sp, #144]
	stp	x14, x15, [sp, #160]
	stp	x16, x17, [sp, #176]
	stp	x18, x19, [sp, #192]
	stp	x20, x21, [sp, #208]
	stp	x22, x23, [sp, #224]
	stp	x24, x25, [sp, #240]
	stp	x26, x27, [sp, #256]
	stp	x28, x29, [sp, #272]
	mrs	x1, elr_el1
	str	x1, [sp, #16]
	mrs	x1, spsr_el1
	str	x1, [sp, #24]
	mrs	x1, esr_el1
	str	x1, [sp, #32]
	mrs	x1, far_el1
	str	x1, [sp, #40]

	mov	x0, sp
	bl	gdb_debug_interrupt

	/* The stub returns when the host continued: the frame may hold an
	 * edited context (pc, registers, SPSR). Restore and ERET. On a
	 * fault continued unchanged this re-executes the faulting
	 * instruction - the fault returns into the stub, under host
	 * control. */
	ldp	x2, x3, [sp, #16]	/* elr, spsr */
	msr	elr_el1, x2
	msr	spsr_el1, x3
	ldp	x0, x1, [sp, #48]
	ldp	x2, x3, [sp, #64]
	ldp	x4, x5, [sp, #80]
	ldp	x6, x7, [sp, #96]
	ldp	x8, x9, [sp, #112]
	ldp	x10, x11, [sp, #128]
	ldp	x12, x13, [sp, #144]
	ldp	x14, x15, [sp, #160]
	ldp	x16, x17, [sp, #176]
	ldp	x18, x19, [sp, #192]
	ldp	x20, x21, [sp, #208]
	ldp	x22, x23, [sp, #224]
	ldp	x24, x25, [sp, #240]
	ldp	x26, x27, [sp, #256]
	ldp	x28, x29, [sp, #272]
	ldr	x30, [sp, #8]
	add	sp, sp, #288
	eret
#endif /* THREADX_UP_BUILD */

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
