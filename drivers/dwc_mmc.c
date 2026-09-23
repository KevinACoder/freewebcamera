/*
 * @file   dwc_mmc.c
 * @brief  DW-MMC host controller driver core (the sdmmc0 slot on this board,
 *         currently carrying an SDIO card), with IDMAC data transfers in
 *         polling and interrupt modes.
 *
 * PROVENANCE AND PORTING POLICY (decision D27). The register sequences - CRU
 * gate/reset/source-clock handling, the three-step CIU clock update with
 * UPD_CLK|WAIT_PRV_DAT (and its VOLT_SWITCH variant across CMD11), the CMD11
 * handshake that completes through the volt_switch event instead of CDONE,
 * the IDMAC descriptor chain, and every poll loop - are the author's own
 * RK3568 port, debugged on this board in the standalone line and carried
 * over line-for-line. Sequences are the truth; only the surrounding
 * vocabulary changed:
 *
 *   - register/cache primitives are this project's (regs.h, board.h); the
 *     halfword accessors regs.h gained for the SDHCI sibling apply here too;
 *   - the SDK's non-cacheable DMA area is replaced by static .bss descriptor
 *     lists with explicit board_dcache_flush/invalidate at the points where
 *     the device and the CPU exchange the lists and buffers (the identity
 *     map makes address == bus address);
 *   - fsleep_* become counter-timer busy waits (no kernel call, so the K4
 *     stub build needs nothing here);
 *   - the SDK read-only-config blocks are dropped: this milestone writes
 *     storage through the eMMC controller, and the sdmmc0 slot carries no
 *     storage at all.
 *
 * Interrupt mode is carried over intact (handler, event routing, the
 * re-arm-free mask discipline) but runs disabled: the adapter drives this
 * controller in polling mode, the same shape the AHCI and NVMe drivers
 * shipped in.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "regs.h"

#include "dwmmc.h"
#include "dwc_mmc_regs.h"

/* --- constants -------------------------------------------------------------- */

#define DWMMC_TIMEOUT_MS	1000U	/* command wait budget */
#define DWMMC_DATA_TIMEOUT_MS	3000U	/* data wait budget */
#define DWMMC_RESET_RETRIES	500U

/* Set to log every command/data transfer (boot debugging only: the console
 * line rate makes this unusable for bulk transfers). */
#ifndef DWMMC_VERBOSE
#define DWMMC_VERBOSE		0
#endif

