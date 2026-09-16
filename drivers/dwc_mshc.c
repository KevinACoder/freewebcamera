/*
 * @file   dwc_mshc.c
 * @brief  DWC MSHC host controller driver core (the eMMC controller, SDHCI
 *         4.20a register interface), with SDMA data transfers in polling and
 *         interrupt modes.
 *
 * PROVENANCE AND PORTING POLICY (decision D27). The register sequences - CRU
 * gate/reset/source-clock handling, the pin-group iomux/pull/drive table,
 * the SDHCI clock tree with the 50M source bypass (this board has no 52M
 * tap: 52 MHz is served from the 50M source, 25 MHz from FREQ_SEL=1), the
 * DLL bypass below 100 MHz and lock sequence above it, the SDMA engine with
 * its 512 KiB boundary restart, the command/data reset recovery, and the
 * byte-shifted R2 readout this controller needs - are the author's own
 * RK3568 port, debugged on this board in the standalone line and carried
 * over line-for-line. Vocabulary changed only:
 *
 *   - register primitives are this project's regs.h (halfword accessors were
 *     added for exactly this driver: access width is part of the sequence);
 *   - the SDK non-cacheable buffer assumption is replaced by explicit
 *     board_dcache_flush/invalidate around the SDMA buffer;
 *   - fsleep_* become counter-timer busy waits (kernel-free for the K4 build);
 *   - the SDK read-only-config blocks are dropped: this controller is the
 *     storage target of the milestone and is authorized for writes (D28).
 *
 * Interrupt mode is carried over intact but runs disabled; the adapter
 * drives this controller in polling mode.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "regs.h"

#include "dwcmshc.h"
#include "dwc_mshc_regs.h"

/* --- constants -------------------------------------------------------------- */

#define DWCMSHC_TIMEOUT_MS	1000U	/* command wait budget */
#define DWCMSHC_DATA_TIMEOUT_MS	3000U	/* data wait budget */
#define DWCMSHC_RESET_RETRIES	2000U	/* SW_RST_ALL self-clear polls */
#define DWCMSHC_INIT_CLK_FREQ	400000U

/* Board-debug round: per-command tracing on (revert to 0 after bring-up). */
#ifndef DWCMSHC_VERBOSE
#define DWCMSHC_VERBOSE		1
#endif

#define DWCMSHC_ERROR(fmt, ...)	board_log("dwmshc: " fmt "\n", ##__VA_ARGS__)
#define DWCMSHC_INFO(fmt, ...) \
	do { \
		if (DWCMSHC_VERBOSE) { \
			board_log("dwmshc: " fmt "\n", ##__VA_ARGS__); \
		} \
	} while (0)

/* Bounded busy wait on the system counter (see dwc_mmc.c). */
static void dwc_mshc_udelay(uint32_t us)
{
	uint32_t freq;
	uint64_t start;
	uint64_t ticks;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(start));
	ticks = ((uint64_t)freq * us + 999999U) / 1000000U;
	for (;;) {
		uint64_t now;

		__asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
		if ((now - start) >= ticks) {
			break;
		}
	}
}

/* Rockchip CRU/GRF hiword write-enable store. */
static void dwc_mshc_rk_clrset(uintptr_t reg, uint32_t mask, uint32_t val)
{
	reg_wr32(reg, ((mask & 0xFFFFU) << 16) | (val & mask));
}

/**
 * CRU bring-up: enable the five eMMC gates, release the five soft resets,
 * select CCLK_EMMC = 24M (the 400 kHz identity clock comes from the SDHCI
 * FREQ_SEL divider) and BCLK_EMMC = 200M.
 */
static int dwc_mshc_cru_clock_init(struct dwc_mshc *inst)
{
	uintptr_t cru = RK3568_CRU_BASE_ADDR;

	/* gate bit = 1 means the clock is off; write 0 to enable */
	dwc_mshc_rk_clrset(cru + DWMSHC_CRU_CLKGATE_CON9_OFF,
			   DWMSHC_CRU_EMMC_GATE_ALL, 0);
	/* release soft resets (assert = write 1, release = write 0) */
	dwc_mshc_rk_clrset(cru + DWMSHC_CRU_SOFTRST_CON7_OFF,
			   DWMSHC_CRU_EMMC_SOFTRST_ALL, 0);
	/* CCLK_EMMC -> 24M, BCLK_EMMC -> 200M (clksel_con[28]) */
	dwc_mshc_rk_clrset(cru + DWMSHC_CRU_CLKSEL_CON28_OFF,
			   DWMSHC_CRU_CCLK_EMMC_MUX_MASK,
			   DWMSHC_CRU_CCLK_EMMC_SEL_24M << DWMSHC_CRU_CCLK_EMMC_MUX_SHIFT);
	dwc_mshc_rk_clrset(cru + DWMSHC_CRU_CLKSEL_CON28_OFF,
			   DWMSHC_CRU_BCLK_EMMC_MUX_MASK,
			   DWMSHC_CRU_BCLK_EMMC_SEL_200M << DWMSHC_CRU_BCLK_EMMC_MUX_SHIFT);
	inst->cur_source_clock = 24000000U;

	return DWCMSHC_SUCCESS;
}

/**
 * eMMC pin group (GPIO1_B4~B7 + C0~C7, fn1), matching u-boot rk3568.c and
 * the rk3568-pinctrl.dtsi emmc group: fn1 iomux, bus/clk/cmd pull-up
 * drive-2, datastrobe/rstnout pull-none.
 */
static int dwc_mshc_pinctrl_init(struct dwc_mshc *inst)
{
	uintptr_t grf = RK3568_GRF_BASE_ADDR;

	/* iomux: B4-B7 / C0-C7 -> fn1 (value 0x1111, enable 0xffff<<16) */
	reg_wr32(grf + DWMSHC_GRF_GPIO1B_IOMUX_H_OFF, (0xFFFFU << 16) | 0x1111U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_IOMUX_L_OFF, (0xFFFFU << 16) | 0x1111U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_IOMUX_H_OFF, (0xFFFFU << 16) | 0x1111U);

	/* pull (2-bit/pin): B4-B7 pull-up; C0-C5 pull-up, C6/C7 pull-none */
	reg_wr32(grf + DWMSHC_GRF_GPIO1B_PULL_OFF, 0xF0000000U | 0x5500U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_PULL_OFF, 0x0FF00000U | 0x0555U);

	/* drive strength level 2 (8-bit/pin, 2 pins/reg, value 0x07, en 0x3f) */
	reg_wr32(grf + DWMSHC_GRF_GPIO1B_DS_2_OFF, 0x3f3f0707U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1B_DS_3_OFF, 0x3f3f0707U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_DS_0_OFF, 0x3f3f0707U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_DS_1_OFF, 0x3f3f0707U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_DS_2_OFF, 0x3f3f0707U);
	reg_wr32(grf + DWMSHC_GRF_GPIO1C_DS_3_OFF, 0x3f3f0707U);

	return DWCMSHC_SUCCESS;
}

