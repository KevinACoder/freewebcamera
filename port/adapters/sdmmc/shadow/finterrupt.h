/*
 * @file   finterrupt.h
 * @brief  Shadow of the SDK interrupt-control header the standalone host
 *         glue calls in its interrupt-mode path. Maps the four calls onto
 *         this project's CMSIS irq_ctrl.h backend.
 *
 * The SDK's Install carries a per-controller parameter; this project's
 * IRQ_SetHandler does not, so the pair is parked in the glue's dispatch
 * variables (defined in sdmmc_host.c) and read back by its thunk.
 *
 * Polling mode (this milestone's configuration) never reaches any of this;
 * it exists so the carried-over interrupt-mode code compiles unchanged.
 *
 * Not an interface: nothing outside the sdmmc adapter includes this.
 */

#ifndef SDMMC_SHADOW_FINTERRUPT_H
#define SDMMC_SHADOW_FINTERRUPT_H

#include <stdint.h>

#include "board.h"
#include "irq_ctrl.h"

/* Provided by sdmmc_host.c: the one IRQ the glue arms, and its dispatch. */
extern void (*sdmmc_glue_irq_handler)(void *param);
extern void *sdmmc_glue_irq_param;
void sdmmc_glue_irq_dispatch(void);

#define InterruptSetPriority(irqn, prio) \
	((void)(prio), \
	 (void)IRQ_SetPriority((IRQn_ID_t)(irqn), BOARD_IRQ_PRIORITY_API_CALL_RAW))

#define InterruptInstall(irqn, handler, param, name) \
	((void)(name), \
	 (sdmmc_glue_irq_handler = (handler)), \
	 (sdmmc_glue_irq_param = (param)), \
	 (void)IRQ_SetHandler((IRQn_ID_t)(irqn), sdmmc_glue_irq_dispatch), \
	 0)

#define InterruptUmask(irqn) ((void)IRQ_Enable((IRQn_ID_t)(irqn)))
#define InterruptMask(irqn)  ((void)IRQ_Disable((IRQn_ID_t)(irqn)))

#endif /* SDMMC_SHADOW_FINTERRUPT_H */
