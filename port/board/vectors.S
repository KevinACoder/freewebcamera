/*
 * @file   vectors.S
 * @brief  The FreeRTOS vector table the upstream port expects us to supply.
 *
 * The upstream ARM_AARCH64_SRE port declares `_freertos_vector_table` as an
 * external symbol and loads VBAR_EL1 from it in vPortRestoreTaskContext, but
 * never defines it - the port is generic and expects the integrator to provide
 * the table. Without it the link fails on that symbol.
 *
 * Layout follows the AArch64 exception model: 16 entries of 0x80 bytes,
 * grouped as four kinds (sync / IRQ / FIQ / SError) for four source states
 * (current EL with SP_EL0, current EL with SP_ELx, lower EL AArch64, lower EL
 * AArch32). Only two entries do real work:
 *
 *   +0x000 / +0x200  SVC -> FreeRTOS_SWI_Handler  (task-level yield, since the
 *                    port uses SVC 0 rather than a self-posted SGI)
 *   +0x080 / +0x280  IRQ -> FreeRTOS_IRQ_Handler
 *
 * The rest park. A fault before or after the scheduler starts is fatal here -
 * there is no recovery path in a single-core RTOS with no MMU-protected
 * processes, and silently ignoring it would hide exactly the failures this
 * project needs to see.
 *
 * Note both SP_EL0 and SP_ELx entries are wired: the port's context switch
 * flips SPSEL, so an interrupt can arrive with either stack selected.
 */

	.arch	armv8-a
	.section .vectors, "ax"
	.align	11

	.globl	_freertos_vector_table
	.type	_freertos_vector_table, %function
_freertos_vector_table:

	/* Current EL, SP_EL0 */
	.p2align 7
	b	FreeRTOS_SWI_Handler		/* synchronous: SVC yield */
	.p2align 7
	b	FreeRTOS_IRQ_Handler
	.p2align 7
	b	board_fiq_park
	.p2align 7
	b	board_serror_park

	/* Current EL, SP_ELx */
	.p2align 7
	b	FreeRTOS_SWI_Handler
	.p2align 7
	b	FreeRTOS_IRQ_Handler
	.p2align 7
	b	board_fiq_park
	.p2align 7
	b	board_serror_park

	/* Lower EL, AArch64 */
	.p2align 7
	b	FreeRTOS_SWI_Handler
	.p2align 7
	b	FreeRTOS_IRQ_Handler
	.p2align 7
	b	board_fiq_park
	.p2align 7
	b	board_serror_park

	/* Lower EL, AArch32 */
	.p2align 7
	b	FreeRTOS_SWI_Handler
	.p2align 7
	b	FreeRTOS_IRQ_Handler
	.p2align 7
	b	board_fiq_park
	.p2align 7
	b	board_serror_park

	.text
	.align 2

/* FIQ and SError are unrecoverable: FIQ would mean something is configured
 * as Group 0 (which this port never does), SError means a bus error. Report
 * the one distinguishing fact - the fault status register - and park, rather
 * than pretending to continue. */
	.type	board_fiq_park, %function
board_fiq_park:
	ldr	x0, =msg_fiq
	b	board_park

	.type	board_serror_park, %function
board_serror_park:
	ldr	x0, =msg_serror
	b	board_park

/* Report and park. Reached only from the two unrecoverable vectors above;
 * nothing here can return, because there is no context to return to. */
	.type	board_park, %function
board_park:
	bl	board_early_print
1:	wfe
	b	1b

	.section .rodata, "a"
msg_fiq:	.asciz	"\n[board] FAULT: FIQ (unexpected - nothing should be Group 0)\n"
msg_serror:	.asciz	"\n[board] FAULT: SError (bus error)\n"