/**
 * Pick the CRU CCLK_EMMC source and the SDHCI FREQ_SEL divider:
 * card_clk = source / (2 * freq_sel), freq_sel = 0 bypasses.
 */
static int dwc_mshc_select_clock_source(struct dwc_mshc *inst, uint32_t target,
					uint32_t *freq_sel)
{
	uintptr_t cru = RK3568_CRU_BASE_ADDR;
	uint32_t sel;
	uint32_t src_rate;
	uint32_t fsel;

	if (target <= 750000U) {
		/* 400 kHz identity: 375k source, bypass (u-boot/linux same) */
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_375K;
		src_rate = 375000U;
		fsel = 0U;
	} else if (target <= 24000000U) {
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_24M;
		src_rate = 24000000U;
		fsel = src_rate / (2U * target);
	} else if (target <= 52000000U) {
		/* 25M/50M/52M from the 50M source (this board has no 52M tap).
		 * 50M/52M bypass; anything slower takes the smallest divider
		 * that lands at or under the target (26M -> FREQ_SEL=1 ->
		 * 25MHz): the protocol's normal-mode stage runs against a card
		 * still in default speed, which cannot be clocked at 50MHz
		 * (measured 2026-09-16: the bypass made every following CMD6
		 * time out). */
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_50M;
		src_rate = 50000000U;
		if (src_rate <= target) {
			fsel = 0U;
		} else {
			fsel = (src_rate + (2U * target) - 1U) / (2U * target);
		}
	} else if (target <= 100000000U) {
		/* SDR50: 100M source, bypass */
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_100M;
		src_rate = 100000000U;
		fsel = 0U;
	} else if (target <= 150000000U) {
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_150M;
		src_rate = 150000000U;
		fsel = 0U;
	} else {
		/* HS200 200M */
		sel = DWMSHC_CRU_CCLK_EMMC_SEL_200M;
		src_rate = 200000000U;
		fsel = 0U;
	}

	dwc_mshc_rk_clrset(cru + DWMSHC_CRU_CLKSEL_CON28_OFF,
			   DWMSHC_CRU_CCLK_EMMC_MUX_MASK,
			   sel << DWMSHC_CRU_CCLK_EMMC_MUX_SHIFT);
	inst->cur_source_clock = src_rate;
	*freq_sel = fsel;

	DWCMSHC_INFO("CLK target=%u -> source=%u(%u) freq_sel=%u", target, sel,
		     src_rate, fsel);
	return DWCMSHC_SUCCESS;
}

/**
 * DLL lock for >=100 MHz (HS200/HS400): reset DLL -> start (start_point=5,
 * increment=2) -> wait LOCKED -> program RXCLK/TXCLK/STRBIN from the lock
 * value (u-boot dwcmshc sequence).
 */
static int dwc_mshc_dll_lock(struct dwc_mshc *inst)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t status = 0U;
	uint32_t loop;
	uint32_t lock_value;

	/* reset DLL */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CTRL_OFFSET, DWMSHC_DLL_SRST);
	dwc_mshc_udelay(1);
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CTRL_OFFSET, 0);

	/* auto-tuning control: tune clock stop en + pre/post change delay */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_AT_CTRL_OFFSET,
			 (0x1U << 16) | (0x2U << 17) | (0x3U << 19));

	/* start DLL: start_point=5, increment=2 */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CTRL_OFFSET,
			 (5U << 16) | (2U << 8) | DWMSHC_DLL_START);

	/* wait for lock (500us budget, u-boot same) */
	loop = 500U;
	do {
		status = DWMSHC_READ_REG(base_addr, DWMSHC_DLL_STATUS0_OFFSET);
		if ((status & DWMSHC_DLL_STATUS_LOCKED) &&
		    (0U == (status & DWMSHC_DLL_STATUS_TIMEOUT))) {
			break;
		}
		dwc_mshc_udelay(1);
	} while (--loop);

	if (0U == loop) {
		DWCMSHC_ERROR("DLL lock timeout (status0=0x%x)", status);
		return DWCMSHC_ERR_TIMEOUT;
	}

	lock_value = ((status & 0xFFU) * 2U) & 0xFFU;

	/* RXCLK: DLYENA | ORI_GATE | NO_INVERTER | TAP_VALUE_SEL | lock<<8 */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_RXCLK_OFFSET,
			 DWMSHC_DLL_DLYENA | DWMSHC_DLL_RXCLK_ORI_GATE |
				 DWMSHC_DLL_RXCLK_NO_INVERTER |
				 DWMSHC_DLL_TAP_VALUE_SEL |
				 (lock_value << DWMSHC_DLL_TAP_VALUE_OFFSET));
	/* TXCLK: DLYENA | TAPNUM_FROM_SW | NO_INVERTER | hs200_tx_tap(16) */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_TXCLK_OFFSET,
			 DWMSHC_DLL_DLYENA | DWMSHC_DLL_TXCLK_TAPNUM_FROM_SW |
				 DWMSHC_DLL_RXCLK_NO_INVERTER | 16U |
				 DWMSHC_DLL_TAP_VALUE_SEL |
				 (lock_value << DWMSHC_DLL_TAP_VALUE_OFFSET));
	/* STRBIN: DLYENA | hs400_strbin_tap(3) | TAPNUM_FROM_SW | TAP_VALUE_SEL */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_STRBIN_OFFSET,
			 DWMSHC_DLL_DLYENA | 3U |
				 DWMSHC_DLL_STRBIN_TAPNUM_FROM_SW |
				 DWMSHC_DLL_TAP_VALUE_SEL |
				 (lock_value << DWMSHC_DLL_TAP_VALUE_OFFSET));

	DWCMSHC_INFO("DLL locked: lock_value=%u", lock_value);
	return DWCMSHC_SUCCESS;
}