#define DWMMC_ERROR(fmt, ...)	board_log("dwmmc: " fmt "\n", ##__VA_ARGS__)
#define DWMMC_INFO(fmt, ...) \
	do { \
		if (DWMMC_VERBOSE) { \
			board_log("dwmmc: " fmt "\n", ##__VA_ARGS__); \
		} \
	} while (0)

/* Bounded busy wait on the system counter (CNTPTC runs at cntfrq; readable
 * at EL1, no kernel involvement - the K4 stub build links this unchanged). */
static void dwc_udelay(uint32_t us)
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

/* Rockchip CRU hiword write-enable store. */
static void dwc_mmc_rk_clrset(uintptr_t reg, uint32_t mask, uint32_t val)
{
	reg_wr32(reg, ((mask & 0xFFFFU) << 16) | (val & mask));
}


/* Card presence gate. cd_broken slots (soldered SDIO modules) are treated as
 * always present: their CDETECT floats and would fail every gate. */
static bool dwc_mmc_cd_ok(const struct dwc_mmc *inst)
{
	return inst->config.cd_broken || dwc_mmc_card_exists(inst->config.base_addr);
}

/* Per-instance IDMAC descriptor lists (256 entries x 16 B = 4 KiB each).
 * These are the CPU-device shared memory of this driver: every build of the
 * chain is flushed before the doorbell, reads happen only from the device's
 * own status words via the controller, so no invalidation path exists. */
static __attribute__((aligned(64))) struct dwc_mmc_idma_desc
	dwc_mmc_desc[2][DWMMC_DMA_DESC_MAX_NUM];

/**
 * CRU bring-up for one controller: enable HCLK/CLK gates, release the soft
 * reset, pick the 50M source (the 400 kHz identity clock comes from the CIU
 * divider, matching u-boot).
 */
static int dwc_mmc_cru_clock_init(struct dwc_mmc *inst)
{
	uintptr_t cru = RK3568_CRU_BASE_ADDR;
	uint32_t gate_mask, rst_mask, mux_mask, mux_shift;

	if (0U == inst->config.instance_id) {
		gate_mask = DWMMC_CRU_SDMMC0_HCLK_GATE | DWMMC_CRU_SDMMC0_CLK_GATE;
		rst_mask = DWMMC_CRU_SDMMC0_SOFTRST;
		mux_mask = DWMMC_CRU_SDMMC0_MUX_MASK;
		mux_shift = DWMMC_CRU_SDMMC0_MUX_SHIFT;
	} else {
		gate_mask = DWMMC_CRU_SDMMC1_HCLK_GATE | DWMMC_CRU_SDMMC1_CLK_GATE;
		rst_mask = DWMMC_CRU_SDMMC1_SOFTRST;
		mux_mask = DWMMC_CRU_SDMMC1_MUX_MASK;
		mux_shift = DWMMC_CRU_SDMMC1_MUX_SHIFT;
	}

	/* gate bit = 1 means the clock is off; write 0 to enable. */
	dwc_mmc_rk_clrset(cru + DWMMC_CRU_GATE_CON_OFF, gate_mask, 0);
	/* release the soft reset (assert = write 1, release = write 0) */
	dwc_mmc_rk_clrset(cru + DWMMC_CRU_SOFTRST_CON_OFF, rst_mask, 0);
	/* source clock: 50M */
	dwc_mmc_rk_clrset(cru + DWMMC_CRU_CLKSEL_CON_OFF, mux_mask,
			  DWMMC_CRU_SDMMC_SEL_50M << mux_shift);
	inst->cur_source_clock = 50000000U;

	return DWMMC_SUCCESS;
}

/**
 * Pick the CRU source and the CIU divider for a target card clock:
 * card_clk = source / (2 * clk_div), CLKDIV = 0 bypasses.
 */
static int dwc_mmc_select_clock_source(struct dwc_mmc *inst, uint32_t target,
				       uint32_t *clk_div)
{
	uintptr_t cru = RK3568_CRU_BASE_ADDR;
	uint32_t mux_mask, mux_shift;
	uint32_t sel;
	uint32_t src_rate;

	if (0U == inst->config.instance_id) {
		mux_mask = DWMMC_CRU_SDMMC0_MUX_MASK;
		mux_shift = DWMMC_CRU_SDMMC0_MUX_SHIFT;
	} else {
		mux_mask = DWMMC_CRU_SDMMC1_MUX_MASK;
		mux_shift = DWMMC_CRU_SDMMC1_MUX_SHIFT;
	}

	if (target <= 750000U) {
		/* 400 kHz identity: 750k source, bypass (u-boot behaviour) */
		sel = DWMMC_CRU_SDMMC_SEL_750K;
		src_rate = 750000U;
		*clk_div = 0U;
	} else if (target <= 50000000U) {
		/* 25M/50M/52M: 50M source; 25M takes CLKDIV=1, 50M bypasses */
		sel = DWMMC_CRU_SDMMC_SEL_50M;
		src_rate = 50000000U;
		*clk_div = (target >= 50000000U) ? 0U : 1U;
	} else if (target <= 100000000U) {
		/* SDR50: 100M source, bypass */
		sel = DWMMC_CRU_SDMMC_SEL_100M;
		src_rate = 100000000U;
		*clk_div = 0U;
	} else {
		/* SDR104: 300M source, CLKDIV=1 -> 150MHz (300M/(2*1)) */
		sel = DWMMC_CRU_SDMMC_SEL_300M;
		src_rate = 300000000U;
		*clk_div = 1U;
	}

	dwc_mmc_rk_clrset(cru + DWMMC_CRU_CLKSEL_CON_OFF, mux_mask,
			  sel << mux_shift);
	inst->cur_source_clock = src_rate;

	DWMMC_INFO("CLK target=%u -> source=%u(%u) div=%u", target, sel,
		   src_rate, *clk_div);
	return DWMMC_SUCCESS;
}

/**
 * Three-step clock parameter update (TRM 6.6.4.3): stop clocks -> write the
 * divider -> re-enable, each step issued to the CIU as an update_clk_regs_only
 * command. While a CMD11 voltage switch is in flight every UPD_CLK must carry
 * VOLT_SWITCH (linux dw_mci_setup_bus: "must continue to set bit 28").
 */
static int dwc_mmc_clock_update(struct dwc_mmc *inst, uint32_t clk_div)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t cmd_bits = DWMMC_CMD_UPD_CLK | DWMMC_CMD_WAIT_PRV_DAT;
	uint32_t status;
	uint32_t loop;

	if (inst->in_volt_switch) {
		cmd_bits |= DWMMC_CMD_VOLT_SWITCH;
	}

	/* Rockchip drive/sample phases, the vendor-kernel set_ios values
	 * (drive 90deg for hold time, sample 0deg - rk356x.dtsi sets no
	 * default-sample-phase). M11 board round 2: CMD53 data ran DCRC
	 * storms at HS50 until these were programmed - the sdmmc0 data
	 * phase had never been driven before. */
	DWMMC_WRITE_REG(base_addr, DWMMC_TIMING_CON0_OFFSET,
			DWMMC_TIMING_CON_DRIVE_90);
	DWMMC_WRITE_REG(base_addr, DWMMC_TIMING_CON1_OFFSET,
			DWMMC_TIMING_CON_SAMPLE_0);

	/* 1) stop all clocks */
	DWMMC_WRITE_REG(base_addr, DWMMC_CLKENA_OFFSET, 0);
	DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET, cmd_bits | DWMMC_CMD_START);
	loop = DWMMC_RESET_RETRIES;
	do {
		status = DWMMC_READ_REG(base_addr, DWMMC_CMD_OFFSET);
		if (0U == (status & DWMMC_CMD_START)) {
			break;
		}
		dwc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		return DWMMC_ERR_TIMEOUT;
	}

	/* 2) update the divider */
	DWMMC_WRITE_REG(base_addr, DWMMC_CLKDIV_OFFSET, clk_div);
	DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET, cmd_bits | DWMMC_CMD_START);
	loop = DWMMC_RESET_RETRIES;
	do {
		status = DWMMC_READ_REG(base_addr, DWMMC_CMD_OFFSET);
		if (0U == (status & DWMMC_CMD_START)) {
			break;
		}
		dwc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		return DWMMC_ERR_TIMEOUT;
	}

	/* 3) re-enable the clock */
	DWMMC_WRITE_REG(base_addr, DWMMC_CLKENA_OFFSET,
			DWMMC_CLKEN_ENABLE | DWMMC_CLKEN_LOW_PWR);
	DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET, cmd_bits | DWMMC_CMD_START);
	loop = DWMMC_RESET_RETRIES;
	do {
		status = DWMMC_READ_REG(base_addr, DWMMC_CMD_OFFSET);
		if (0U == (status & DWMMC_CMD_START)) {
			break;
		}
		dwc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		return DWMMC_ERR_TIMEOUT;
	}

	return DWMMC_SUCCESS;
}

int dwc_mmc_software_reset(uintptr_t base_addr, int retries)
{
	uint32_t ctrl;

	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET, DWMMC_CTRL_ALL_RESET_FLAGS);
	while (retries-- > 0) {
		ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);
		if (0U == (ctrl & DWMMC_CTRL_ALL_RESET_FLAGS)) {
			return DWMMC_SUCCESS;
		}
	}

	return DWMMC_ERR_TIMEOUT;
}

