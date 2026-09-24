/*
 * @file   tx_up_shim.c
 * @brief  UP (single-core) ThreadX build: the SMP-port primitives the CMSIS
 *         adapter calls, reduced to their single-core meaning.
 *
 * port/adapters/cmsis_rtos2_threadx/ takes its kernel lock (osKernelLock and
 * the ISR-safe paths) through _tx_thread_smp_protect/_tx_thread_smp_unprotect.
 * Under common_smp those come from the port's spinlock assembly; the UP
 * kernel has no such symbols - its own critical section IS the DAIF mask
 * (tx_port.h TX_DISABLE/TX_RESTORE). This shim provides the same pair scoped
 * to one core so the adapter source stays kernel-shape agnostic. Compiled
 * only for THREADX_UP=1 images (see the Makefile THREADX_UP block).
 *
 * Semantics mirror the SMP pair's contract at the call sites: protect()
 * returns the interrupted posture and masks IRQ+FIQ; unprotect() restores it
 * wholesale - exactly tx_port.h's __disable_interrupts/__restore_interrupts.
 */

#include <stdint.h>

unsigned int _tx_thread_smp_protect(void)
{
	unsigned int daif;

	__asm__ __volatile__("mrs %0, daif" : "=r"(daif));
	__asm__ __volatile__("msr daifset, 0x3" ::: "memory");
	return daif;
}

void _tx_thread_smp_unprotect(unsigned int save)
{
	unsigned long daif = save;

	__asm__ __volatile__("msr daif, %0" :: "r"(daif) : "memory");
}
