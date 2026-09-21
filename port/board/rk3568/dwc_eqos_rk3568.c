/*
 * @file   dwc_eqos_rk3568.c
 * @brief  RK3568 SoC glue for the DesignWare Ethernet QoS MAC core: CRU
 *         clocks, GRF interface/delay configuration, pin iomux and the PHY
 *         reset GPIO.
 *
 * Order matters here and is the verified one: clocks, iomux, RGMII delays,
 * then the PHY reset pulse (a hard reset re-latches the PHY's straps, so
 * anything the PHY driver wants to undo has to happen after this returns).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>

#include "cmsis_os2.h"
#include "regs.h"

#include "dwc_eqos_rk3568.h"

/* GRF iomux register for one pin of a bank: four bits per pin, four pins per
 * 32-bit word, two words per eight-pin group. */
static uintptr_t grf_iomux_reg(unsigned int bank_off, unsigned int pin,
			       unsigned int *shift)
{
	*shift = (pin % 4) * 4;

	return RK3568_GRF_BASE + bank_off + (pin / 8) * 8 + ((pin % 8) / 4) * 4;
}

/* GRF/CRU field write with the hiword write-enable mask. */
static void grf_write(unsigned int off, uint32_t mask, uint32_t val)
{
	reg_wr32(RK3568_GRF_BASE + off, ((mask & 0xffffu) << 16) | (val & mask));
}

static void rk3568_gmac_clocks_enable(const struct rk3568_gmac_soc *soc)
{
	/* Gate bits are active-low: write 0 into the hiword-masked field to
	 * enable the clock. */
	reg_wr32(RK3568_CRU_BASE + soc->clkgate_con_off,
		 (uint32_t)soc->clkgate_mask << 16);
	/* Soft reset bits are active-low too: write 0 to deassert. */
	reg_wr32(RK3568_CRU_BASE + soc->softrst_con_off,
		 (uint32_t)soc->softrst_mask << 16);
	/* GMAC clock muxes: all-zero selector = 125 MHz RGMII output mode
	 * (rx/tx from the 125M source, ptp ref 62.5M). */
	reg_wr32(RK3568_CRU_BASE + soc->clksel_con_off, 0xffffu << 16);
}

static void rk3568_gmac_iomux(const struct rk3568_gmac_soc *soc)
{
	int i;

	if (soc->grf_route_off != 0) {
		/* Full register word: write-enable mask in bits[31:16]. */
		reg_wr32(RK3568_GRF_BASE + soc->grf_route_off, soc->grf_route_val);
	}

	for (i = 0; i < soc->pin_num; i++) {
		const struct rk3568_gmac_pin *p = &soc->pins[i];
		unsigned int shift;
		uintptr_t reg = grf_iomux_reg(soc->grf_iomux_off, p->pin, &shift);

		reg_wr32(reg, ((0xfu << shift) << 16) | ((uint32_t)p->func << shift));
	}
}

static void rk3568_gmac_rgmii_setup(const struct rk3568_gmac_soc *soc)
{
	/* CON1 (interface select and delay enables) before CON0 (the delay
	 * values): the verified order. */
	grf_write(soc->grf_maccon1_off, soc->grf_maccon1_mask, soc->grf_maccon1_val);
	grf_write(soc->grf_maccon0_off, soc->grf_maccon0_mask, soc->grf_maccon0_val);
}

/* GPIO bank, version 2 layout: data at 0x00/0x04, direction at 0x08/0x0c,
 * value in bits[15:0] and write-enable in bits[31:16]. */
static void gpio_write(unsigned long bank, unsigned int pin, int output, int value)
{
	unsigned long off = (pin < 16) ? 0x0 : 0x4;
	uint32_t bit = 1u << (pin & 15);
	uint32_t we = bit << 16;

	/* Data first, then the direction. */
	reg_wr32(bank + off, we | (value ? bit : 0));
	reg_wr32(bank + off + 0x8, we | (output ? bit : 0));
}

static void rk3568_gmac_phy_reset_pulse(const struct rk3568_gmac_soc *soc)
{
	/* Active low: assert for 20 ms, release, then wait 100 ms for the PHY
	 * to boot and re-latch its straps. Writing the polarity backwards
	 * leaves MDIO reading 0xffff from every address, which looks like a
	 * dead bus rather than a held reset. */
	gpio_write(soc->rst_gpio_base, soc->rst_gpio_pin, 1, 0);
	osDelay(20);
	gpio_write(soc->rst_gpio_base, soc->rst_gpio_pin, 1, 1);
	osDelay(100);
}

int rk3568_gmac_soc_init(const struct dwc_eqos_plat *plat)
{
	const struct rk3568_gmac_soc *soc = plat->soc_data;

	rk3568_gmac_clocks_enable(soc);
	rk3568_gmac_iomux(soc);
	rk3568_gmac_rgmii_setup(soc);
	rk3568_gmac_phy_reset_pulse(soc);

	return 0;
}

void rk3568_gmac_soc_swr_quirk(const struct dwc_eqos_plat *plat, int enter)
{
	const struct rk3568_gmac_soc *soc = plat->soc_data;
	uint32_t val = (enter != 0) ? 0x1u : 0x0u;

	/* While DMA_MODE.SWR is asserted the GMAC clock mux has to take the
	 * RMII (62.5M) path or the reset never clears: on gmac0 the 125M direct
	 * path is not active at cold boot, and gmac1 needs the same walk on
	 * some boots. Leaving it in RMII afterwards would keep the MAC at the
	 * wrong rate, hence the exit call. */
	reg_wr32(RK3568_CRU_BASE + soc->clksel_con_off, (0x3u << 16) | val);
}