int dwc_mmc_initialize(struct dwc_mmc *inst, const struct dwc_mmc_config *config)
{
	uintptr_t base_addr;
	int ret = DWMMC_SUCCESS;

	if (inst->is_ready != 0U) {
		DWMMC_ERROR("device is already initialized");
	}

	dwc_mmc_deinitialize(inst);
	inst->config = *config;
	base_addr = inst->config.base_addr;

	/* static descriptor list (the CPU-device shared memory of this driver) */
	inst->dma_desc = dwc_mmc_desc[inst->config.instance_id];
	inst->dma_desc_num = DWMMC_DMA_DESC_MAX_NUM;

	/* board: CRU gates / soft reset / source clock */
	(void)dwc_mmc_cru_clock_init(inst);

	/* controller soft reset */
	ret = dwc_mmc_software_reset(base_addr, DWMMC_RESET_RETRIES);
	if (DWMMC_SUCCESS != ret) {
		return ret;
	}

	/* global interrupt enable + internal DMAC */
	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET,
			DWMMC_CTRL_INT_ENABLE | DWMMC_CTRL_USE_IDMAC);

	/* clear pending interrupts */
	dwc_mmc_clear_interrupt_status(base_addr);

	/* FIFO thresholds: fifo-depth = 0x100, TX wmark 0x80, RX wmark 0x7f,
	 * MSIZE = 6 */
	DWMMC_WRITE_REG(base_addr, DWMMC_FIFOTH_OFFSET,
			DWMMC_FIFOTH_MSIZE(6) | DWMMC_FIFOTH_RX_WMARK(0x7FU) |
				DWMMC_FIFOTH_TX_WMARK(0x80U));

	/* enable card power */
	DWMMC_WRITE_REG(base_addr, DWMMC_PWREN_OFFSET, 1U);

	/* wait for the power ramp (TRM 6.6.4.1: enable power -> wait) */
	dwc_udelay(10000U);

	/* initial 400 kHz clock */
	(void)dwc_mmc_set_card_clk(inst, 400000U);

	/* let the clock settle before the first command (CMD0 times out
	 * without this delay - measured on this board) */
	dwc_udelay(2000U);

	/* card detect */
	if (!dwc_mmc_cd_ok(inst)) {
		DWMMC_ERROR("storage device not found (base 0x%lx)",
			    (unsigned long)base_addr);
		return DWMMC_ERR_CARD_NO_FOUND;
	}

	inst->is_ready = 1U;

	return DWMMC_SUCCESS;
}

void dwc_mmc_deinitialize(struct dwc_mmc *inst)
{
	inst->is_ready = 0U;
	memset(inst, 0, sizeof(*inst));
}

int dwc_mmc_set_card_clk(struct dwc_mmc *inst, uint32_t clk_freq_hz)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t clk_div = 0U;
	int ret;

	if (0U == clk_freq_hz) {
		/* stop the clock (voltage-switch sequencing, TRM 6.6.4.3) */
		uint32_t loop = DWMMC_RESET_RETRIES;
		uint32_t bits = DWMMC_CMD_UPD_CLK | DWMMC_CMD_WAIT_PRV_DAT;

		if (inst->in_volt_switch) {
			bits |= DWMMC_CMD_VOLT_SWITCH;
		}

		DWMMC_WRITE_REG(base_addr, DWMMC_CLKENA_OFFSET, 0);
		DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET, bits | DWMMC_CMD_START);
		do {
			if (0U == (DWMMC_READ_REG(base_addr, DWMMC_CMD_OFFSET) &
				   DWMMC_CMD_START)) {
				break;
			}
			dwc_udelay(100);
		} while (--loop);
		return DWMMC_SUCCESS;
	}

	if (!dwc_mmc_cd_ok(inst)) {
		return DWMMC_SUCCESS; /* no card: nothing to clock */
	}

	ret = dwc_mmc_select_clock_source(inst, clk_freq_hz, &clk_div);
	if (DWMMC_SUCCESS != ret) {
		DWMMC_ERROR("select_clock_source(%u) failed: %d", clk_freq_hz, ret);
		return ret;
	}

	ret = dwc_mmc_clock_update(inst, clk_div);
	if (DWMMC_SUCCESS != ret) {
		DWMMC_ERROR("clock_update(%u, div=%u) failed: %d cmd=0x%08x",
			    clk_freq_hz, clk_div, ret,
			    DWMMC_READ_REG(base_addr, DWMMC_CMD_OFFSET));
		return ret;
	}

	/* the clock coming back ends the voltage-switch window */
	inst->in_volt_switch = false;

	/* let the CIU/card settle after the switch */
	dwc_udelay(1000U);

	DWMMC_INFO("BUS CLOCK: %u Hz, source %u, div %u", clk_freq_hz,
		   inst->cur_source_clock, clk_div);
	return ret;
}

int dwc_mmc_set_signal_voltage(struct dwc_mmc *inst, bool v18)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t uhs = DWMMC_READ_REG(base_addr, DWMMC_UHSREG_OFFSET);

	if (v18) {
		uhs |= DWMMC_UHSREG_18V;
	} else {
		uhs &= ~(uint32_t)DWMMC_UHSREG_18V;
	}
	DWMMC_WRITE_REG(base_addr, DWMMC_UHSREG_OFFSET, uhs);

	return DWMMC_SUCCESS;
}

void dwc_mmc_set_card_bus_width(struct dwc_mmc *inst, uint32_t bus_width)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t ctype = DWMMC_CTYPE_1BIT;

	if (8U == bus_width) {
		ctype = DWMMC_CTYPE_8BIT;
	} else if (4U == bus_width) {
		ctype = DWMMC_CTYPE_4BIT;
	}

	DWMMC_WRITE_REG(base_addr, DWMMC_CTYPE_OFFSET, ctype);
}

/**
 * Sample clock phase (CRU sdmmc0_con[1]/sdmmc1_con[1], shift 1). The format
 * matches linux clk-mmc-phase.c: raw = (delay_sel<<10)|(delay_num<<2)|deg90,
 * written as ((0x7ff) << 17) | (raw << 1) (hiword enable bits [17:1]).
 */
static void dwc_mmc_set_sample_phase(struct dwc_mmc *inst, uint32_t degrees)
{
	uintptr_t cru = RK3568_CRU_BASE_ADDR;
	uint32_t reg_off = (0U == inst->config.instance_id)
				   ? DWMMC_CRU_SDMMC0_PHASE_CON_OFF
				   : DWMMC_CRU_SDMMC1_PHASE_CON_OFF;
	uint32_t nineties = degrees / 90U;
	uint32_t remainder = degrees % 90U;
	uint32_t delay_num = 0U;
	uint32_t raw_value;
	uint32_t rate;

	if (0U != remainder) {
		/* delay unit ~= 60ps; rate = ciu/2 (linux formula) */
		rate = inst->cur_source_clock / 2U;
		delay_num = (uint32_t)((10000000ULL * (uint64_t)remainder) /
				       (((uint64_t)rate / 1000U) * 36U * 6U));
		if (delay_num > 255U) {
			delay_num = 255U;
		}
	}

	raw_value = (delay_num ? (1u << 10) : 0U) | (delay_num << 2) | nineties;
	reg_wr32(cru + reg_off, ((0x07FFU << (1U + 16U)) | (raw_value << 1)));
}

/**
 * Sample-phase tuning for SDR50/SDR104: sweep 0..270 degrees, issue the
 * tuning command once per step, pick the middle of the longest passing run.
 */
