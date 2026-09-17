/*
 * @file   portasm_smp.S
 * @brief  Context switch and IRQ entry for the SMP port.
 *
 * Derived from the vendored upstream single-core port
 * (third-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE/portASM.S, MIT)
 * and registered as a derivative in IMPORT-INFO.md / imports.md. The save,
 * restore, SVC and IRQ sequences keep the upstream shape exactly - including
 * the SP_EL0/SP_ELx dance (tasks run with SP_ELx selected; the SP_EL0 stack
 * carries the context frames) - with three SMP changes:
 *
 *  1. The "current task" is pxCurrentTCBs[core], core read from MPIDR
 *     (Aff0), not the single pxCurrentTCB. The index arithmetic is
 *     [base, index, lsl #3] on a pointer array - pointer size 8.
 *
 *  2. Every per-core scalar (yield-required, interrupt nesting, FPU flag)
 *     is an array indexed the same way.
 *
 *  3. The critical-nesting save/restore is GONE. In SMP the kernel keeps
 *     the nesting count in the TCB and vTaskSwitchContext asserts it is
 *     zero at every switch, so no task is ever switched out with a masked
 *     PMR. The restore writes 0xff (unmask) unconditionally; the stack slot
 *     that used to carry the nesting count is still pushed/popped so the
 *     initial-task layout built by pxPortInitialiseStack stays shared.
 *
 * FPU note: the build is -mgeneral-regs-only and no task ever registers an
 * FPU context, so the FPU blocks below are dead weight kept for fidelity
 * with the upstream sequence (and for the day someone enables FP).
 */

	.text

	/* Variables and functions. */
	.extern pxCurrentTCBs
	.extern vTaskSwitchContext
	.extern vApplicationIRQHandler
	.extern ullPortInterruptNesting
	.extern ullPortTaskHasFPUContext
	.extern ullPortYieldRequired
	.extern _freertos_vector_table

	.global FreeRTOS_IRQ_Handler
	.global FreeRTOS_SWI_Handler
	.global vPortRestoreTaskContext

/* This port's core index: MPIDR Aff1 (this SoC numbers its cores in Aff1 -
 * Aff0 is 0 for every core; measured 0x8100_0N00, see board.h). Must match
 * board_smp_core_id() bit for bit. */
.macro port_get_core_id, reg
	mrs	\reg, mpidr_el1
	lsr	\reg, \reg, #8
	and	\reg, \reg, #0xff
.endm

