/*
 * @file   rtl8211f.h
 * @brief  Realtek RTL8211F gigabit PHY, as a CMSIS ARM_DRIVER_ETH_PHY.
 *
 * One PHY per GMAC port, reached over that port's MDIO: the CMSIS shape for
 * this is a PHY driver that borrows the MAC's management interface -
 * Initialize() receives the MAC's PHY_Read/PHY_Write functions and uses those
 * for every register access. The PHY driver therefore knows nothing about
 * GMAC, DMA or clocks, and the MAC driver knows nothing about PHY models.
 *
 * Two things are RTL8211F-specific and worth knowing before reading the code:
 *
 *  - The chip straps its own RGMII TX delay and a HARD reset re-latches the
 *    strap. This board's GRF delays are calibrated with the PHY-side delay
 *    OFF, so after every hard reset the strapped delay must be cleared again
 *    (page 0xd08, register 0x11, bit 8). Leaving it set double-delays the
 *    transmit path and long frames die while ARP and short traffic keep
 *    working - the misleading combination recorded in the lab notes.
 *
 *  - Its register page is selected through register 0x1f (write 0x0d08 for the
 *    page holding that delay bit, write 0 to return to page 0), not through a
 *    separate page register.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_RTL8211F_H
#define FREEWEBCAMERA_RTL8211F_H

#include <stdint.h>

#include "Driver_ETH_PHY.h"

#define RTL8211F_PHY_COUNT	2
#define RTL8211F_DRV_VERSION	0x0100

/* Identification, from the standard PHY ID registers. */
#define RTL8211F_PHYSID1	0x001c
#define RTL8211F_PHYSID2	0xc916

/* What the boot log and the shell want to report about a port's PHY. */
struct rtl8211f_info {
	uint8_t  addr;	/* MDIO address the scan settled on */
	uint16_t id1;
	uint16_t id2;
	uint8_t  found;	/* 0 when no PHY answered on the bus */
};

int32_t rtl8211f_initialize(unsigned int index, ARM_ETH_PHY_Read_t fn_read,
			    ARM_ETH_PHY_Write_t fn_write);
int32_t rtl8211f_uninitialize(unsigned int index);
int32_t rtl8211f_power_control(unsigned int index, ARM_POWER_STATE state);
int32_t rtl8211f_set_interface(unsigned int index, uint32_t interface);
int32_t rtl8211f_set_mode(unsigned int index, uint32_t mode);
ARM_ETH_LINK_STATE rtl8211f_get_link_state(unsigned int index);
ARM_ETH_LINK_INFO rtl8211f_get_link_info(unsigned int index);
void rtl8211f_get_info(unsigned int index, struct rtl8211f_info *info);

/*
 * Declare one CMSIS ARM_DRIVER_ETH_PHY instance (same shape as the MAC's
 * instance macro: the ops struct carries no "this" pointer).
 */
#define RTL8211F_DECLARE_INSTANCE(n)                                           \
static ARM_DRIVER_VERSION rtl8211f##n##_get_version(void)                      \
{                                                                              \
	return (ARM_DRIVER_VERSION){ ARM_ETH_PHY_API_VERSION, RTL8211F_DRV_VERSION }; \
}                                                                              \
static int32_t rtl8211f##n##_initialize(ARM_ETH_PHY_Read_t fn_read,            \
					ARM_ETH_PHY_Write_t fn_write)          \
{                                                                              \
	return rtl8211f_initialize((n), fn_read, fn_write);                    \
}                                                                              \
static int32_t rtl8211f##n##_uninitialize(void)                                \
{                                                                              \
	return rtl8211f_uninitialize((n));                                     \
}                                                                              \
static int32_t rtl8211f##n##_power_control(ARM_POWER_STATE state)              \
{                                                                              \
	return rtl8211f_power_control((n), state);                             \
}                                                                              \
static int32_t rtl8211f##n##_set_interface(uint32_t interface)                 \
{                                                                              \
	return rtl8211f_set_interface((n), interface);                         \
}                                                                              \
static int32_t rtl8211f##n##_set_mode(uint32_t mode)                           \
{                                                                              \
	return rtl8211f_set_mode((n), mode);                                   \
}                                                                              \
static ARM_ETH_LINK_STATE rtl8211f##n##_get_link_state(void)                   \
{                                                                              \
	return rtl8211f_get_link_state((n));                                   \
}                                                                              \
static ARM_ETH_LINK_INFO rtl8211f##n##_get_link_info(void)                     \
{                                                                              \
	return rtl8211f_get_link_info((n));                                    \
}                                                                              \
ARM_DRIVER_ETH_PHY Driver_ETH_PHY##n = {                                       \
	rtl8211f##n##_get_version,                                             \
	rtl8211f##n##_initialize,                                              \
	rtl8211f##n##_uninitialize,                                            \
	rtl8211f##n##_power_control,                                           \
	rtl8211f##n##_set_interface,                                           \
	rtl8211f##n##_set_mode,                                                \
	rtl8211f##n##_get_link_state,                                          \
	rtl8211f##n##_get_link_info,                                           \
}

#endif /* FREEWEBCAMERA_RTL8211F_H */