int dwc_mmc_sample_tuning(struct dwc_mmc *inst, uint32_t tuning_cmd,
			  uint8_t *buf, uint32_t blk_size)
{
	struct dwc_mmc_cmd cmd;
	struct dwc_mmc_data data;
	static const uint32_t phases[10] = { 0U, 30U, 60U, 90U, 120U,
					     150U, 180U, 210U, 240U, 270U };
	uint32_t best_start = 0U;
	uint32_t best_len = 0U;
	uint32_t cur_start = 0U;
	uint32_t cur_len = 0U;
	uint32_t good_phase = 0U;
	uint32_t i;
	bool good[10] = { false };

	/* tuning sweeps with interrupts masked so no handler steals status */
	DWMMC_WRITE_REG(inst->config.base_addr, DWMMC_INTMASK_OFFSET, 0);

	for (i = 0U; i < 10U; i++) {
		dwc_mmc_set_sample_phase(inst, phases[i]);
		dwc_udelay(100);

		memset(&cmd, 0, sizeof(cmd));
		memset(&data, 0, sizeof(data));
		cmd.cmdidx = tuning_cmd;
		cmd.cmdarg = 0U;
		cmd.flag = DWMMC_CMD_FLAG_EXP_RESP | DWMMC_CMD_FLAG_NEED_RESP_CRC |
			   DWMMC_CMD_FLAG_EXP_DATA | DWMMC_CMD_FLAG_READ_DATA;
		data.buf = buf;
		data.blksz = blk_size;
		data.blkcnt = 1U;
		data.datalen = blk_size;
		cmd.data_p = &data;

		if (DWMMC_SUCCESS == dwc_mmc_poll_transfer(inst, &cmd)) {
			good[i] = true;
		}
	}

	/* longest passing run */
	for (i = 0U; i < 10U; i++) {
		if (good[i]) {
			if (0U == cur_len) {
				cur_start = i;
			}
			cur_len++;
		} else {
			if (cur_len > best_len) {
				best_len = cur_len;
				best_start = cur_start;
			}
			cur_len = 0U;
		}
	}
	if (cur_len > best_len) {
		best_len = cur_len;
		best_start = cur_start;
	}

	if (0U == best_len) {
		DWMMC_ERROR("tuning failed: no good sample phase");
		return DWMMC_ERR_CMD_FAILED;
	}

	good_phase = phases[best_start + best_len / 2U];
	dwc_mmc_set_sample_phase(inst, good_phase);
	DWMMC_INFO("tuning done: good range %u-%u, select phase %u",
		   phases[best_start], phases[best_start + best_len - 1U],
		   good_phase);

	return DWMMC_SUCCESS;
}

/**
 * Assemble the CMD register value from a command descriptor (START is added
 * by the caller, last).
 */
static uint32_t dwc_mmc_make_raw_cmd(struct dwc_mmc_cmd *cmd_p)
{
	uint32_t raw_cmd = DWMMC_CMD_INDX(cmd_p->cmdidx);

	if (11U == cmd_p->cmdidx) {
		/* CMD11 voltage switch: volt_switch (bit 28) is mandatory, and
		 * the following UPD_CLK must keep it until the switch finishes
		 * (linux dw_mci_setup_bus). */
		raw_cmd |= DWMMC_CMD_VOLT_SWITCH;
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_NEED_INIT) {
		raw_cmd |= DWMMC_CMD_SEND_INIT;
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_NEED_STOP) {
		raw_cmd |= DWMMC_CMD_STOP_ABORT;
	} else if (!(cmd_p->flag & DWMMC_CMD_FLAG_EXP_DATA)) {
		/* A CMD11 must not carry wait_prvdata_complete: the handshake
		 * holds DAT low and the wait would deadlock. linux sets
		 * PRV_DAT_WAIT only for commands with data. */
		if (11U != cmd_p->cmdidx) {
			raw_cmd |= DWMMC_CMD_WAIT_PRV_DAT;
		}
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_NEED_AUTO_STOP) {
		raw_cmd |= DWMMC_CMD_SEND_AUTO_STOP;
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_DATA) {
		raw_cmd |= DWMMC_CMD_DATA_EXP | DWMMC_CMD_WAIT_PRV_DAT;
		if (cmd_p->flag & DWMMC_CMD_FLAG_WRITE_DATA) {
			raw_cmd |= DWMMC_CMD_RW;
		}
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_RESP) {
		raw_cmd |= DWMMC_CMD_RESP_EXP;
		if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_LONG_RESP) {
			raw_cmd |= DWMMC_CMD_RESP_LENGTH;
		}
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_NEED_RESP_CRC) {
		raw_cmd |= DWMMC_CMD_CHECK_CRC;
	}

	return raw_cmd;
}

/* DW-MMC hands R2 out of RESP0..3 in descending word order. */
static void dwc_mmc_read_response(uintptr_t base_addr, struct dwc_mmc_cmd *cmd_p)
{
	if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_LONG_RESP) {
		cmd_p->response[0] = DWMMC_READ_REG(base_addr, DWMMC_RESP3_OFFSET);
		cmd_p->response[1] = DWMMC_READ_REG(base_addr, DWMMC_RESP2_OFFSET);
		cmd_p->response[2] = DWMMC_READ_REG(base_addr, DWMMC_RESP1_OFFSET);
		cmd_p->response[3] = DWMMC_READ_REG(base_addr, DWMMC_RESP0_OFFSET);
	} else {
		cmd_p->response[0] = DWMMC_READ_REG(base_addr, DWMMC_RESP0_OFFSET);
		cmd_p->response[1] = 0U;
		cmd_p->response[2] = 0U;
		cmd_p->response[3] = 0U;
	}
}