/**
 * DLL configuration: <=52 MHz takes the bypass path, >=100 MHz (HS200/HS400)
 * takes the lock path (linux/u-boot dwcmshc_rk3568_set_clock same split).
 */
static int dwc_mshc_dll_config(struct dwc_mshc *inst)
{
	uintptr_t base_addr = inst->config.base_addr;

	if (inst->cur_card_clk < 100000000U) {
		/* disable DLL, reset clock phase */
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CTRL_OFFSET, 0);
		/* disable cmd conflict check */
		DWMSHC_WRITE_BYTE(base_addr, DWMSHC_HOST_CTRL3_OFFSET, 0);
		/* DLL bypass */
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CTRL_OFFSET,
				 DWMSHC_DLL_BYPASS | DWMSHC_DLL_START);
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_RXCLK_OFFSET,
				 DWMSHC_DLL_RXCLK_ORI_GATE);
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_TXCLK_OFFSET, 0);
		/* U-Boot and linux both zero the CMD-output phase here; leaving a
		 * stale value skews the host->card command output at these speeds */
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_CMDOUT_OFFSET, 0);
		/* STRBIN: DLYENA | DELAY_NUM_SEL | 16<<16 (ddr50_strbin_delay_num) */
		DWMSHC_WRITE_REG(base_addr, DWMSHC_DLL_STRBIN_OFFSET,
				 DWMSHC_DLL_DLYENA |
					 DWMSHC_DLL_STRBIN_DELAY_NUM_SEL |
					 (16U << DWMSHC_DLL_STRBIN_DELAY_NUM_OFF));
	} else {
		return dwc_mshc_dll_lock(inst);
	}

	return DWCMSHC_SUCCESS;
}

int dwc_mshc_software_reset(uintptr_t base_addr, int retries)
{
	uint32_t rst;

	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_SW_RST_OFFSET, DWMSHC_SW_RST_ALL);
	while (retries-- > 0) {
		rst = DWMSHC_READ_BYTE(base_addr, DWMSHC_SW_RST_OFFSET);
		if (0U == (rst & DWMSHC_SW_RST_ALL)) {
			return DWCMSHC_SUCCESS;
		}
		dwc_mshc_udelay(100);
	}

	return DWCMSHC_ERR_TIMEOUT;
}

int dwc_mshc_initialize(struct dwc_mshc *inst,
			const struct dwc_mshc_config *config)
{
	uintptr_t base_addr;
	int ret = DWCMSHC_SUCCESS;
	uint32_t ctrl1;

	if (inst->is_ready != 0U) {
		DWCMSHC_ERROR("device is already initialized");
	}

	dwc_mshc_deinitialize(inst);
	inst->config = *config;
	base_addr = inst->config.base_addr;

	/* board: CRU gates / soft resets / source clocks */
	(void)dwc_mshc_cru_clock_init(inst);

	/* eMMC pin group iomux/pull/drive */
	(void)dwc_mshc_pinctrl_init(inst);

	/* controller soft reset (SDHCI SW_RST_ALL) */
	ret = dwc_mshc_software_reset(base_addr, DWCMSHC_RESET_RETRIES);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	/* restore the internal clock after the reset (linux rk35xx_sdhci_reset) */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_MISC_CON_OFFSET, DWMSHC_MISC_INTCLK_EN);

	/* cmd conflict check off (linux/u-boot both clear it) */
	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_HOST_CTRL3_OFFSET, 0);

	/* 8-bit bus + SDMA: the standalone reference drives this exact board
	 * with DMA_SEL=SDMA and the buffer address in SDMASA (u-boot/linux on
	 * this part likewise use the legacy engine), and that combination is
	 * the one proven to move eMMC data here. */
	ctrl1 = DWMSHC_HC1_EXT_DAT_XFER | DWMSHC_HC1_DAT_XFER_WIDTH |
		DWMSHC_HC1_DMA_SEL_SDMA;
	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET, (uint8_t)ctrl1);

	/* CARD_IS_EMMC (reset value 0x0C, RST_N released -> 0x0D) */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_EMMC_CTRL_OFFSET, 0x0DU);

	/* board VCCQ (vccio2) is fixed 1.8V: set the 1.8V signaling semantic
	 * from the start so a later HS200 switch finds the controller ready
	 * (linux order: switch voltage before timing) */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET,
			  DWMSHC_HC2_SIGNALING_1_8V);

	/* clear pending status; enable all status bits (signal enables stay
	 * off here - interrupt mode turns them on) */
	dwc_mshc_clear_interrupt_status(base_addr);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_EN_OFF, DWMSHC_NINT_ALL);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_EN_OFF, DWMSHC_EINT_ALL);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF, 0);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_SIG_OFF, 0);

	/* response/data timeout counters */
	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_TOUT_CTRL_OFFSET, 0x0EU);

	/* enable card power: the board's eMMC IO rail is vcc_1v8, so select
	 * 1.8V | bus power (= 0x0b, exactly what u-boot programs here). The
	 * standalone line asked for 3.3V, which the doubled field encoding
	 * turned into 3.0V - a mismatch that leaves the host's output cells
	 * configured for the wrong level. */
	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_PWR_CTRL_OFFSET,
			  DWMSHC_PWR_VDD_1_8V | DWMSHC_PWR_SD_BUS_POWER);

	/* wait for the power ramp */
	dwc_mshc_udelay(10000U);

	/* initial 400 kHz clock */
	ret = dwc_mshc_set_card_clk(inst, DWCMSHC_INIT_CLK_FREQ);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	/* let the clock settle before the first command (CMD0 times out
	 * without this delay - measured on this board) */
	dwc_mshc_udelay(2000U);

	inst->is_ready = 1U;

	return DWCMSHC_SUCCESS;
}

/*
 * One-shot state dump: enough registers to diff our configuration against a
 * known-good driver, plus the head of the transfer buffer so a data-phase
 * failure can be told apart from a misconfiguration. Diagnose-only.
 */
