/*
 * @file   port_vectors.S
 * @brief  The VBAR_EL1 exception table the scheduler installs per core.
 *
 * Layout and contents transplanted from the board-validated reference SMP
 * port (MIT/BSD, see IMPORT-INFO.md). Sixteen 0x80-byte entries; only the
 * first two groups do real work:
 *
 *   +0x000 group (current EL, SP_EL0): where tasks take exceptions - a task
 *          runs at EL1 with SPSEL=0, so IRQ and SVC land here.
 *   +0x200 group (current EL, SP_ELx): exceptions inside the port's own
 *          SP_EL1 windows (context save/restore, kernel C on the exception
 *          stack).
 *   +0x400..+0x780 groups (lower EL): unreachable at EL1 - there is no lower
 *          implemented EL that can take an exception into EL1 - so they park.
 *
 * Two bring-up lessons from the reference line are kept as-is:
 *  - the FIQ entries are a sentinel that prints 'F' on the polled UART and
 *    spins: a FIQ here means a Group 0 interrupt (OP-TEE's lines) has
 *    punched through to NS EL1, which used to present as a silent freeze;
 *  - the SP_ELx synchronous entry has its own handler (vTaskSwitchSPx): an
 *    SVC arriving with SP_ELx selected means a yield ran from the wrong
 *    stack, and parking loudly beats corrupting the switch.
 */

	.arch	armv8-a

/* board_conf.h is data-only #defines; .S files take it through the
 * preprocessor like startup.S does. */
#include "board_conf.h"

	.global _freertos_vector_table
	.global vSynchronousInterruptHandler
	.global vSynchronousInterruptHandlerSPx
	.global vSErrorInterruptHandler
	.global FreeRTOS_IRQ_Handler

	.section .vectors.freertos, "ax"


_freertos_vector_table:
.set	FREERTOS_VBAR, _freertos_vector_table
.align 11 /* VBAR_EL1 Vector Base Address is [55:48]  */
.org(FREERTOS_VBAR)
	b	vSynchronousInterruptHandler

.org (FREERTOS_VBAR + 0x80)
	b	FreeRTOS_IRQ_Handler

.org (FREERTOS_VBAR + 0x100)
	b	freertos_fiq_hang		/* bring-up 轮12: 原为 b . 静默死循环, FIQ 一到即无声冻结 */

.org (FREERTOS_VBAR + 0x180)
	b	vSErrorInterruptHandler

.org (FREERTOS_VBAR + 0x200)
	b	vSynchronousInterruptHandlerSPx

.org (FREERTOS_VBAR + 0x280)
	b	FreeRTOS_IRQ_Handler

.org (FREERTOS_VBAR + 0x300)
	b	freertos_fiq_hang		/* bring-up 轮12: 同上 (SPx 组) */

.org (FREERTOS_VBAR + 0x380)
	b	vSErrorInterruptHandler		/* SError/vSError */

.org (FREERTOS_VBAR + 0x400)
	b	.

.org (FREERTOS_VBAR + 0x480)
	b	.

.org (FREERTOS_VBAR + 0x500)
	b	.

.org (FREERTOS_VBAR + 0x580)
	b	.

.org (FREERTOS_VBAR + 0x600)
	b	.

.org (FREERTOS_VBAR + 0x680)
	b	.

.org (FREERTOS_VBAR + 0x700)
	b	.

.org (FREERTOS_VBAR + 0x780)
	b	.

/* bring-up 轮12：FIQ 到达即打 'F' 后自旋——证明 Group0 中断（OP-TEE 侧）打穿了 NS EL1。
 * 诊断用：clobber x17/x18（FIQ 进入即死态，无所谓）。UART 基址取 board_conf.h
 * （DW-APB 16550：32 位访问、4 字节步进，THR 在 +0x00，LSR 在 +0x14）。 */
freertos_fiq_hang:
	ldr	x17, =BOARD_UART2_BASE
991:	ldr	w18, [x17, #0x14]
	tbz	w18, #5, 991b
	movz	w18, #'F'
	str	w18, [x17]
	movz	w18, #0x0D
	str	w18, [x17]
	movz	w18, #0x0A
	str	w18, [x17]
992:	wfe
	b	992b

	.ltorg