static int dwc_mmc_send_command(uintptr_t base_addr, struct dwc_mmc_cmd *cmd_p)
{
	uint32_t raw_cmd;
	uint32_t mask = 0U;
	uint32_t loop;

	/* wait for data busy to clear (STATUS bit 9) */
	loop = DWMMC_TIMEOUT_MS;
	while (DWMMC_READ_REG(base_addr, DWMMC_STATUS_OFFSET) &
	       DWMMC_STATUS_DATA_BUSY) {
		if (0U == --loop) {
			DWMMC_ERROR("timeout on data busy");
			return DWMMC_ERR_TIMEOUT;
		}
		dwc_udelay(100);
	}

	/* clear all interrupt status */
	dwc_mmc_clear_interrupt_status(base_addr);

	DWMMC_WRITE_REG(base_addr, DWMMC_CMDARG_OFFSET, cmd_p->cmdarg);

	raw_cmd = dwc_mmc_make_raw_cmd(cmd_p);
	raw_cmd |= DWMMC_CMD_USE_HOLD_REG | DWMMC_CMD_START;
	DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET, raw_cmd);

	if (11U == cmd_p->cmdidx) {
		/* CMD11: the controller does not raise CDONE before the switch
		 * completes; it raises the volt_switch event instead (RINTSTS
		 * bit 10, HTO slot). The R1 response is already posted when
		 * the event fires - read RESP0 and skip the CDONE wait. The
		 * stop-clock/re-clock handshaking belongs to the protocol
		 * layer; CDONE arrives after the switch. */
		loop = DWMMC_TIMEOUT_MS;
		do {
			mask = DWMMC_READ_REG(base_addr, DWMMC_RINTSTS_OFFSET);
			if (mask & DWMMC_INTMSK_HTO) {
				break;
			}
			dwc_udelay(100);
		} while (--loop);

		if (0U == loop) {
			DWMMC_ERROR("CMD 11 volt switch event timeout (rintsts=0x%x)",
				    mask);
			return DWMMC_ERR_TIMEOUT;
		}

		if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_RESP) {
			dwc_mmc_read_response(base_addr, cmd_p);
		}
		return DWMMC_SUCCESS;
	}

	/* wait for command completion */
	loop = DWMMC_TIMEOUT_MS;
	do {
		mask = DWMMC_READ_REG(base_addr, DWMMC_RINTSTS_OFFSET);
		if (mask & DWMMC_INTMSK_CDONE) {
			break;
		}
		dwc_udelay(100);
	} while (--loop);

	if (0U == loop) {
		DWMMC_ERROR("CMD %u timeout", cmd_p->cmdidx);
		DWMMC_ERROR("DBG ctrl=0x%08x clkena=0x%08x clkdiv=0x%08x clksrc=0x%08x",
			    DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET),
			    DWMMC_READ_REG(base_addr, DWMMC_CLKENA_OFFSET),
			    DWMMC_READ_REG(base_addr, DWMMC_CLKDIV_OFFSET),
			    DWMMC_READ_REG(base_addr, DWMMC_CLKSRC_OFFSET));
		DWMMC_ERROR("DBG status=0x%08x rintsts=0x%08x mintsts=0x%08x intmask=0x%08x",
			    DWMMC_READ_REG(base_addr, DWMMC_STATUS_OFFSET),
			    mask,
			    DWMMC_READ_REG(base_addr, DWMMC_MINTSTS_OFFSET),
			    DWMMC_READ_REG(base_addr, DWMMC_INTMASK_OFFSET));
		/* never W1C the SDIO card-interrupt bit from thread context:
		 * the DAT1 level belongs to the ISR/consumer pair */
		DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
				mask & ~DWMMC_INTMSK_SDIO_INT);
		return DWMMC_ERR_TIMEOUT;
	}

	if (mask & DWMMC_INTMSK_RTO) {
		DWMMC_INFO("CMD %u response timeout (expected for CMD55)", cmd_p->cmdidx);
		DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
				mask & ~DWMMC_INTMSK_SDIO_INT);
		return DWMMC_ERR_TIMEOUT;
	}

	if (mask & DWMMC_INTMSK_RE) {
		DWMMC_ERROR("CMD %u response error", cmd_p->cmdidx);
		DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
				mask & ~DWMMC_INTMSK_SDIO_INT);
		return DWMMC_ERR_CMD_FAILED;
	}

	if (cmd_p->flag & DWMMC_CMD_FLAG_EXP_RESP) {
		dwc_mmc_read_response(base_addr, cmd_p);
	}

	return DWMMC_SUCCESS;
}

/* Build the IDMAC descriptor chain for one transfer. Returns the descriptor
 * count, or 0 on a parameter error. Bus addresses: the identity map makes
 * the VA the bus address. */
static uint32_t dwc_mmc_setup_dma_descriptor(struct dwc_mmc_idma_desc *desc_list,
					     uint32_t desc_num,
					     uintptr_t desc_bus_addr,
					     uintptr_t buf_bus_addr,
					     uint32_t datalen)
{
	uint32_t desc_cnt = 0U;
	uint32_t remain = datalen;
	uintptr_t buf = buf_bus_addr;

	if ((0U == datalen) || (0U != (buf_bus_addr & 0x3U))) {
		return 0U;
	}

	while ((remain > 0U) && (desc_cnt < desc_num)) {
		uint32_t len = (remain > DWMMC_DMA_DESC_MAX_DATA_LEN)
				       ? DWMMC_DMA_DESC_MAX_DATA_LEN
				       : remain;
		struct dwc_mmc_idma_desc *desc = &desc_list[desc_cnt];

		desc->flags = DWMMC_IDMAC_DES0_OWN | DWMMC_IDMAC_DES0_CH;
		desc->cnt = len;
		desc->addr = (uint32_t)buf;
		if (0U == desc_cnt) {
			desc->flags |= DWMMC_IDMAC_DES0_FD;
		}
		if (len >= remain) {
			/* last descriptor */
			desc->flags |= DWMMC_IDMAC_DES0_LD;
			desc->next_addr = 0U;
		} else {
			desc->next_addr = (uint32_t)(desc_bus_addr +
						     (uintptr_t)(desc_cnt + 1U) *
							     (uintptr_t)sizeof(struct dwc_mmc_idma_desc));
		}

		remain -= len;
		buf += len;
		desc_cnt++;
	}

	return desc_cnt;
}