void dwc_mshc_dump_state(struct dwc_mshc *inst, const char *why,
			 const void *buf, uint32_t len)
{
	uintptr_t base = inst->config.base_addr;
	uintptr_t cru = RK3568_CRU_BASE_ADDR;

	DWCMSHC_ERROR("%s: HC1=%02x PWR=%02x CLKCTRL=%04x HC2=%04x", why,
		      DWMSHC_READ_BYTE(base, DWMSHC_HOST_CTRL1_OFFSET),
		      DWMSHC_READ_BYTE(base, DWMSHC_PWR_CTRL_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_CLK_CTRL_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_HOST_CTRL2_OFFSET));
	DWCMSHC_ERROR("  CRU EM_CON0=%08x CON1=%08x CLKSEL28=%08x",
		      reg_rd32(cru + DWMSHC_CRU_EMMC_CON0_OFF),
		      reg_rd32(cru + DWMSHC_CRU_EMMC_CON1_OFF),
		      reg_rd32(cru + DWMSHC_CRU_CLKSEL_CON28_OFF));
	DWCMSHC_ERROR("  SDMASA=%08x BLKSZ=%04x BLKCNT=%04x XFER=%04x CMD=%04x",
		      DWMSHC_READ_REG(base, DWMSHC_SDMASA_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_BLOCKSIZE_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_BLOCKCOUNT_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_XFER_MODE_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_CMD_OFFSET));
	DWCMSHC_ERROR("  PSTATE=%08x NINT=%04x EINT=%04x", 
		      DWMSHC_READ_REG(base, DWMSHC_PSTATE_OFFSET),
		      DWMSHC_READ_HALF(base, DWMSHC_NORMAL_INT_STAT_OFF),
		      DWMSHC_READ_HALF(base, DWMSHC_ERR_INT_STAT_OFF));
	if ((NULL != buf) && (len >= 16U)) {
		const uint32_t *w = (const uint32_t *)buf;

		DWCMSHC_ERROR("  buf[0..3]=%08x %08x %08x %08x (va %p)",
			      w[0], w[1], w[2], w[3], buf);
	}
}

void dwc_mshc_deinitialize(struct dwc_mshc *inst)
{
	inst->is_ready = 0U;
	memset(inst, 0, sizeof(*inst));
}

int dwc_mshc_set_card_clk(struct dwc_mshc *inst, uint32_t clk_freq_hz)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t freq_sel = 0U;
	uint32_t clk_ctrl;
	uint32_t loop;
	int ret;

	if (0U == clk_freq_hz) {
		/* stop the card clock (internal clock keeps running). The
		 * protocol only asks for this through SetCardClock(0), which
		 * it does when its own speed bookkeeping came up empty. */
		DWCMSHC_ERROR("clock stop requested (protocol reached us with freq=0)");
		clk_ctrl = DWMSHC_READ_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET);
		clk_ctrl &= ~(uint32_t)DWMSHC_CLK_SD_EN;
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET,
				  (uint16_t)clk_ctrl);
		return DWCMSHC_SUCCESS;
	}

	/* wait for cmd/data inhibit to clear (PSTATE bit0/bit1) */
	loop = DWCMSHC_TIMEOUT_MS;
	while (DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET) &
	       (DWMSHC_PSTATE_CMD_INHIBIT | DWMSHC_PSTATE_DAT_INHIBIT)) {
		if (0U == --loop) {
			DWCMSHC_ERROR("timeout on clock inhibit (pstate=0x%x)",
				      DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET));
			return DWCMSHC_ERR_TIMEOUT;
		}
		dwc_mshc_udelay(100);
	}

	/* stop the output clock */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET, 0);

	/* CRU source selection */
	ret = dwc_mshc_select_clock_source(inst, clk_freq_hz, &freq_sel);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	/* divider + internal clock enable */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET,
			  DWMSHC_CLK_INT_EN | DWMSHC_CLK_FREQ_SEL(freq_sel));

	/* wait for the internal clock to stabilise */
	loop = DWCMSHC_TIMEOUT_MS;
	do {
		clk_ctrl = DWMSHC_READ_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET);
		if (clk_ctrl & DWMSHC_CLK_INT_STABLE) {
			break;
		}
		dwc_mshc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		DWCMSHC_ERROR("internal clock not stable (clk_ctrl=0x%x)", clk_ctrl);
		return DWCMSHC_ERR_TIMEOUT;
	}

	/* enable the card clock output */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET,
			  (uint16_t)(clk_ctrl | DWMSHC_CLK_SD_EN));

	inst->cur_card_clk = clk_freq_hz;

	/* DLL configuration (<=52MHz bypass, >=100MHz lock) */
	ret = dwc_mshc_dll_config(inst);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	/* high speed: >=25MHz sets HIGH_SPEED; 25-52MHz -> UHS_MODE=SDR25
	 * (linux MMC_HS); >=100MHz (HS200) -> UHS_MODE=SDR104(3) + 1.8V
	 * signaling (linux MMC_HS200 same) */
	if (clk_freq_hz >= 25000000U) {
		uint32_t ctrl1 = DWMSHC_READ_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET);
		uint32_t ctrl2 = DWMSHC_READ_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET);

		DWMSHC_WRITE_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET,
				  (uint8_t)(ctrl1 | DWMSHC_HC1_HIGH_SPEED));

		if (clk_freq_hz >= 100000000U) {
			ctrl2 = (ctrl2 & ~(uint32_t)0x7U) | DWMSHC_HC2_UHS_MODE(3);
			ctrl2 |= DWMSHC_HC2_SIGNALING_1_8V;
		} else {
			ctrl2 = (ctrl2 & ~(uint32_t)0x7U) | DWMSHC_HC2_UHS_MODE(1);
			/* board signaling is fixed 1.8V: SIGNALING_EN stays set
			 * from init, it does not fall back */
		}
		/* DRV_TYPE_A: the output driver strength. U-Boot carries this in
		 * HOST_CTRL2 (measured 0x009b on this board) and it is the only
		 * host->card drive knob that differs from ours: without it the
		 * card sees corrupt DAT bytes at 52 MHz (CMD24 answers with
		 * DATA_CRC/END_BIT) while reads stay clean, and the same write
		 * succeeds at 24 MHz - an output-integrity margin, not a
		 * programming error. */
		ctrl2 |= DWMSHC_HC2_DRV_TYPE(1U);
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET, (uint16_t)ctrl2);
	}

	/* let the clock tree settle after the switch */
	dwc_mshc_udelay(1000U);

	DWCMSHC_INFO("BUS CLOCK: %u Hz, source %u Hz, freq_sel %u", clk_freq_hz,
		     inst->cur_source_clock, freq_sel);
	return ret;
}

