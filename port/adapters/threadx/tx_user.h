/*
 * @file   tx_user.h
 * @brief  ThreadX user configuration (shadow header, adapter-owned).
 *
 * Included by the kernel via -DTX_INCLUDE_USER_DEFINE_FILE, the same way
 * the FreeRTOS image's FreeRTOSConfig.h is adapter-owned. Everything left
 * undefined here takes the tx_port.h default; the defaults that matter for
 * this image are recorded below so nobody has to go digging:
 *
 *   TX_THREAD_SMP_MAX_CORES      4   (matches BOARD_SMP_CORES=4; forwarded
 *                                     as -DSMP_CORES, not redefined here)
 *   TX_MAX_PRIORITIES            32
 *   TX_MINIMUM_STACK             200
 *   TX_TIMER_THREAD_PRIORITY     0   (kernel timer thread, mostly asleep)
 *   TX_TIMER_THREAD_STACK_SIZE   16384 (default 4096 overflows the
 *                                     net80211 callout callbacks - driver
 *                                     watchdog/state-machine chains run
 *                                     there on the CMSIS osTimer bridge;
 *                                     M7 first boot crashed with wild
 *                                     jumps ~2s after attach)
 *
 * Deliberately NOT enabled:
 *   TX_ENABLE_WFI                idle would WFI instead of the port's
 *                                default busy-poll; kept off to stay on
 *                                the upstream default until the comparison
 *                                run asks for it (knob, not policy).
 *   TX_ENABLE_STACK_CHECKING     adds per-thread fill/checking; off for
 *                                behavioral parity with the FreeRTOS image.
 *   ENABLE_ARM_FP                the image is -mgeneral-regs-only; there is
 *                                no FP state to save (tx_port.h guards all
 *                                FP code on this define).
 */

#ifndef TX_USER_H
#define TX_USER_H

/* Backlink from each thread control block to the CMSIS adapter's slot, set
 * by cmsis_os2_impl.c right after tx_thread_create. Thread flags need the
 * per-thread TX_EVENT_FLAGS_GROUP that lives in the slot; walking a 16-entry
 * slot table on every ISR flag-set would add a scan to the console wake-up
 * path, so the pointer rides in the TCB instead. The kernel itself never
 * touches the extension field. */
/* The board's tick glue arms a 1000 Hz timer (tx_glue.c TX_TICK_RATE_HZ);
 * CherryUSB's osal refuses to compile against any other claim, and every
 * ms-based osal timeout rides on this being true. */
#define TX_TIMER_TICKS_PER_SECOND			1000

/* The net80211 callouts (urtwn watchdog, ieee80211 state machines) run on
 * the kernel timer thread via the CMSIS osTimer bridge - a driver-depth
 * call chain that the 4 KB default stack cannot hold. */
#define TX_TIMER_THREAD_STACK_SIZE			16384

#define TX_THREAD_USER_EXTENSION	VOID	*tx_thread_cmsis_slot;

#endif /* TX_USER_H */