static int dwc_mmc_prepare_dma(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd_p)
{
	struct dwc_mmc_data *dat_p = cmd_p->data_p;
	uintptr_t base_addr = inst->config.base_addr;
	uintptr_t buf_bus = (uintptr_t)dat_p->buf;
	uintptr_t desc_bus = (uintptr_t)inst->dma_desc;
	uint32_t desc_cnt;
	uint32_t ctrl;

	if ((NULL == dat_p->buf) || (0U == dat_p->datalen)) {
		return DWMMC_ERR_INVALID_BUF;
	}

	if ((0U != (buf_bus & 0x3U)) || (0U != (dat_p->datalen & 0x3U))) {
		DWMMC_ERROR("buffer not 4-byte aligned for DMA");
		return DWMMC_ERR_INVALID_BUF;
	}

	/* build the chain, then hand it to the device: the descriptors are
	 * CPU-written shared memory, flush before the controller reads them */
	desc_cnt = dwc_mmc_setup_dma_descriptor(inst->dma_desc, inst->dma_desc_num,
						desc_bus, buf_bus, dat_p->datalen);
	if (0U == desc_cnt) {
		DWMMC_ERROR("setup DMA descriptor failed (len=%u, num=%u)",
			    dat_p->datalen, inst->dma_desc_num);
		return DWMMC_ERR_INVALID_BUF;
	}
	board_dcache_flush(desc_bus,
			   (unsigned long)desc_cnt * sizeof(struct dwc_mmc_idma_desc));
	/* a write transfer hands the buffer to the device as well */
	if (0U != (cmd_p->flag & DWMMC_CMD_FLAG_WRITE_DATA)) {
		board_dcache_flush(buf_bus, dat_p->datalen);
	}
	/* a read transfer hands the buffer to the device the other way:
	 * drop the CPU's lines covering it before the doorbell so no
	 * dirty line evicted during the DMA window can write back over
	 * the incoming data (M11 r3: block-mode CMD53 corruption) */
	if (0U != (cmd_p->flag & DWMMC_CMD_FLAG_READ_DATA)) {
		board_dcache_invalidate(buf_bus, dat_p->datalen);
	}

	/* FIFO reset and wait for self-clear */
	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET, DWMMC_CTRL_FIFO_RESET);
	ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);
	while (ctrl & DWMMC_CTRL_FIFO_RESET) {
		dwc_udelay(100);
		ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);
	}

	/* descriptor list base address */
	DWMMC_WRITE_REG(base_addr, DWMMC_DBADDR_OFFSET, (uint32_t)desc_bus);

	/* clear stale IDMAC status, then arm the IDMAC status enables: on
	 * this IP the IDSTS completion/error bits do not post unless the
	 * matching IDINTEN bits are set (NetBSD writes IDIE before its
	 * wait, Linux dw_mci_idmac_init likewise - poll mode read RI
	 * without ever arming it and saw nothing but zeroes) */
	DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET, DWMMC_INTMSK_ALL);
	DWMMC_WRITE_REG(base_addr, DWMMC_IDINTEN_OFFSET,
			DWMMC_IDMAC_INT_RI | DWMMC_IDMAC_INT_TI |
			DWMMC_IDMAC_INT_NI | DWMMC_IDMAC_INT_AI |
			DWMMC_IDMAC_ERR_FLAGS);

	/* CTRL: enable IDMAC + DMA (INT_ENABLE must ride along - the FIFO
	 * reset clears it) */
	ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);
	ctrl |= DWMMC_CTRL_USE_IDMAC | DWMMC_CTRL_DMA_ENABLE | DWMMC_CTRL_INT_ENABLE;
	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET, ctrl);

	/* BMOD: enable the IDMAC */
	ctrl = DWMMC_READ_REG(base_addr, DWMMC_BMOD_OFFSET);
	ctrl |= DWMMC_IDMAC_ENABLE | DWMMC_IDMAC_FB;
	DWMMC_WRITE_REG(base_addr, DWMMC_BMOD_OFFSET, ctrl);

	/* block size / byte count */
	DWMMC_WRITE_REG(base_addr, DWMMC_BLKSIZ_OFFSET, dat_p->blksz);
	DWMMC_WRITE_REG(base_addr, DWMMC_BYTCNT_OFFSET, dat_p->datalen);

	return DWMMC_SUCCESS;
}

/* Wait for the IDMAC to close the last descriptor of a read transfer:
 * DTO only means the controller consumed the wire data - the FIFO-to-RAM
 * drain trails it. RI is the bit NetBSD's dwc_mmc ISR waits on. */
static int dwc_mmc_wait_idmac_rx(uintptr_t base_addr)
{
	uint32_t idsts = 0U;
	uint32_t loop = DWMMC_DATA_TIMEOUT_MS;

	do {
		idsts = DWMMC_READ_REG(base_addr, DWMMC_IDSTS_OFFSET);

		if (0U != (idsts & DWMMC_IDMAC_ERR_FLAGS)) {
			DWMMC_ERROR("IDMAC error: 0x%x", idsts);
			break;
		}
		if (0U != (idsts & DWMMC_IDMAC_INT_RI)) {
			DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET,
					DWMMC_INTMSK_ALL);
			return DWMMC_SUCCESS;
		}

		dwc_udelay(100);
	} while (--loop);

	DWMMC_ERROR("IDMAC receive not complete (idsts=0x%x)", idsts);
	DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET, DWMMC_INTMSK_ALL);
	return DWMMC_ERR_TIMEOUT;
}

static int dwc_mmc_wait_data_over(uintptr_t base_addr, struct dwc_mmc_cmd *cmd_p)
{
	uint32_t mask;
	uint32_t loop = DWMMC_DATA_TIMEOUT_MS;
	int ret;

	do {
		mask = DWMMC_READ_REG(base_addr, DWMMC_RINTSTS_OFFSET);

		if (mask & (DWMMC_DATA_ERR_FLAGS | DWMMC_DATA_TOUT_FLAGS)) {
			DWMMC_ERROR("CMD %u data error: 0x%x", cmd_p->cmdidx, mask);
			DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
					mask & ~DWMMC_INTMSK_SDIO_INT);
			DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET, 0xFFFFFFFFU);
			return DWMMC_ERR_DATA_FAILED;
		}

		if (mask & DWMMC_INTMSK_DTO) {
			DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
					mask & ~DWMMC_INTMSK_SDIO_INT);
			if (0U != (cmd_p->flag & DWMMC_CMD_FLAG_READ_DATA)) {
				ret = dwc_mmc_wait_idmac_rx(base_addr);
				if (DWMMC_SUCCESS != ret) {
					return ret;
				}
				/* drop this core's stale copies before the
				 * consumer reads: pre-DMA invalidation ran on
				 * whatever core armed the transfer, the
				 * consumer may resume on another one */
				board_dcache_invalidate(
					(uintptr_t)cmd_p->data_p->buf,
					cmd_p->data_p->datalen);
			}
			return DWMMC_SUCCESS;
		}

		dwc_udelay(100);
	} while (--loop);

	DWMMC_ERROR("CMD %u data timeout", cmd_p->cmdidx);
	DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
			mask & ~DWMMC_INTMSK_SDIO_INT);
	return DWMMC_ERR_TIMEOUT;
}