void dwc_mshc_set_card_bus_width(struct dwc_mshc *inst, uint32_t bus_width)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t ctrl1 = DWMSHC_READ_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET);

	/* keep DMA_SEL and the other bits; touch only the bus width. The width is
	 * a one-of: 8-bit is EXT_DAT_XFER alone, 4-bit is DAT_XFER_WIDTH alone.
	 * Setting both (which an earlier revision of this port did) puts the
	 * controller in a state the spec does not define and which corrupts the
	 * host->card data phase while leaving card->host reads working - u-boot,
	 * linux and the standalone all write bit5 with bit1 clear for 8-bit. */
	ctrl1 &= ~(uint32_t)(DWMSHC_HC1_DAT_XFER_WIDTH | DWMSHC_HC1_EXT_DAT_XFER);

	if (8U == bus_width) {
		ctrl1 |= DWMSHC_HC1_EXT_DAT_XFER;
	} else if (4U == bus_width) {
		ctrl1 |= DWMSHC_HC1_DAT_XFER_WIDTH;
	}

	DWMSHC_WRITE_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET, (uint8_t)ctrl1);
}

static uint32_t dwc_mshc_make_raw_cmd(struct dwc_mshc_cmd *cmd_p)
{
	uint32_t raw_cmd = DWMSHC_CMD_INDEX(cmd_p->cmdidx);

	if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_RESP) {
		if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_LONG_RESP) {
			raw_cmd |= DWMSHC_CMD_RESP_TYPE_136;
		} else {
			raw_cmd |= DWMSHC_CMD_RESP_TYPE_48;
		}

		if (cmd_p->flag & DWCMSHC_CMD_FLAG_NEED_RESP_CRC) {
			raw_cmd |= DWMSHC_CMD_CRC_CHK;
			/* R2 (136-bit) gets no index check (u-boot/linux sdhci) */
			if (!(cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_LONG_RESP)) {
				raw_cmd |= DWMSHC_CMD_IDX_CHK;
			}
		}
	}

	if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_DATA) {
		raw_cmd |= DWMSHC_CMD_DATA_PRESENT;
	}

	return raw_cmd;
}

/* DWC MSHC holds the 136-bit response byte-shifted: the first register
 * byte is wire padding (0x00) and the register content is the response
 * shifted right by 8 bits, so recover R[127:96]..R[31:8] by shifting left
 * across words. Verified against this board's U-Boot: the same registers
 * decode to Manufacturer f4 / OEM 0x122 / name ARJ11X, which is exactly
 * what U-Boot's own mmc info prints (measured 2026-09-16). */
static void dwc_mshc_read_response(uintptr_t base_addr, struct dwc_mshc_cmd *cmd_p)
{
	if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_LONG_RESP) {
		uint32_t r01 = DWMSHC_READ_REG(base_addr, DWMSHC_RESP01_OFFSET);
		uint32_t r23 = DWMSHC_READ_REG(base_addr, DWMSHC_RESP23_OFFSET);
		uint32_t r45 = DWMSHC_READ_REG(base_addr, DWMSHC_RESP45_OFFSET);
		uint32_t r67 = DWMSHC_READ_REG(base_addr, DWMSHC_RESP67_OFFSET);

		DWCMSHC_INFO("R2 raw (cmd %u): %08x %08x %08x %08x", cmd_p->cmdidx,
			     r01, r23, r45, r67);
		cmd_p->response[3] = (r67 << 8) | (r45 >> 24);
		cmd_p->response[2] = (r45 << 8) | (r23 >> 24);
		cmd_p->response[1] = (r23 << 8) | (r01 >> 24);
		cmd_p->response[0] = (r01 << 8);
	} else {
		cmd_p->response[0] = DWMSHC_READ_REG(base_addr, DWMSHC_RESP01_OFFSET);
		cmd_p->response[1] = 0U;
		cmd_p->response[2] = 0U;
		cmd_p->response[3] = 0U;
	}
}

static int dwc_mshc_send_command(uintptr_t base_addr, struct dwc_mshc_cmd *cmd_p)
{
	uint32_t raw_cmd;
	uint32_t stat = 0U;
	uint32_t err = 0U;
	uint32_t loop;
	uint32_t inhibit;

	/* wait for command inhibit to clear (PSTATE bit0); data commands also
	 * need the data path idle (PSTATE bit1) */
	inhibit = DWMSHC_PSTATE_CMD_INHIBIT;
	if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_DATA) {
		inhibit |= DWMSHC_PSTATE_DAT_INHIBIT;
	}
	loop = DWCMSHC_TIMEOUT_MS;
	while (DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET) & inhibit) {
		if (0U == --loop) {
			DWCMSHC_ERROR("timeout on CMD inhibit (pstate=0x%x)",
				      DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET));
			return DWCMSHC_ERR_TIMEOUT;
		}
		dwc_mshc_udelay(100);
	}

	/* clear all interrupt status */
	dwc_mshc_clear_interrupt_status(base_addr);

	DWMSHC_WRITE_REG(base_addr, DWMSHC_ARGUMENT_OFFSET, cmd_p->cmdarg);

	raw_cmd = dwc_mshc_make_raw_cmd(cmd_p);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CMD_OFFSET, (uint16_t)raw_cmd);

	/* wait for command completion */
	loop = DWCMSHC_TIMEOUT_MS;
	do {
		stat = DWMSHC_READ_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF);
		err = DWMSHC_READ_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF);

		if (stat & DWMSHC_NINT_CMD_COMPLETE) {
			break;
		}

		if (err & DWMSHC_CMD_ERR_FLAGS) {
			break;
		}

		dwc_mshc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		DWCMSHC_ERROR("CMD %u timeout", cmd_p->cmdidx);
		DWCMSHC_ERROR("DBG pstate=0x%08x nint=0x%04x eint=0x%04x clk=0x%04x ctrl1=0x%02x ctrl2=0x%04x",
			      DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET), stat,
			      err,
			      DWMSHC_READ_HALF(base_addr, DWMSHC_CLK_CTRL_OFFSET),
			      DWMSHC_READ_BYTE(base_addr, DWMSHC_HOST_CTRL1_OFFSET),
			      DWMSHC_READ_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET));
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF, (uint16_t)stat);
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, (uint16_t)err);
		/* reset the command circuit so CMD_INHIBIT does not wedge the
		 * next command; this controller gates the SD clock output in
		 * the reset, so put it back (measured 2026-09-16) */
		DWMSHC_WRITE_BYTE(base_addr, DWMSHC_SW_RST_OFFSET, DWMSHC_SW_RST_CMD);
		dwc_mshc_udelay(1000);
		return DWCMSHC_ERR_TIMEOUT;
	}

	if (err & DWMSHC_CMD_ERR_FLAGS) {
		if ((err & DWMSHC_EINT_CMD_TIMEOUT) && (55U == cmd_p->cmdidx)) {
			DWCMSHC_INFO("CMD %u response timeout (expected for CMD55)",
				     cmd_p->cmdidx);
		} else {
			DWCMSHC_ERROR("CMD %u error: eint=0x%x", cmd_p->cmdidx, err);
		}
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, (uint16_t)err);
		return DWCMSHC_ERR_CMD_FAILED;
	}

	/* clear only CMD_COMPLETE: a data command may already have
	 * XFER_COMPLETE set, which must stay for WaitDataOver (writing the
	 * whole stat back would clear it) */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
			  DWMSHC_NINT_CMD_COMPLETE);

	if (cmd_p->flag & DWCMSHC_CMD_FLAG_EXP_RESP) {
		dwc_mshc_read_response(base_addr, cmd_p);
	}

	return DWCMSHC_SUCCESS;
}

