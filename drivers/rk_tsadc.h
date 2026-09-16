/*
 * @file   rk_tsadc.h
 * @brief  RK3568 on-chip thermal sensor ADC (TSADC @ 0xFE710000).
 *
 * The board's real temperature source. The RK809 PMIC has no readable
 * temperature register (only hotdie/TSD threshold bits), which is what closed
 * the older "PMIC temperature over I2C" plan (decision D30).
 *
 * Consumers:
 *   rk_tsadc_init()              idempotent bring-up (clocks, GRF taps, auto mode)
 *   rk_tsadc_read_mc(src, &t)    temperature in millicelsius; src 0=CPU, 1=GPU
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#ifndef FREEWEBCAMERA_RK_TSADC_H
#define FREEWEBCAMERA_RK_TSADC_H

#include <stdint.h>

#define RK_TSADC_SRC_CPU	0
#define RK_TSADC_SRC_GPU	1

int rk_tsadc_init(void);
int rk_tsadc_read_mc(int src, int32_t *temp_mc);

#endif /* FREEWEBCAMERA_RK_TSADC_H */
