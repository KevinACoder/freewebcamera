/*
 * @file   rk_i2c.h
 * @brief  RK3568 I2C (Rockchip v5 controller) - CMSIS-Driver instances.
 *
 * Consumers extern only the two standard ARM_DRIVER_I2C instances from here:
 *   Driver_I2C0  0xFDD40000 (PMU domain, RK809 PMIC @0x20)
 *   Driver_I2C1  0xFE5A0000 (RX8025T RTC @0x32, INA3221 @0x40)
 * Everything else is the driver's own business.
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#ifndef FREEWEBCAMERA_RK_I2C_H
#define FREEWEBCAMERA_RK_I2C_H

#include "Driver_I2C.h"

extern ARM_DRIVER_I2C Driver_I2C0;
extern ARM_DRIVER_I2C Driver_I2C1;

#endif /* FREEWEBCAMERA_RK_I2C_H */