/* Program the SDMA engine for one transfer (u-boot sdhci: SDMASA points at
 * the data buffer, 512 KiB boundary; transfers stay <= 64 blocks / 32 KiB,
 * boundary restarts are handled in WaitDataOver).
 *
 * The buffer here is ordinary write-back cacheable memory (the reference
 * build is non-cacheable, so its driver needs no maintenance at all). Both
 * directions therefore need explicit care:
 *   - read: flush_invalidate *before* issuing the command. The protocol
 *     layer memsets the staging buffer to zero immediately before an
 *     EXT_CSD read (fsl_mmc.c), and those dirty zero lines would otherwise
 *     be written back over the device's data; flushing first leaves the
 *     cache clean, so nothing can clobber the transfer, and the final
 *     invalidate after completion drops whatever the CPU still holds.
 *   - write: clean before issuing so the device sees the fresh bytes.
 * Same double-sided discipline as drivers/dwc_nvme.c. */
static int dwc_mshc_prepare_dma(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd_p)
{
	struct dwc_mshc_data *dat_p = cmd_p->data_p;
	uintptr_t base_addr = inst->config.base_addr;
	uintptr_t buf_bus = (uintptr_t)dat_p->buf;
	uint32_t xfer;

	if ((NULL == dat_p->buf) || (0U == dat_p->datalen)) {
		return DWCMSHC_ERR_INVALID_BUF;
	}

	if ((0U != (buf_bus & 0x3U)) || (0U != (dat_p->datalen & 0x3U))) {
		DWCMSHC_ERROR("buffer not 4-byte aligned for DMA");
		return DWCMSHC_ERR_INVALID_BUF;
	}

	if (0U != (cmd_p->flag & DWCMSHC_CMD_FLAG_WRITE_DATA)) {
		board_dcache_flush(buf_bus, dat_p->datalen);
	} else {
		board_dcache_flush_invalidate(buf_bus, dat_p->datalen);
	}

	/* SDMA system address (0x00): the buffer itself, not a descriptor */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_SDMASA_OFFSET, (uint32_t)buf_bus);
	inst->cur_dma_phy = buf_bus;
	inst->cur_dma_len = dat_p->datalen;
	inst->cur_dma_boundary = 0U;

	/* block size / count (BLOCKSIZE[14:12] = DMA boundary 512 KiB) */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_BLOCKSIZE_OFFSET,
			  (uint16_t)(dat_p->blksz | (7U << 12)));
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_BLOCKCOUNT_OFFSET,
			  (uint16_t)dat_p->blkcnt);

	/* transfer mode: DMA | BLK_CNT_EN | multi | read direction */
	xfer = DWMSHC_XFER_DMA_EN | DWMSHC_XFER_BLK_CNT_EN;
	if (dat_p->blkcnt > 1U) {
		xfer |= DWMSHC_XFER_MULTI_BLK;
	}
	if (cmd_p->flag & DWCMSHC_CMD_FLAG_READ_DATA) {
		xfer |= DWMSHC_XFER_DATA_DIR_READ;
	}
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_XFER_MODE_OFFSET, (uint16_t)xfer);

	return DWCMSHC_SUCCESS;
}

static int dwc_mshc_wait_data_over(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t stat;
	uint32_t err;
	uint32_t loop = DWCMSHC_DATA_TIMEOUT_MS;

	do {
		stat = DWMSHC_READ_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF);
		err = DWMSHC_READ_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF);

		if (err & DWMSHC_DATA_ERR_FLAGS) {
			DWCMSHC_ERROR("CMD %u data error: eint=0x%x adma_err=0x%x",
				      cmd_p->cmdidx, err,
				      DWMSHC_READ_BYTE(base_addr, DWMSHC_ADMA_ERR_STAT_OFF));
			dwc_mshc_dump_state(inst, "data-error", cmd_p->data_p->buf,
					    cmd_p->data_p->datalen);
			DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
					  (uint16_t)stat);
			DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF,
					  (uint16_t)err);
			return DWCMSHC_ERR_DATA_FAILED;
		}

		if (stat & DWMSHC_NINT_XFER_COMPLETE) {
			DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
					  (uint16_t)stat);
			/* a read just landed in RAM: drop the stale lines */
			if (0U != (cmd_p->flag & DWCMSHC_CMD_FLAG_READ_DATA)) {
				board_dcache_invalidate(
					(uintptr_t)cmd_p->data_p->buf,
					cmd_p->data_p->datalen);
			}
			return DWCMSHC_SUCCESS;
		}

		/* SDMA hit the 512KiB boundary: bump SDMASA and continue */
		if (stat & DWMSHC_NINT_DMA_INTERRUPT) {
			uint32_t transferred =
				(inst->cur_dma_boundary + 1U) * (512U * 1024U);
			DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
					  DWMSHC_NINT_DMA_INTERRUPT);
			if (transferred < inst->cur_dma_len) {
				DWMSHC_WRITE_REG(base_addr, DWMSHC_SDMASA_OFFSET,
						 (uint32_t)(inst->cur_dma_phy +
							    transferred));
				inst->cur_dma_boundary++;
			}
		}

		dwc_mshc_udelay(100);
	} while (--loop);

	DWCMSHC_ERROR("CMD %u data timeout (nint=0x%x eint=0x%x)", cmd_p->cmdidx,
		      stat, err);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF, (uint16_t)stat);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, (uint16_t)err);
	return DWCMSHC_ERR_TIMEOUT;
}