static int dwc_mmc_transfer_data(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret;
	uint32_t ctrl;

	ret = dwc_mmc_prepare_dma(inst, cmd_p);
	if (DWMMC_SUCCESS != ret) {
		return ret;
	}

	ret = dwc_mmc_send_command(base_addr, cmd_p);
	if (DWMMC_SUCCESS != ret) {
		return ret;
	}

	ret = dwc_mmc_wait_data_over(base_addr, cmd_p);

	/* stop the DMA (the next transfer re-enables it) */
	ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);
	ctrl &= ~(uint32_t)DWMMC_CTRL_DMA_ENABLE;
	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET, ctrl);

	return ret;
}

int dwc_mmc_poll_transfer(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd_data_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret = DWMMC_SUCCESS;

	if (!dwc_mmc_cd_ok(inst)) {
		DWMMC_ERROR("card not found (base 0x%lx)", (unsigned long)base_addr);
		return DWMMC_ERR_CARD_NO_FOUND;
	}

	if (cmd_data_p->flag & DWMMC_CMD_FLAG_EXP_DATA) {
		DWMMC_INFO("====DATA [%u] START: buf %p len=%u====",
			   cmd_data_p->cmdidx,
			   cmd_data_p->data_p->buf, cmd_data_p->data_p->datalen);
		ret = dwc_mmc_transfer_data(inst, cmd_data_p);
		if (DWMMC_SUCCESS != ret) {
			DWMMC_ERROR("transfer data failed %d", ret);
			return ret;
		}
		DWMMC_INFO("====DATA [%u] END: %d====", cmd_data_p->cmdidx, ret);
	} else {
		DWMMC_INFO("=====CMD [%u] START=====", cmd_data_p->cmdidx);
		ret = dwc_mmc_send_command(base_addr, cmd_data_p);
		if (DWMMC_SUCCESS != ret) {
			DWMMC_ERROR("send cmd failed %d", ret);
			return ret;
		}
		if (11U == cmd_data_p->cmdidx) {
			/* CMD11 started: the following UPD_CLK carries
			 * VOLT_SWITCH until the clock comes back */
			inst->in_volt_switch = true;
		}
		DWMMC_INFO("=====CMD [%u] END: %d=====", cmd_data_p->cmdidx, ret);
	}

	return ret;
}

int dwc_mmc_restart(struct dwc_mmc *inst)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret;

	ret = dwc_mmc_software_reset(base_addr, DWMMC_RESET_RETRIES);
	if (DWMMC_SUCCESS != ret) {
		return ret;
	}

	DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET,
			DWMMC_CTRL_INT_ENABLE | DWMMC_CTRL_USE_IDMAC);
	dwc_mmc_clear_interrupt_status(base_addr);
	DWMMC_WRITE_REG(base_addr, DWMMC_FIFOTH_OFFSET,
			DWMMC_FIFOTH_MSIZE(6) | DWMMC_FIFOTH_RX_WMARK(0x7FU) |
				DWMMC_FIFOTH_TX_WMARK(0x80U));
	DWMMC_WRITE_REG(base_addr, DWMMC_PWREN_OFFSET, 1U);

	return DWMMC_SUCCESS;
}

int dwc_mmc_interrupt_transfer(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd_data_p)
{
	uintptr_t base_addr = inst->config.base_addr;
	int ret = DWMMC_SUCCESS;

	if (!dwc_mmc_cd_ok(inst)) {
		DWMMC_ERROR("card not found (base 0x%lx)", (unsigned long)base_addr);
		return DWMMC_ERR_CARD_NO_FOUND;
	}

	if (cmd_data_p->flag & DWMMC_CMD_FLAG_EXP_DATA) {
		ret = dwc_mmc_prepare_dma(inst, cmd_data_p);
		if (DWMMC_SUCCESS != ret) {
			return ret;
		}
		inst->cur_is_write =
			(0U != (cmd_data_p->flag & DWMMC_CMD_FLAG_WRITE_DATA));
	} else {
		inst->cur_is_write = false;
	}

	/* clear pending interrupts, then arm command-done / data-done / error */
	dwc_mmc_clear_interrupt_status(base_addr);
	if (11U == cmd_data_p->cmdidx) {
		/* CMD11: the volt_switch event (HTO bit 10) stands in for CDONE */
		DWMMC_WRITE_REG(base_addr, DWMMC_INTMASK_OFFSET,
				DWMMC_INTMSK_CDONE | DWMMC_INTMSK_HTO |
					DWMMC_INTMSK_RTO | DWMMC_INTMSK_RE |
					DWMMC_DATA_ERR_FLAGS | DWMMC_DATA_TOUT_FLAGS);
	} else {
		DWMMC_WRITE_REG(base_addr, DWMMC_INTMASK_OFFSET,
				DWMMC_INTMSK_CDONE | DWMMC_INTMSK_DTO |
					DWMMC_INTMSK_RTO | DWMMC_INTMSK_RE |
					DWMMC_DATA_ERR_FLAGS | DWMMC_DATA_TOUT_FLAGS);
	}

	/* issue the command */
	DWMMC_WRITE_REG(base_addr, DWMMC_CMDARG_OFFSET, cmd_data_p->cmdarg);
	DWMMC_WRITE_REG(base_addr, DWMMC_CMD_OFFSET,
			dwc_mmc_make_raw_cmd(cmd_data_p) | DWMMC_CMD_USE_HOLD_REG |
				DWMMC_CMD_START);

	if (11U == cmd_data_p->cmdidx) {
		inst->in_volt_switch = true;
	}

	return ret;
}

uint32_t dwc_mmc_get_interrupt_mask(struct dwc_mmc *inst)
{
	return DWMMC_READ_REG(inst->config.base_addr, DWMMC_INTMASK_OFFSET);
}

void dwc_mmc_set_interrupt_mask(struct dwc_mmc *inst, uint32_t mask, bool enable)
{
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t old_mask = DWMMC_READ_REG(base_addr, DWMMC_INTMASK_OFFSET);
	uint32_t new_mask;

	if (enable) {
		new_mask = old_mask | mask;
	} else {
		new_mask = old_mask & (~mask);
	}

	DWMMC_WRITE_REG(base_addr, DWMMC_INTMASK_OFFSET, new_mask);
}

void dwc_mmc_register_event_handler(struct dwc_mmc *inst, uint32_t event,
				    dwc_mmc_event_handler_t handler, void *args)
{
	inst->evt_handler[event] = handler;
	inst->evt_args[event] = args;
}

/* Interrupt dispatch: command done, data done (direction-routed), command
 * and data errors, and the IDMAC summary bits. Status is cleared once at the
 * end (write-one-clear); the DMA engine is stopped after a data event and
 * the next transfer re-arms it. */