.macro portSAVE_CONTEXT

	/* Switch to use the EL0 stack pointer. */
	MSR	SPSEL, #0

	/* Save the entire context. */
	STP	X0, X1, [SP, #-0x10]!
	STP	X2, X3, [SP, #-0x10]!
	STP	X4, X5, [SP, #-0x10]!
	STP	X6, X7, [SP, #-0x10]!
	STP	X8, X9, [SP, #-0x10]!
	STP	X10, X11, [SP, #-0x10]!
	STP	X12, X13, [SP, #-0x10]!
	STP	X14, X15, [SP, #-0x10]!
	STP	X16, X17, [SP, #-0x10]!
	STP	X18, X19, [SP, #-0x10]!
	STP	X20, X21, [SP, #-0x10]!
	STP	X22, X23, [SP, #-0x10]!
	STP	X24, X25, [SP, #-0x10]!
	STP	X26, X27, [SP, #-0x10]!
	STP	X28, X29, [SP, #-0x10]!
	STP	X30, XZR, [SP, #-0x10]!

	/* Save the SPSR and ELR. */
	MRS	X3, SPSR_EL1
	MRS	X2, ELR_EL1
	STP	X2, X3, [SP, #-0x10]!

	/* Save the FPU context indicator: PER CORE. */
	port_get_core_id X4
	LDR	X0, ullPortTaskHasFPUContextConst
	LDR	X2, [X0, X4, LSL #3]

	/* Save the FPU context, if any (32 128-bit plus two 64-bit status
	 * registers). */
	CMP	X2, #0
	B.EQ	1f
	STP	Q0, Q1, [SP,#-0x20]!
	STP	Q2, Q3, [SP,#-0x20]!
	STP	Q4, Q5, [SP,#-0x20]!
	STP	Q6, Q7, [SP,#-0x20]!
	STP	Q8, Q9, [SP,#-0x20]!
	STP	Q10, Q11, [SP,#-0x20]!
	STP	Q12, Q13, [SP,#-0x20]!
	STP	Q14, Q15, [SP,#-0x20]!
	STP	Q16, Q17, [SP,#-0x20]!
	STP	Q18, Q19, [SP,#-0x20]!
	STP	Q20, Q21, [SP,#-0x20]!
	STP	Q22, Q23, [SP,#-0x20]!
	STP	Q24, Q25, [SP,#-0x20]!
	STP	Q26, Q27, [SP,#-0x20]!
	STP	Q28, Q29, [SP,#-0x20]!
	STP	Q30, Q31, [SP,#-0x20]!

	/* Even though upper 32 bits of FPSR and FPCR are reserved, save and
	 * restore the whole 64 bits to keep 16-byte SP alignement. */
	MRS	X9, FPSR
	MRS	X10, FPCR
	STP	X9, X10, [SP, #-0x10]!

1:
	/* FPU indicator + the nesting slot (layout-compatible with the
	 * initial task stack; the nesting value itself lives in the TCB). */
	MOV	X3, #0
	STP	X2, X3, [SP, #-0x10]!

	LDR	X0, pxCurrentTCBsConst
	LDR	X1, [X0, X4, LSL #3]
	MOV	X0, SP	/* Move SP into X0 for saving. */
	STR	X0, [X1]

	/* Switch to use the ELx stack pointer. */
	MSR	SPSEL, #1

	.endm

; /**********************************************************************/

.macro portRESTORE_CONTEXT

	/* Switch to use the EL0 stack pointer. */
	MSR	SPSEL, #0

	/* Set the SP to point to the stack of the task being restored: the
	 * saved context pointer of THIS core's current TCB. */
	port_get_core_id X4
	LDR	X0, pxCurrentTCBsConst
	LDR	X1, [X0, X4, LSL #3]
	LDR	X0, [X1]
	MOV	SP, X0

	LDP	X2, X3, [SP], #0x10	/* FPU indicator + nesting slot. */

	/* Set the PMR register to be correct for the task being restored.
	 * In SMP that is always "fully open": kernel critical sections mask
	 * DAIF, not PMR, and a context switch only happens with the nesting
	 * count at zero - see the file comment. */
	MOV	X1, #255
	MSR	s3_0_c4_c6_0, X1	/* s3_0_c4_c6_0 is ICC_PMR_EL1. */
	DSB	SY
	ISB	SY

	/* Restore the FPU context indicator (per-core). */
	LDR	X0, ullPortTaskHasFPUContextConst
	STR	X2, [X0, X4, LSL #3]

	/* Restore the FPU context, if any. */
	CMP	X2, #0
	B.EQ	1f
	LDP	X9, X10, [SP], #0x10
	LDP	Q30, Q31, [SP], #0x20
	LDP	Q28, Q29, [SP], #0x20
	LDP	Q26, Q27, [SP], #0x20
	LDP	Q24, Q25, [SP], #0x20
	LDP	Q22, Q23, [SP], #0x20
	LDP	Q20, Q21, [SP], #0x20
	LDP	Q18, Q19, [SP], #0x20
	LDP	Q16, Q17, [SP], #0x20
	LDP	Q14, Q15, [SP], #0x20
	LDP	Q12, Q13, [SP], #0x20
	LDP	Q10, Q11, [SP], #0x20
	LDP	Q8, Q9, [SP], #0x20
	LDP	Q6, Q7, [SP], #0x20
	LDP	Q4, Q5, [SP], #0x20
	LDP	Q2, Q3, [SP], #0x20
	LDP	Q0, Q1, [SP], #0x20
	MSR	FPSR, X9
	MSR	FPCR, X10
1:
	LDP	X2, X3, [SP], #0x10	/* SPSR and ELR. */

	/* Restore the SPSR and ELR. */
	MSR	SPSR_EL1, X3
	MSR	ELR_EL1, X2

	LDP	X30, XZR, [SP], #0x10
	LDP	X28, X29, [SP], #0x10
	LDP	X26, X27, [SP], #0x10
	LDP	X24, X25, [SP], #0x10
	LDP	X22, X23, [SP], #0x10
	LDP	X20, X21, [SP], #0x10
	LDP	X18, X19, [SP], #0x10
	LDP	X16, X17, [SP], #0x10
	LDP	X14, X15, [SP], #0x10
	LDP	X12, X13, [SP], #0x10
	LDP	X10, X11, [SP], #0x10
	LDP	X8, X9, [SP], #0x10
	LDP	X6, X7, [SP], #0x10
	LDP	X4, X5, [SP], #0x10
	LDP	X2, X3, [SP], #0x10
	LDP	X0, X1, [SP], #0x10

	/* Switch to use the ELx stack pointer. */
	MSR	SPSEL, #1

	ERET

	.endm


/******************************************************************************
 * FreeRTOS_SWI_Handler handler is used to perform a context switch.
 *****************************************************************************/
.align 8
.type FreeRTOS_SWI_Handler, %function
FreeRTOS_SWI_Handler:
	/* Save the context of the current task and select a new task to run. */
	portSAVE_CONTEXT

	MRS	X0, ESR_EL1
	LSR	X1, X0, #26

	CMP	X1, #0x15	/* 0x15 = SVC instruction. */
	B.NE	FreeRTOS_Abort

	/* vTaskSwitchContext(xCoreID) - the SMP kernel takes both kernel
	 * locks itself and asserts the critical nesting count is zero. */
	port_get_core_id X0
	BL	vTaskSwitchContext

	portRESTORE_CONTEXT

FreeRTOS_Abort:
	/* Any synchronous exception that is NOT the port's SVC yield lands
	 * here. A bare `B .` made this the most expensive blind spot in the
	 * SMP bring-up: a data abort, an undefined instruction or a bad ERET
	 * target in any task froze the whole system with zero output. Park
	 * LOUD instead: the full ESR (exception class + syndrome) and the
	 * faulting ELR go straight to the UART, then the core parks with
	 * interrupts masked. */
	adr	x2, abort_msg
	bl	uart_early_puts
	mrs	x3, ESR_EL1
	lsr	x3, x3, #26		/* exception class in bits [31:26] */
	adr	x2, abort_ec_msg
	bl	uart_early_puts
	mov	x0, x3
	bl	uart_early_put_hex32
	adr	x2, abort_elr_msg
	bl	uart_early_puts
	mrs	x0, ELR_EL1
	lsr	x0, x0, #32
	bl	uart_early_put_hex32
	mrs	x0, ELR_EL1
	bl	uart_early_put_hex32
	adr	x2, abort_park_msg
	bl	uart_early_puts
2:	wfe
	b	2b

/******************************************************************************
 * vPortRestoreTaskContext is used to start the scheduler (on each core, with
 * pxCurrentTCBs[core] already set by the kernel).
 *****************************************************************************/
.align 8
.type vPortRestoreTaskContext, %function
vPortRestoreTaskContext:
.set freertos_vector_base,  _freertos_vector_table

	/* Install the FreeRTOS interrupt handlers. VBAR_EL1 is banked per
	 * core, so every core installs its own - the table itself is shared
	 * and position-independent (branches, not absolute addresses). */
	LDR	X1, =freertos_vector_base
	MSR	VBAR_EL1, X1
	DSB	SY
	ISB	SY

	/* Start the first task. */
	portRESTORE_CONTEXT


/******************************************************************************
 * FreeRTOS_IRQ_Handler handles IRQ entry and exit.

 * This handler is supposed to be used only for IRQs and never for FIQs. Per
 * ARM GIC documentation [1], Group 0 interrupts are always signaled as FIQs.
 * Since this handler is only for IRQs, We can safely assume Group 1 while
 * accessing Interrupt Acknowledge and End Of Interrupt registers and
 * therefore, use ICC_IAR1_EL1 and ICC_EOIR1_EL1.
 *
 * SMP changes against the upstream sequence: the nesting counter and the
 * yield-required flag are per-core arrays indexed by MPIDR Aff0, and the
 * tail call to vTaskSwitchContext carries this core's id. The core index is
 * kept alive across the C dispatch call on the stack (X0-X18 are
 * caller-saved), exactly like the INTID.
 *
 * [1] https://developer.arm.com/documentation/198123/0300/Arm-CoreLink-GIC-fundamentals
 *****************************************************************************/
.align 8
.type FreeRTOS_IRQ_Handler, %function
FreeRTOS_IRQ_Handler:
	/* Save volatile registers. */
	STP	X0, X1, [SP, #-0x10]!
	STP	X2, X3, [SP, #-0x10]!
	STP	X4, X5, [SP, #-0x10]!
	STP	X6, X7, [SP, #-0x10]!
	STP	X8, X9, [SP, #-0x10]!
	STP	X10, X11, [SP, #-0x10]!
	STP	X12, X13, [SP, #-0x10]!
	STP	X14, X15, [SP, #-0x10]!
	STP	X16, X17, [SP, #-0x10]!
	STP	X18, X19, [SP, #-0x10]!
	STP	X29, X30, [SP, #-0x10]!

	/* Save the SPSR and ELR. */
	MRS	X3, SPSR_EL1
	MRS	X2, ELR_EL1
	STP	X2, X3, [SP, #-0x10]!

	/* Increment the interrupt nesting counter, per core. */
	LDR	X5, ullPortInterruptNestingConst
	port_get_core_id X4
	LDR	X1, [X5, X4, LSL #3]	/* Old nesting count in X1. */
	ADD	X6, X1, #1
	STR	X6, [X5, X4, LSL #3]

	/* Maintain the interrupt nesting information across the function
	 * call. */
	STP	X1, X5, [SP, #-0x10]!

	/* Read interrupt ID from the interrupt acknowledge register and
	 * store it in X0 for future parameter and interrupt clearing use. */
	MRS	X0, S3_0_C12_C12_0	/* S3_0_C12_C12_0 is ICC_IAR1_EL1. */

	/* Maintain the interrupt ID and core index across the function call. */
	STP	X0, X4, [SP, #-0x10]!

	/* Call the C handler. */
	BL	vApplicationIRQHandler

	/* Disable interrupts. */
	MSR	DAIFSET, #2
	DSB	SY
	ISB	SY

	/* Restore the interrupt ID and core index. */
	LDP	X0, X4, [SP], #0x10

	/* End IRQ processing by writing interrupt ID value to the EOI
	 * register. */
	MSR	S3_0_C12_C12_1, X0	/* S3_0_C12_C12_1 is ICC_EOIR1_EL1. */

	/* Restore the critical nesting count. */
	LDP	X1, X5, [SP], #0x10
	STR	X1, [X5, X4, LSL #3]

	/* Has interrupt nesting unwound? */
	CMP	X1, #0
	B.NE	Exit_IRQ_No_Context_Switch

	/* Is a context switch required on THIS core? */
	LDR	X0, ullPortYieldRequiredConst
	LDR	X1, [X0, X4, LSL #3]
	CMP	X1, #0
	B.EQ	Exit_IRQ_No_Context_Switch

	/* Reset this core's yield-required flag to 0. */
	MOV	X2, #0
	STR	X2, [X0, X4, LSL #3]

	/* Restore volatile registers. */
	LDP	X4, X5, [SP], #0x10	/* SPSR and ELR. */
	MSR	SPSR_EL1, X5
	MSR	ELR_EL1, X4
	DSB	SY
	ISB	SY

	LDP	X29, X30, [SP], #0x10
	LDP	X18, X19, [SP], #0x10
	LDP	X16, X17, [SP], #0x10
	LDP	X14, X15, [SP], #0x10
	LDP	X12, X13, [SP], #0x10
	LDP	X10, X11, [SP], #0x10
	LDP	X8, X9, [SP], #0x10
	LDP	X6, X7, [SP], #0x10
	LDP	X4, X5, [SP], #0x10
	LDP	X2, X3, [SP], #0x10
	LDP	X0, X1, [SP], #0x10

	/* Save the context of the current task and select a new task to run
	 * on this core. */
	portSAVE_CONTEXT
	port_get_core_id X0
	BL	vTaskSwitchContext
	portRESTORE_CONTEXT

Exit_IRQ_No_Context_Switch:
	/* Restore volatile registers. */
	LDP	X4, X5, [SP], #0x10	/* SPSR and ELR. */
	MSR	SPSR_EL1, X5
	MSR	ELR_EL1, X4
	DSB	SY
	ISB	SY

	LDP	X29, X30, [SP], #0x10
	LDP	X18, X19, [SP], #0x10
	LDP	X16, X17, [SP], #0x10
	LDP	X14, X15, [SP], #0x10
	LDP	X12, X13, [SP], #0x10
	LDP	X10, X11, [SP], #0x10
	LDP	X8, X9, [SP], #0x10
	LDP	X6, X7, [SP], #0x10
	LDP	X4, X5, [SP], #0x10
	LDP	X2, X3, [SP], #0x10
	LDP	X0, X1, [SP], #0x10

	ERET

/******************************************************************************
 * If the application provides an implementation of vApplicationIRQHandler(),
 * then it will get called directly without saving the FPU registers on
 * interrupt entry, and this weak implementation of
 * vApplicationIRQHandler() will not get called. Unchanged from upstream.
 *****************************************************************************/

.align 8
.weak vApplicationIRQHandler
.type vApplicationIRQHandler, %function
vApplicationIRQHandler:
	/* Save LR and FP on the stack */
	STP	X29, X30, [SP, #-0x10]!

	/* Save FPU registers (32 128-bits + 2 64-bits configuration and
	 * status registers) */
	STP	Q0, Q1, [SP,#-0x20]!
	STP	Q2, Q3, [SP,#-0x20]!
	STP	Q4, Q5, [SP,#-0x20]!
	STP	Q6, Q7, [SP,#-0x20]!
	STP	Q8, Q9, [SP,#-0x20]!
	STP	Q10, Q11, [SP,#-0x20]!
	STP	Q12, Q13, [SP,#-0x20]!
	STP	Q14, Q15, [SP,#-0x20]!
	STP	Q16, Q17, [SP,#-0x20]!
	STP	Q18, Q19, [SP,#-0x20]!
	STP	Q20, Q21, [SP,#-0x20]!
	STP	Q22, Q23, [SP,#-0x20]!
	STP	Q24, Q25, [SP,#-0x20]!
	STP	Q26, Q27, [SP,#-0x20]!
	STP	Q28, Q29, [SP,#-0x20]!
	STP	Q30, Q31, [SP,#-0x20]!

	/* Even though upper 32 bits of FPSR and FPCR are reserved, save and
	 * restore the whole 64 bits to keep 16-byte SP alignement. */
	MRS	X9, FPSR
	MRS	X10, FPCR
	STP	X9, X10, [SP, #-0x10]!

	/* Call the C handler. */
	BL	vApplicationFPUSafeIRQHandler

	/* Restore FPU registers */

	LDP	X9, X10, [SP], #0x10
	LDP	Q30, Q31, [SP], #0x20
	LDP	Q28, Q29, [SP], #0x20
	LDP	Q26, Q27, [SP], #0x20
	LDP	Q24, Q25, [SP], #0x20
	LDP	Q22, Q23, [SP], #0x20
	LDP	Q20, Q21, [SP], #0x20
	LDP	Q18, Q19, [SP], #0x20
	LDP	Q16, Q17, [SP], #0x20
	LDP	Q14, Q15, [SP], #0x20
	LDP	Q12, Q13, [SP], #0x20
	LDP	Q10, Q11, [SP], #0x20
	LDP	Q8, Q9, [SP], #0x20
	LDP	Q6, Q7, [SP], #0x20
	LDP	Q4, Q5, [SP], #0x20
	LDP	Q2, Q3, [SP], #0x20
	LDP	Q0, Q1, [SP], #0x20
	MSR	FPSR, X9
	MSR	FPCR, X10

	/* Restore FP and LR */
	LDP	X29, X30, [SP], #0x10
	RET

.align 8
/* The constant pool: addresses of the symbols the fast paths need. .quad,
 * because LDR-literal with an X register reads eight bytes. */
pxCurrentTCBsConst:		.quad	pxCurrentTCBs
ullPortTaskHasFPUContextConst:	.quad	ullPortTaskHasFPUContext
ullPortInterruptNestingConst:	.quad	ullPortInterruptNesting
ullPortYieldRequiredConst:	.quad	ullPortYieldRequired

abort_msg:	.asciz	"\n[carrier] ABORT: non-yield synchronous exception, EC="
abort_ec_msg:	.asciz	" ELR="
abort_elr_msg:	.asciz	""
abort_park_msg:	.asciz	"\n"

.end