static int dwc_mshc_transfer_data(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret;

	ret = dwc_mshc_prepare_dma(inst, cmd_p);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	ret = dwc_mshc_send_command(base_addr, cmd_p);
	if (DWCMSHC_SUCCESS != ret) {
		return ret;
	}

	ret = dwc_mshc_wait_data_over(inst, cmd_p);

	if (DWCMSHC_SUCCESS != ret) {
		/* reset the data circuit so DAT_INHIBIT does not wedge the
		 * next command */
		DWMSHC_WRITE_BYTE(base_addr, DWMSHC_SW_RST_OFFSET, DWMSHC_SW_RST_DAT);
		dwc_mshc_udelay(1000);
	}

	return ret;
}

int dwc_mshc_poll_transfer(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd_data_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret = DWCMSHC_SUCCESS;

	if (cmd_data_p->flag & DWCMSHC_CMD_FLAG_EXP_DATA) {
		ret = dwc_mshc_transfer_data(inst, cmd_data_p);
		if (DWCMSHC_SUCCESS != ret) {
			DWCMSHC_ERROR("transfer data failed %d", ret);
			return ret;
		}
	} else {
		ret = dwc_mshc_send_command(base_addr, cmd_data_p);
		if (DWCMSHC_SUCCESS != ret) {
			DWCMSHC_ERROR("send cmd failed %d", ret);
			return ret;
		}
	}

	return ret;
}

int dwc_mshc_interrupt_transfer(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd_data_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret = DWCMSHC_SUCCESS;
	uint32_t loop;

	if (cmd_data_p->flag & DWCMSHC_CMD_FLAG_EXP_DATA) {
		ret = dwc_mshc_prepare_dma(inst, cmd_data_p);
		if (DWCMSHC_SUCCESS != ret) {
			return ret;
		}
		inst->cur_is_write =
			(0U != (cmd_data_p->flag & DWCMSHC_CMD_FLAG_WRITE_DATA));
	} else {
		inst->cur_is_write = false;
	}

	/* wait for command inhibit to clear (PSTATE bit0) */
	loop = DWCMSHC_TIMEOUT_MS;
	while (DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET) &
	       DWMSHC_PSTATE_CMD_INHIBIT) {
		if (0U == --loop) {
			DWCMSHC_ERROR("timeout on CMD inhibit (pstate=0x%x)",
				      DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET));
			return DWCMSHC_ERR_TIMEOUT;
		}
		dwc_mshc_udelay(100);
	}

	/* clear pending status, make sure signal enables are on (status
	 * enables were all turned on at init) */
	dwc_mshc_clear_interrupt_status(base_addr);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF,
			  DWMSHC_NINT_CMD_COMPLETE | DWMSHC_NINT_XFER_COMPLETE |
				  DWMSHC_NINT_DMA_INTERRUPT);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_SIG_OFF, DWMSHC_EINT_ALL);

	/* issue the command */
	DWMSHC_WRITE_REG(base_addr, DWMSHC_ARGUMENT_OFFSET, cmd_data_p->cmdarg);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CMD_OFFSET,
			  (uint16_t)dwc_mshc_make_raw_cmd(cmd_data_p));

	return ret;
}

static int dwc_mshc_tuning_transfer(uintptr_t base_addr, uint32_t tuning_cmd,
				    uint8_t *buf, uint32_t blk_size)
{
	uint32_t words = blk_size / 4U;
	uint32_t stat;
	uint32_t err;
	uint32_t loop;
	uint32_t n;

	/* wait for cmd/data inhibit to clear */
	loop = DWCMSHC_TIMEOUT_MS;
	while (DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET) &
	       (DWMSHC_PSTATE_CMD_INHIBIT | DWMSHC_PSTATE_DAT_INHIBIT)) {
		if (0U == --loop) {
			DWCMSHC_ERROR("tuning CMD inhibit timeout (pstate=0x%x)",
				      DWMSHC_READ_REG(base_addr, DWMSHC_PSTATE_OFFSET));
			return DWCMSHC_ERR_TIMEOUT;
		}
		dwc_mshc_udelay(100);
	}

	dwc_mshc_clear_interrupt_status(base_addr);

	/* PIO: block size (with 512KiB boundary) + read-only transfer mode
	 * (no DMA), u-boot sdhci_send_tuning same */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_BLOCKSIZE_OFFSET,
			  (uint16_t)(blk_size | (7U << 12)));
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_XFER_MODE_OFFSET,
			  DWMSHC_XFER_DATA_DIR_READ);
	DWMSHC_WRITE_REG(base_addr, DWMSHC_ARGUMENT_OFFSET, 0U);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_CMD_OFFSET,
			  DWMSHC_CMD_INDEX(tuning_cmd) | DWMSHC_CMD_RESP_TYPE_48 |
				  DWMSHC_CMD_CRC_CHK | DWMSHC_CMD_IDX_CHK |
				  DWMSHC_CMD_DATA_PRESENT);

	/* read the data word by word as BUF_RD_READY arrives */
	for (n = 0U; n < words; n++) {
		loop = DWCMSHC_TIMEOUT_MS;
		do {
			stat = DWMSHC_READ_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF);
			err = DWMSHC_READ_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF);

			if (err & (DWMSHC_CMD_ERR_FLAGS | DWMSHC_DATA_ERR_FLAGS)) {
				DWCMSHC_ERROR("tuning CMD %u error: eint=0x%x",
					      tuning_cmd, err);
				DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
						  (uint16_t)stat);
				DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF,
						  (uint16_t)err);
				return DWCMSHC_ERR_CMD_FAILED;
			}

			if (stat & DWMSHC_NINT_BUF_RD_READY) {
				DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
						  DWMSHC_NINT_BUF_RD_READY);
				break;
			}

			dwc_mshc_udelay(100);
		} while (--loop);

		if (0U == loop) {
			DWCMSHC_ERROR("tuning BUF_RD_READY timeout (nint=0x%x eint=0x%x)",
				      stat, err);
			return DWCMSHC_ERR_TIMEOUT;
		}

		((uint32_t *)(void *)buf)[n] =
			DWMSHC_READ_REG(base_addr, DWMSHC_BUF_DATA_OFFSET);
	}

	/* clear leftover status */
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF, DWMSHC_NINT_ALL);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, DWMSHC_EINT_ALL);

	return DWCMSHC_SUCCESS;
}

