/*
 * @file   rk3568_gmac.c
 * @brief  The two GMAC ports of this board, as CMSIS Ethernet MAC instances.
 *
 * Everything in this file is board data: register coordinates, pin multiplexing
 * tables, PHY reset lines and MAC addresses, taken from the runs that verified
 * them (see docs/evidence/). The driver itself knows none of it - it receives a
 * struct dwc_eqos_plat through DWC_EQOS_DECLARE_INSTANCE, which is also the one
 * line that ties a port to an implementation.
 *
 * Both ports are instantiated. They are the same IP at different addresses with
 * different clocks, pin groups and PHY reset lines:
 *
 *              gmac0                    gmac1
 *   base       0xFE2A0000               0xFE010000
 *   irq        59                       64               (INTID = GIC SPI + 32)
 *   MAC        02:e4:a5:35:68:00        02:e4:a5:35:68:01
 *   clocks     CLKGATE_CON15 mask 0x1F70  CLKGATE_CON17 mask 0x07DC
 *   reset      SOFTRST_CON13 bit 7       SOFTRST_CON14 bit 12
 *   mux        CLKSEL_CON31              CLKSEL_CON33
 *   rgmii      GMAC0_CON0 0x264F         GMAC1_CON0 0x033C  (tx/rx delays)
 *   iomux      GPIO2 group (bank 0x020)  GPIO4 group (bank 0x040) + io route
 *   PHY reset  GPIO2_B1 (bank 2, pin 9)  GPIO3_C2 (bank 3, pin 18)
 *
 * The clock-gate masks and the RGMII delay values are not interchangeable:
 * gmac1's receive delay differs from gmac0's, and carrying gmac0's value over is
 * one of the failures recorded in the lab notes.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>

#include "dwc_eqos.h"
#include "dwc_eqos_rk3568.h"
#include "rtl8211f.h"

/* --- gmac0 ---------------------------------------------------------------- */

/* rgmii bus + clock, tx_bus2/rx_bus2, miim. */
static const struct rk3568_gmac_pin gmac0_pins[] = {
	{ 3, 2 }, { 4, 2 }, { 5, 2 }, { 6, 2 }, { 7, 2 }, { 8, 2 },
	{ 11, 1 }, { 12, 1 }, { 13, 1 }, { 14, 1 }, { 15, 2 }, { 16, 2 },
	{ 19, 2 }, { 20, 2 },
};

static const struct rk3568_gmac_soc gmac0_soc = {
	.clkgate_con_off = 0x33C,
	.clkgate_mask    = 0x1F70,
	.softrst_con_off = 0x434,
	.softrst_mask    = 0x0080,
	.clksel_con_off  = 0x17C,

	.grf_maccon0_off  = 0x380,
	.grf_maccon0_val  = 0x264F,	/* rx delay 0x26, tx delay 0x4f */
	.grf_maccon0_mask = 0x7F7F,
	.grf_maccon1_off  = 0x384,
	.grf_maccon1_val  = 0x0013,	/* RGMII + both clock delay enables */
	.grf_maccon1_mask = 0x0073,

	.grf_iomux_off   = 0x020,
	.pins            = gmac0_pins,
	.pin_num         = sizeof(gmac0_pins) / sizeof(gmac0_pins[0]),
	.grf_route_off   = 0,
	.grf_route_val   = 0,

	.rst_gpio_base   = 0xFE750000UL,
	.rst_gpio_pin    = 9,
};

static const struct dwc_eqos_plat gmac0_plat = {
	.index     = 0,
	.base_addr = 0xFE2A0000UL,
	.irq_num   = 59,
	.mac_addr  = { 0x02, 0xe4, 0xa5, 0x35, 0x68, 0x00 },

	.soc_init      = rk3568_gmac_soc_init,
	.soc_swr_quirk = rk3568_gmac_soc_swr_quirk,
	.soc_data      = &gmac0_soc,
};

DWC_EQOS_DECLARE_INSTANCE(0, &gmac0_plat);

/* --- gmac1 ---------------------------------------------------------------- */

/* gmac1m0 tx_bus2/rx_bus2, rgmii clock, clkinout and miim. */
static const struct rk3568_gmac_pin gmac1_pins[] = {
	{ 13, 3 }, { 14, 3 }, { 2, 3 }, { 3, 3 }, { 15, 3 }, { 9, 3 },
	{ 10, 3 }, { 4, 3 }, { 5, 3 }, { 11, 3 }, { 6, 3 }, { 7, 3 },
	{ 16, 3 }, { 20, 3 }, { 21, 3 },
};

static const struct rk3568_gmac_soc gmac1_soc = {
	.clkgate_con_off = 0x344,
	.clkgate_mask    = 0x07DC,
	.softrst_con_off = 0x438,
	.softrst_mask    = 0x1000,
	.clksel_con_off  = 0x184,

	.grf_maccon0_off  = 0x388,
	.grf_maccon0_val  = 0x033C,	/* rx delay 0x03, tx delay 0x3c */
	.grf_maccon0_mask = 0x7F7F,
	.grf_maccon1_off  = 0x38C,
	.grf_maccon1_val  = 0x0013,
	.grf_maccon1_mask = 0x0073,

	.grf_iomux_off   = 0x040,
	.pins            = gmac1_pins,
	.pin_num         = sizeof(gmac1_pins) / sizeof(gmac1_pins[0]),
	.grf_route_off   = 0x300,
	.grf_route_val   = 0x0100,

	.rst_gpio_base   = 0xFE760000UL,
	.rst_gpio_pin    = 18,
};

static const struct dwc_eqos_plat gmac1_plat = {
	.index     = 1,
	.base_addr = 0xFE010000UL,
	.irq_num   = 64,
	.mac_addr  = { 0x02, 0xe4, 0xa5, 0x35, 0x68, 0x01 },

	.soc_init      = rk3568_gmac_soc_init,
	.soc_swr_quirk = rk3568_gmac_soc_swr_quirk,
	.soc_data      = &gmac1_soc,
};

DWC_EQOS_DECLARE_INSTANCE(1, &gmac1_plat);

/* --- the PHYs ------------------------------------------------------------- */

/* One RTL8211F per port, each reached over its own port's MDIO (the MAC
 * instance with the same number hands it the read/write pair). Same chip on
 * both ports, but they are separate drivers rather than one shared one: the
 * link state, the MDIO address and the strap bit are per port, and the
 * adapter's single registration point is where they get paired up. */
RTL8211F_DECLARE_INSTANCE(0);
RTL8211F_DECLARE_INSTANCE(1);