void dwc_mmc_interrupt_handler(void *param)
{
	struct dwc_mmc *inst = (struct dwc_mmc *)param;
	uintptr_t base_addr = inst->config.base_addr;
	uint32_t status;
	uint32_t dma_status;
	bool data_event = false;
	bool error_event = false;
	dwc_mmc_event_handler_t handler;

	status = DWMMC_READ_REG(base_addr, DWMMC_RINTSTS_OFFSET);

	/* poll-transfer + SDIO-interrupt shape (M11 r4): the data path is
	 * polled by its owning thread, which also clears RINTSTS/IDSTS as
	 * it goes. The only event that may legally fire here is the SDIO
	 * card interrupt - touching anything else (a mid-flight CMD53's
	 * IDMAC RI, a CDONE/DTO the poll loop is about to consume) stops
	 * live DMA or steals its completion bits and hangs every transfer.
	 * The board showed exactly that: arm SDIO_INT and every CMD53
	 * times out. */
	if (DWMMC_READ_REG(base_addr, DWMMC_INTMASK_OFFSET) ==
	    DWMMC_INTMSK_SDIO_INT) {
		if (status & DWMMC_INTMSK_SDIO_INT) {
			dwc_mmc_set_interrupt_mask(inst, DWMMC_INTMSK_SDIO_INT,
						   false);
			handler = inst->evt_handler[DWMMC_EVT_SDIO_INT];
			if (handler != NULL) {
				handler(inst->evt_args[DWMMC_EVT_SDIO_INT]);
			}
			DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET,
					DWMMC_INTMSK_SDIO_INT);
		}
		return;
	}

	dma_status = DWMMC_READ_REG(base_addr, DWMMC_IDSTS_OFFSET);

	if (status & DWMMC_INTMSK_CDONE) {
		handler = inst->evt_handler[DWMMC_EVT_CMD_DONE];
		if (handler != NULL) {
			handler(inst->evt_args[DWMMC_EVT_CMD_DONE]);
		}
	}

	/* CMD11: the volt_switch event (HTO bit 10) means the response is in */
	if ((status & DWMMC_INTMSK_HTO) && (inst->in_volt_switch)) {
		handler = inst->evt_handler[DWMMC_EVT_CMD_DONE];
		if (handler != NULL) {
			handler(inst->evt_args[DWMMC_EVT_CMD_DONE]);
		}
	}

	if (status & DWMMC_INTMSK_DTO) {
		handler = inst->evt_handler[inst->cur_is_write
						    ? DWMMC_EVT_DATA_WRITE_DONE
						    : DWMMC_EVT_DATA_READ_DONE];
		if (handler != NULL) {
			handler(inst->evt_args[inst->cur_is_write
						       ? DWMMC_EVT_DATA_WRITE_DONE
						       : DWMMC_EVT_DATA_READ_DONE]);
		}
		data_event = true;
	}

	if (status & (DWMMC_INTMSK_RTO | DWMMC_INTMSK_RE)) {
		handler = inst->evt_handler[DWMMC_EVT_CMD_ERROR];
		if (handler != NULL) {
			handler(inst->evt_args[DWMMC_EVT_CMD_ERROR]);
		}
		error_event = true;
	}

	if (status & (DWMMC_DATA_ERR_FLAGS | DWMMC_DATA_TOUT_FLAGS)) {
		DWMMC_ERROR("data error status: 0x%x", status);
		handler = inst->evt_handler[DWMMC_EVT_DATA_ERROR];
		if (handler != NULL) {
			handler(inst->evt_args[DWMMC_EVT_DATA_ERROR]);
		}
		error_event = true;
	}

	if (dma_status & DWMMC_IDMAC_INT_RI) {
		if (!data_event) {
			handler = inst->evt_handler[DWMMC_EVT_DATA_READ_DONE];
			if (handler != NULL) {
				handler(inst->evt_args[DWMMC_EVT_DATA_READ_DONE]);
			}
			data_event = true;
		}
	}

	if (dma_status & DWMMC_IDMAC_INT_TI) {
		if (!data_event) {
			handler = inst->evt_handler[DWMMC_EVT_DATA_WRITE_DONE];
			if (handler != NULL) {
				handler(inst->evt_args[DWMMC_EVT_DATA_WRITE_DONE]);
			}
			data_event = true;
		}
	}

	if (dma_status & (DWMMC_IDMAC_INT_FBE | DWMMC_IDMAC_INT_DU |
			  DWMMC_IDMAC_INT_CES | DWMMC_IDMAC_INT_AI)) {
		DWMMC_ERROR("dma error status: 0x%x", dma_status);
		if (!error_event) {
			handler = inst->evt_handler[DWMMC_EVT_DATA_ERROR];
			if (handler != NULL) {
				handler(inst->evt_args[DWMMC_EVT_DATA_ERROR]);
			}
		}
	}

	if (status & DWMMC_INTMSK_SDIO_INT) {
		/* SDIO card interrupt (DAT1): self-mask first so a level-held
		 * DAT1 cannot storm the GIC while the consumer works; the
		 * consumer re-arms via dwc_mmc_set_interrupt_mask once it
		 * has consumed the chip-side source. The upcall must only
		 * wake a worker - ISR context, the bus is untouchable. */
		dwc_mmc_set_interrupt_mask(inst, DWMMC_INTMSK_SDIO_INT, false);
		handler = inst->evt_handler[DWMMC_EVT_SDIO_INT];
		if (handler != NULL) {
			handler(inst->evt_args[DWMMC_EVT_SDIO_INT]);
		}
	}

	/* clear interrupt status (write-one-clear) */
	DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET, status);
	DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET, dma_status);

	/* after a data event stop the DMA (re-armed by the next transfer) */
	if (data_event) {
		uint32_t ctrl = DWMMC_READ_REG(base_addr, DWMMC_CTRL_OFFSET);

		ctrl &= ~(uint32_t)DWMMC_CTRL_DMA_ENABLE;
		DWMMC_WRITE_REG(base_addr, DWMMC_CTRL_OFFSET, ctrl);
	}
}

bool dwc_mmc_data_busy(struct dwc_mmc *inst)
{
	/* STATUS bit9 = data_busy (inverted DAT0): low level = busy */
	return (0U != (DWMMC_READ_REG(inst->config.base_addr, DWMMC_STATUS_OFFSET) &
		       DWMMC_STATUS_DATA_BUSY));
}

bool dwc_mmc_card_present(struct dwc_mmc *inst)
{
	return dwc_mmc_card_exists(inst->config.base_addr);
}