/* HS200/HS400 tuning: set EXEC_TUNING, repeat CMD21 (up to 40 attempts,
 * u-boot MAX_TUNING_LOOP), the controller clears EXEC_TUNING when done -
 * TUNED_CLK=1 means success. */
int dwc_mshc_execute_tuning(struct dwc_mshc *inst, uint32_t tuning_cmd,
			    uint8_t *buf, uint32_t blk_size)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t ctrl2;
	uint32_t i;

	/* enable EXEC_TUNING */
	ctrl2 = DWMSHC_READ_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET);
	ctrl2 |= DWMSHC_HC2_EXEC_TUNING;
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET, (uint16_t)ctrl2);

	for (i = 0U; i < 40U; i++) {
		if (DWCMSHC_SUCCESS !=
		    dwc_mshc_tuning_transfer(base_addr, tuning_cmd, buf, blk_size)) {
			break;
		}

		ctrl2 = DWMSHC_READ_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET);
		if (0U == (ctrl2 & DWMSHC_HC2_EXEC_TUNING)) {
			if (ctrl2 & DWMSHC_HC2_TUNED_CLK) {
				DWCMSHC_INFO("tuning success after %u attempts", i + 1U);
				return DWCMSHC_SUCCESS;
			}
			break;
		}
	}

	DWCMSHC_ERROR("tuning failed (%u attempts)", i);
	/* clear EXEC_TUNING; falling back is the caller's decision */
	ctrl2 = DWMSHC_READ_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET);
	ctrl2 &= ~(uint32_t)DWMSHC_HC2_EXEC_TUNING;
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_HOST_CTRL2_OFFSET, (uint16_t)ctrl2);
	return DWCMSHC_ERR_CMD_FAILED;
}

uint32_t dwc_mshc_get_interrupt_mask(struct dwc_mshc *inst)
{
	/* the signal-enable mask is what interrupt mode arms */
	return DWMSHC_READ_HALF(inst->config.base_addr, DWMSHC_NORMAL_INT_SIG_OFF);
}

void dwc_mshc_set_interrupt_mask(struct dwc_mshc *inst, uint32_t mask, bool enable)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t old_mask =
		DWMSHC_READ_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF);
	uint32_t new_mask;

	if (enable) {
		new_mask = old_mask | mask;
	} else {
		new_mask = old_mask & (~mask);
	}

	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF, (uint16_t)new_mask);
}

void dwc_mshc_enable_interrupt_mode(struct dwc_mshc *inst, bool enable)
{
	uintptr_t base_addr = inst->config.base_addr;

	if (enable) {
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF,
				  DWMSHC_NINT_CMD_COMPLETE |
					  DWMSHC_NINT_XFER_COMPLETE |
					  DWMSHC_NINT_DMA_INTERRUPT);
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_SIG_OFF, DWMSHC_EINT_ALL);
	} else {
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_SIG_OFF, 0);
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_SIG_OFF, 0);
	}
}

void dwc_mshc_register_event_handler(struct dwc_mshc *inst, uint32_t event,
				     dwc_mshc_event_handler_t handler, void *args)
{
	inst->evt_handler[event] = handler;
	inst->evt_args[event] = args;
}

/* Interrupt dispatch: command complete, transfer complete, SDMA boundary,
 * and the error summary. Status is cleared as consumed. */
void dwc_mshc_interrupt_handler(void *param)
{
	struct dwc_mshc *inst = (struct dwc_mshc *)param;
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t stat;
	uint32_t err;
	dwc_mshc_event_handler_t handler;

	stat = DWMSHC_READ_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF);
	err = DWMSHC_READ_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF);

	if (stat & DWMSHC_NINT_CMD_COMPLETE) {
		handler = inst->evt_handler[DWCMSHC_EVT_CMD_DONE];
		if (handler != NULL) {
			handler(inst->evt_args[DWCMSHC_EVT_CMD_DONE]);
		}
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
				  DWMSHC_NINT_CMD_COMPLETE);
	}

	if (stat & DWMSHC_NINT_XFER_COMPLETE) {
		handler = inst->evt_handler[inst->cur_is_write
						    ? DWCMSHC_EVT_DATA_WRITE_DONE
						    : DWCMSHC_EVT_DATA_READ_DONE];
		if (handler != NULL) {
			handler(inst->evt_args[inst->cur_is_write
						       ? DWCMSHC_EVT_DATA_WRITE_DONE
						       : DWCMSHC_EVT_DATA_READ_DONE]);
		}
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
				  DWMSHC_NINT_XFER_COMPLETE);
	}

	if (stat & DWMSHC_NINT_DMA_INTERRUPT) {
		uint32_t transferred =
			(inst->cur_dma_boundary + 1U) * (512U * 1024U);

		DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF,
				  DWMSHC_NINT_DMA_INTERRUPT);
		if (transferred < inst->cur_dma_len) {
			DWMSHC_WRITE_REG(base_addr, DWMSHC_SDMASA_OFFSET,
					 (uint32_t)(inst->cur_dma_phy + transferred));
			inst->cur_dma_boundary++;
		}
	}

	if (err & (DWMSHC_CMD_ERR_FLAGS | DWMSHC_DATA_ERR_FLAGS)) {
		DWCMSHC_ERROR("interrupt error: eint=0x%x", err);
		handler = inst->evt_handler[DWCMSHC_EVT_DATA_ERROR];
		if (handler != NULL) {
			handler(inst->evt_args[DWCMSHC_EVT_DATA_ERROR]);
		}
		DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, (uint16_t)err);
	}
}

bool dwc_mshc_data_busy(struct dwc_mshc *inst)
{
	/* SDHCI: DAT[0] driven low = card busy */
	return (0U == (DWMSHC_READ_REG(inst->config.base_addr, DWMSHC_PSTATE_OFFSET) &
		       DWMSHC_PSTATE_DATA_0_LVL));
}
