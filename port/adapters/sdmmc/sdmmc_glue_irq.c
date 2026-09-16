/*
 * @file   sdmmc_glue_irq.c
 * @brief  The one-slot IRQ parking the interrupt-mode glue needs: the SDK's
 *         InterruptInstall carried a per-controller parameter and this
 *         project's IRQ_SetHandler does not. Only reached when a glue runs
 *         in interrupt mode (polling this milestone).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stddef.h>
#include <stdint.h>

#include "finterrupt.h"

void (*sdmmc_glue_irq_handler)(void *param);
void *sdmmc_glue_irq_param;

void sdmmc_glue_irq_dispatch(void)
{
	if (sdmmc_glue_irq_handler != NULL) {
		sdmmc_glue_irq_handler(sdmmc_glue_irq_param);
	}
}
