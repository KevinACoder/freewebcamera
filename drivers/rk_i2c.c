/*
 * @file   rk_i2c.c
 * @brief  RK3568 I2C host driver (Rockchip I2C v5 block) behind the standard
 *         CMSIS-Driver ARM_DRIVER_I2C interface. Polling only; the interrupt
 *         line is left unclaimed.
 *
 * PROVENANCE AND PORTING POLICY (decision D20, same pattern as dwc_mshc.c).
 * The controller is Rockchip's own I2C v5, not Synopsys DesignWare (decision
 * D29 corrects the older design note that pointed at a DesignWare driver).
 * The register sequences below - PMUGRF iomux, PMUCRU/CRU clock-divide, gate
 * and soft-reset bring-up, the START/STOP handshake with its IEN/IPD polling,
 * the 32-byte FIFO packing (4 bytes per word, first byte in bits[7:0]), and
 * the TRX-mode random read via MRXADDR/MRXRADDR with the bit24 valid flag -
 * are the author's own RK3568 port, debugged on this board in the standalone
 * line (2026-08-11: RX8025 seconds ticking, INA3221 config/mfr/die and the
 * 3.3/5/12 V rails read correctly) and carried over line-for-line. Vocabulary
 * changed only:
 *
 *   - register primitives are this project's regs.h;
 *   - fsleep_* become counter-timer busy waits (kernel-free for the K4 build);
 *   - the SDK's (chip, reg, len) call shape is re-framed as the standard
 *     CMSIS-Driver master API, with two semantic notes below.
 *
 * CMSIS mapping (the two semantic notes):
 *
 *   1. MasterTransmit(addr, data, num, xfer_pending=true) with num == 1 does
 *      NOT touch the bus: it buffers the payload, and the following
 *      MasterReceive executes the proven ONE-SHOT combined random read
 *      (START, addr+W, reg, RESTART, addr+R, data, STOP in a single TRX op).
 *      Rationale: every proven controller sequence on this block begins from
 *      the disabled engine; issuing START from the enabled-but-idle state
 *      after a pending transmit was tried first and hangs (no STARTIPD, the
 *      2026-09-17 first board round). The buffered form is bit-identical to
 *      the standalone-line MasterRead. num > 1 with xfer_pending is refused
 *      (UNSUPPORTED) rather than silently degraded.
 *
 *   2. num == 0 on MasterTransmit is a probe: address byte only, NAK becomes
 *      ARM_DRIVER_ERROR_SPECIFIC (this is how `i2c scan` works). A
 *      MasterReceive with NO buffered pointer byte runs the same TRX op with
 *      MRXRADDR invalid (current-address read); this variant is inferred from
 *      the valid-bit design, not independently proven on the board.
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "regs.h"

#include "Driver_I2C.h"
#include "rk_i2c.h"
#include "rk_i2c_regs.h"

#define RK_I2C_DRV_VERSION	ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0)

/* --- instance state --------------------------------------------------------- */

struct rk_i2c_plat {
	const char *name;
	uintptr_t base;
	uint32_t scl_hz;			/* default bus speed */
	void (*platform_init)(void);
};

struct rk_i2c {
	const struct rk_i2c_plat *plat;
	uint32_t tuning;	/* CON[15:8] setup bits from the speed choice */
	uint32_t scl_hz;	/* current bus speed */
	bool initialized;
	bool powered;
	bool busy;
	bool pending;		/* a xfer_pending transmit owns the bus */
	uint8_t pending_byte;	/* its single payload byte, for MRXRADDR */
	uint32_t xfer_count;	/* bytes moved by the last op */
};

static void rk_i2c_udelay(uint32_t us)
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
static void rk_i2c_clrset(uintptr_t reg, uint32_t mask, uint32_t val)
{
	reg_wr32(reg, ((mask & 0xFFFFU) << 16) | (val & mask));
}

/* --- platform bring-up (line-for-line from the standalone-line port) -------- */

static void rk_i2c_platform_init_0(void)
{
	uintptr_t pmugrf = RK_I2C_PMUGRF_BASE;
	uintptr_t pmucru = RK_I2C0_PMUCRU_BASE;

	/* GPIO0_B1/B2 -> fn1 */
	rk_i2c_clrset(pmugrf + RK_I2C_PMUGRF_GPIO0B_IOMUX_L,
		      RK_I2C0_IOMUX_L_MASK, RK_I2C0_IOMUX_L_VAL);
	/* clk_i2c0 = ppll 200M / (div+1), div=1 -> 100 MHz */
	rk_i2c_clrset(pmucru + RK_I2C0_CLKSEL_CON3,
		      RK_I2C0_CLKSEL_DIV_MASK, RK_I2C0_CLKSEL_DIV_100M);
	/* open pclk/clk gates (gate bit: 1=off, write 0=on) */
	rk_i2c_clrset(pmucru + RK_I2C0_CLKGATE_CON1, RK_I2C0_GATE_MASK, 0U);
	/* soft reset: assert, 10 us, release */
	rk_i2c_clrset(pmucru + RK_I2C0_SOFTRST_CON0,
		      RK_I2C0_RST_MASK, RK_I2C0_RST_MASK);
	rk_i2c_udelay(10);
	rk_i2c_clrset(pmucru + RK_I2C0_SOFTRST_CON0, RK_I2C0_RST_MASK, 0U);
}

static void rk_i2c_platform_init_1(void)
{
	uintptr_t pmugrf = RK_I2C_PMUGRF_BASE;
	uintptr_t cru = RK_I2C1_CRU_BASE;

	/* GPIO0_B3/B4 -> fn1 (B3 in the low word, B4 crosses into the high) */
	rk_i2c_clrset(pmugrf + RK_I2C_PMUGRF_GPIO0B_IOMUX_L,
		      RK_I2C1_IOMUX_L_MASK, RK_I2C1_IOMUX_L_VAL);
	rk_i2c_clrset(pmugrf + RK_I2C_PMUGRF_GPIO0B_IOMUX_H,
		      RK_I2C1_IOMUX_H_MASK, RK_I2C1_IOMUX_H_VAL);
	/* clk_i2c mux -> gpll_100m (nominal 100 MHz) */
	rk_i2c_clrset(cru + RK_I2C1_CLKSEL_CON71,
		      RK_I2C1_MUX_MASK, RK_I2C1_MUX_SEL_100M << RK_I2C1_MUX_SHIFT);
	/* open pclk_i2c1/clk_i2c1, then the shared clk_i2c gate */
	rk_i2c_clrset(cru + RK_I2C1_CLKGATE_CON30, RK_I2C1_GATE_MASK, 0U);
	rk_i2c_clrset(cru + RK_I2C1_CLKGATE_CON32,
		      (1UL << RK_I2C1_SHARED_GATE_BIT), 0U);
	/* soft reset: assert, 10 us, release */
	rk_i2c_clrset(cru + RK_I2C1_SOFTRST_CON22,
		      RK_I2C1_RST_MASK, RK_I2C1_RST_MASK);
	rk_i2c_udelay(10);
	rk_i2c_clrset(cru + RK_I2C1_SOFTRST_CON22, RK_I2C1_RST_MASK, 0U);
}

/* --- SCL timing (the u-boot rk_i2c_adapter_clk algorithm) ------------------- */

struct rk_i2c_spec {
	uint32_t min_low_ns;
	uint32_t min_high_ns;
	uint32_t max_rise_ns;
	uint32_t max_fall_ns;
};

static const struct rk_i2c_spec spec_standard = { 4700U, 4000U, 1000U, 300U };
static const struct rk_i2c_spec spec_fast = { 1300U, 600U, 300U, 300U };
static const struct rk_i2c_spec spec_fast_plus = { 500U, 260U, 120U, 120U };

static uint32_t div_round_up(uint32_t a, uint32_t b)
{
	return (a + b - 1U) / b;
}

static int32_t rk_i2c_calc_timing(struct rk_i2c *inst, uint32_t scl_hz)
{
	const struct rk_i2c_spec *spec;
	uint32_t i2c_rate;
	uint32_t speed;
	uint32_t start_setup = 0U;
	uint32_t min_total_div, min_low_div, min_high_div, min_hold_div;
	uint32_t low_div, high_div, extra_div, extra_low_div;
	uint32_t min_low_ns, min_high_ns;

	if (scl_hz >= 1000U && scl_hz <= 100000U) {
		start_setup = 1U;
		speed = 100U;
		spec = &spec_standard;
	} else if (scl_hz <= 400000U) {
		speed = 400U;
		spec = &spec_fast;
	} else if (scl_hz <= 1000000U) {
		speed = 1000U;
		spec = &spec_fast_plus;
	} else {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	i2c_rate = div_round_up(RK_I2C_REF_CLK_HZ, 1000U);
	speed = div_round_up(scl_hz, 1000U);

	min_total_div = div_round_up(i2c_rate, speed * 8U);

	min_high_ns = spec->max_rise_ns + spec->min_high_ns;
	min_high_div = div_round_up(i2c_rate * min_high_ns, 8U * 1000000U);

	min_low_ns = spec->max_fall_ns + spec->min_low_ns;
	min_low_div = div_round_up(i2c_rate * min_low_ns, 8U * 1000000U);

	min_high_div = (min_high_div < 1U) ? 2U : min_high_div;
	min_low_div = (min_low_div < 1U) ? 2U : min_low_div;

	min_hold_div = min_high_div + min_low_div;
	if (min_hold_div >= min_total_div) {
		high_div = min_high_div;
		low_div = min_low_div;
	} else {
		extra_div = min_total_div - min_hold_div;
		extra_low_div = div_round_up(min_low_div * extra_div, min_hold_div);
		low_div = min_low_div + extra_low_div;
		high_div = min_high_div + (extra_div - extra_low_div);
	}

	high_div--;
	low_div--;
	if (high_div > 0xFFFFU || low_div > 0xFFFFU) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	/* 1 is enough for the data hold/setup tuning (standard mode wants a
	 * start-setup cycle as well). */
	inst->tuning = RK_I2C_CON_SDA_CFG(1) | RK_I2C_CON_STA_CFG(start_setup);
	reg_wr32(inst->plat->base + RK_I2C_CLKDIV,
		 (high_div << RK_I2C_CLKDIV_HIGH_SHIFT) | low_div);

	return ARM_DRIVER_OK;
}

/* --- engine primitives ------------------------------------------------------- */

static int32_t rk_i2c_wait_bit(uintptr_t base, uint32_t reg, uint32_t mask,
			       bool set, uint32_t timeout_us)
{
	uint32_t loop = timeout_us / 10U;

	while (loop-- > 0U) {
		uint32_t val = reg_rd32(base + reg);

		if (set ? ((val & mask) != 0U) : ((val & mask) == 0U)) {
			return ARM_DRIVER_OK;
		}
		rk_i2c_udelay(10);
	}
	return ARM_DRIVER_ERROR_TIMEOUT;
}

/* START handshake (u-boot rk_i2c_send_start_bit shape). con carries the mode
 * bits; EN/START/tuning are added here. */
static int32_t rk_i2c_send_start(struct rk_i2c *inst, uint32_t con)
{
	uintptr_t base = inst->plat->base;
	uint32_t cfg = inst->tuning;
	int32_t ret;

	reg_wr32(base + RK_I2C_IPD, RK_I2C_IPD_ALL_CLEAN);
	reg_wr32(base + RK_I2C_IEN, RK_I2C_INT_START);
	reg_wr32(base + RK_I2C_CON, RK_I2C_CON_EN | RK_I2C_CON_START | cfg | con);

	ret = rk_i2c_wait_bit(base, RK_I2C_IPD, RK_I2C_INT_START, true,
			      RK_I2C_TIMEOUT_US);
	if (ret != ARM_DRIVER_OK) {
		board_log("i2c: %s start timeout (con=0x%x ipd=0x%x)",
			  inst->plat->name,
			  reg_rd32(base + RK_I2C_CON),
			  reg_rd32(base + RK_I2C_IPD));
		return ret;
	}

	/* clear START, keep the mode */
	reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_START);
	reg_wr32(base + RK_I2C_CON, RK_I2C_CON_EN | cfg | con);

	return ARM_DRIVER_OK;
}

static int32_t rk_i2c_send_stop(struct rk_i2c *inst)
{
	uintptr_t base = inst->plat->base;
	uint32_t cfg = inst->tuning;
	int32_t ret;

	reg_wr32(base + RK_I2C_IPD, RK_I2C_IPD_ALL_CLEAN);
	reg_wr32(base + RK_I2C_IEN, RK_I2C_INT_STOP);
	reg_wr32(base + RK_I2C_CON, RK_I2C_CON_EN | cfg | RK_I2C_CON_STOP);

	ret = rk_i2c_wait_bit(base, RK_I2C_IPD, RK_I2C_INT_STOP, true,
			      RK_I2C_TIMEOUT_US);
	if (ret == ARM_DRIVER_OK) {
		reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_STOP);
	}

	return ret;
}

static void rk_i2c_disable(struct rk_i2c *inst)
{
	uintptr_t base = inst->plat->base;

	reg_wr32(base + RK_I2C_IEN, 0U);
	reg_wr32(base + RK_I2C_IPD, RK_I2C_IPD_ALL_CLEAN);
	reg_wr32(base + RK_I2C_CON, 0U);
}

/* --- master transmit ---------------------------------------------------------- */

static int32_t rk_i2c_master_tx(struct rk_i2c *inst, uint32_t addr,
				const uint8_t *data, uint32_t num)
{
	uintptr_t base = inst->plat->base;
	uint32_t cfg = inst->tuning;
	uint32_t remain;
	uint32_t total;
	bool first = true;
	int32_t ret = ARM_DRIVER_OK;

	/* total = address byte + payload, matching u-boot rk_i2c_write */
	total = num + 1U;
	remain = total;

	while (remain != 0U) {
		uint32_t chunk = (remain > RK_I2C_FIFO_SIZE) ? RK_I2C_FIFO_SIZE : remain;
		uint32_t i, j;

		/* load the TX FIFO: byte0 = slave address, then payload */
		for (i = 0U; i < div_round_up(chunk, 4U); i++) {
			uint32_t txdata = 0U;

			for (j = 0U; j < 4U; j++) {
				uint32_t idx = i * 4U + j;

				if (idx == chunk) {
					break;
				}
				if (idx == 0U) {
					txdata |= (addr << 1) & 0xFFU;
				} else {
					txdata |= (uint32_t)data[idx - 1U] << (j * 8U);
				}
			}
			reg_wr32(base + RK_I2C_TXDATA + (i * 4U), txdata);
		}

		if (first) {
			ret = rk_i2c_send_start(inst, RK_I2C_CON_MOD_TX);
			first = false;
		} else {
			reg_wr32(base + RK_I2C_CON,
				 RK_I2C_CON_EN | cfg | RK_I2C_CON_MOD_TX);
		}
		if (ret != ARM_DRIVER_OK) {
			break;
		}

		reg_wr32(base + RK_I2C_IEN, RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV);
		reg_wr32(base + RK_I2C_MTXCNT, chunk);

		ret = rk_i2c_wait_bit(base, RK_I2C_IPD,
				      RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV, true,
				      RK_I2C_TIMEOUT_US);
		if (ret != ARM_DRIVER_OK) {
			board_log("i2c: %s tx timeout (ipd=0x%x)",
				  inst->plat->name, reg_rd32(base + RK_I2C_IPD));
			break;
		}
		if ((reg_rd32(base + RK_I2C_IPD) & RK_I2C_INT_NAKRCV) != 0U) {
			reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_NAKRCV);
			ret = ARM_DRIVER_ERROR_SPECIFIC;
			break;
		}
		reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_MBTF);

		remain -= chunk;
	}

	(void)rk_i2c_send_stop(inst);
	rk_i2c_disable(inst);
	inst->pending = false;
	inst->xfer_count = (int32_t)num;
	return ret;
}

/* --- master receive ------------------------------------------------------------ */

static int32_t rk_i2c_master_rx(struct rk_i2c *inst, uint32_t addr,
				uint8_t *data, uint32_t num, bool xfer_pending)
{
	uintptr_t base = inst->plat->base;
	uint32_t cfg = inst->tuning;
	uint32_t remain = num;
	uint32_t offset = 0U;
	bool snd_chunk = false;
	int32_t ret = ARM_DRIVER_OK;

	/* slave address for the read phase (R/W bit = 1) */
	reg_wr32(base + RK_I2C_MRXADDR, RK_I2C_MRX_VALID | ((addr << 1) | 1U));
	if (inst->pending) {
		/* replay the buffered pointer byte as the register byte: the
		 * proven one-shot random read (START, addr+W, reg, RESTART,
		 * addr+R, data, STOP in one controller op) */
		reg_wr32(base + RK_I2C_MRXRADDR,
			 RK_I2C_MRX_VALID | inst->pending_byte);
	} else {
		/* no register bytes: bare restart/current-address read */
		reg_wr32(base + RK_I2C_MRXRADDR, 0U);
	}
	inst->pending = false;

	while (remain != 0U) {
		uint32_t chunk = (remain > RK_I2C_FIFO_SIZE) ? RK_I2C_FIFO_SIZE : remain;
		uint32_t i, j;

		if (!snd_chunk) {
			uint32_t con = RK_I2C_CON_EN | RK_I2C_CON_MOD_TRX;

			if (remain <= RK_I2C_FIFO_SIZE) {
				con |= RK_I2C_CON_LASTACK;
			}
			ret = rk_i2c_send_start(inst, con);
		} else {
			uint32_t con = RK_I2C_CON_EN | RK_I2C_CON_MOD_RX;

			if (remain <= RK_I2C_FIFO_SIZE) {
				con |= RK_I2C_CON_LASTACK;
			}
			reg_wr32(base + RK_I2C_CON, con | cfg);
			ret = ARM_DRIVER_OK;
		}
		if (ret != ARM_DRIVER_OK) {
			break;
		}

		reg_wr32(base + RK_I2C_IEN, RK_I2C_INT_MBRF | RK_I2C_INT_NAKRCV);
		reg_wr32(base + RK_I2C_MRXCNT, chunk);

		ret = rk_i2c_wait_bit(base, RK_I2C_IPD,
				      RK_I2C_INT_MBRF | RK_I2C_INT_NAKRCV, true,
				      RK_I2C_TIMEOUT_US);
		if (ret != ARM_DRIVER_OK) {
			board_log("i2c: %s rx timeout (ipd=0x%x)",
				  inst->plat->name, reg_rd32(base + RK_I2C_IPD));
			break;
		}
		if ((reg_rd32(base + RK_I2C_IPD) & RK_I2C_INT_NAKRCV) != 0U) {
			reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_NAKRCV);
			ret = ARM_DRIVER_ERROR_SPECIFIC;
			break;
		}
		reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_MBRF);

		/* unpack the FIFO (first byte in RXDATA0 bits[7:0]) */
		for (i = 0U; i < div_round_up(chunk, 4U); i++) {
			uint32_t rxdata = reg_rd32(base + RK_I2C_RXDATA + (i * 4U));

			for (j = 0U; j < 4U; j++) {
				if (i * 4U + j == chunk) {
					break;
				}
				data[offset + i * 4U + j] =
					(uint8_t)((rxdata >> (j * 8U)) & 0xFFU);
			}
		}

		remain -= chunk;
		offset += chunk;
		snd_chunk = true;
	}

	if (!xfer_pending || ret != ARM_DRIVER_OK) {
		(void)rk_i2c_send_stop(inst);
		rk_i2c_disable(inst);
	}

	if (ret == ARM_DRIVER_OK) {
		inst->xfer_count = (int32_t)num;
	}
	return ret;
}

/* --- CMSIS-Driver core ---------------------------------------------------------- */

static int32_t rk_i2c_initialize(struct rk_i2c *inst, ARM_I2C_SignalEvent_t cb)
{
	(void)cb;	/* polling driver: no events to signal */

	if (inst->initialized) {
		return ARM_DRIVER_OK;
	}
	inst->initialized = true;
	inst->powered = false;
	inst->busy = false;
	inst->pending = false;
	return ARM_DRIVER_OK;
}

static int32_t rk_i2c_uninitialize(struct rk_i2c *inst)
{
	if (inst->powered) {
		rk_i2c_disable(inst);
		inst->powered = false;
	}
	inst->initialized = false;
	return ARM_DRIVER_OK;
}

static int32_t rk_i2c_power_control(struct rk_i2c *inst, ARM_POWER_STATE state)
{
	int32_t ret;

	switch (state) {
	case ARM_POWER_OFF:
		if (inst->powered) {
			rk_i2c_disable(inst);
			inst->powered = false;
		}
		return ARM_DRIVER_OK;
	case ARM_POWER_LOW:
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	case ARM_POWER_FULL:
		if (inst->powered) {
			return ARM_DRIVER_OK;
		}
		if (!inst->initialized) {
			return ARM_DRIVER_ERROR;
		}
		inst->plat->platform_init();
		ret = rk_i2c_calc_timing(inst, inst->plat->scl_hz);
		if (ret != ARM_DRIVER_OK) {
			return ret;
		}
		rk_i2c_udelay(1000);	/* clocks/resets settle */
		reg_wr32(inst->plat->base + RK_I2C_IPD, RK_I2C_IPD_ALL_CLEAN);
		reg_wr32(inst->plat->base + RK_I2C_IEN, 0U);
		reg_wr32(inst->plat->base + RK_I2C_CON,
			 RK_I2C_CON_EN | inst->tuning);
		inst->powered = true;
		board_log("i2c: %s ready (scl %u Hz)", inst->plat->name,
			  inst->plat->scl_hz);
		return ARM_DRIVER_OK;
	default:
		return ARM_DRIVER_ERROR_PARAMETER;
	}
}

static int32_t rk_i2c_check_args(struct rk_i2c *inst, uint32_t addr)
{
	if (!inst->powered) {
		return ARM_DRIVER_ERROR;
	}
	if (inst->busy) {
		return ARM_DRIVER_ERROR_BUSY;
	}
	if ((addr & ARM_I2C_ADDRESS_10BIT) != 0U || (addr & ~0x7FFU) != 0U) {
		return ARM_DRIVER_ERROR_PARAMETER;	/* 7-bit only */
	}
	return ARM_DRIVER_OK;
}

static int32_t rk_i2c_do_transmit(struct rk_i2c *inst, uint32_t addr,
				  const uint8_t *data, uint32_t num,
				  bool xfer_pending)
{
	int32_t ret;

	ret = rk_i2c_check_args(inst, addr);
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}
	inst->busy = true;

	if (num == 0U) {
		/* probe: address byte only, NAK signals "no device here" */
		uintptr_t base = inst->plat->base;

		reg_wr32(base + RK_I2C_TXDATA, (addr << 1) & 0xFFU);
		ret = rk_i2c_send_start(inst, RK_I2C_CON_MOD_TX);
		if (ret == ARM_DRIVER_OK) {
			reg_wr32(base + RK_I2C_IEN,
				 RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV);
			reg_wr32(base + RK_I2C_MTXCNT, 1U);
			ret = rk_i2c_wait_bit(base, RK_I2C_IPD,
					      RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV,
					      true, RK_I2C_TIMEOUT_US);
			if (ret == ARM_DRIVER_OK) {
				if ((reg_rd32(base + RK_I2C_IPD) &
				     RK_I2C_INT_NAKRCV) != 0U) {
					reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_NAKRCV);
					ret = ARM_DRIVER_ERROR_SPECIFIC;
				} else {
					reg_wr32(base + RK_I2C_IPD, RK_I2C_INT_MBTF);
					inst->xfer_count = 0U;
				}
			}
		}
		(void)rk_i2c_send_stop(inst);
		rk_i2c_disable(inst);
		inst->pending = false;
	} else if (xfer_pending) {
		/* Buffer only, no bus activity: the following MasterReceive
		 * executes the proven one-shot combined random read. Starting
		 * a new START from the enabled-and-idle state a completed
		 * transmit leaves behind was tried on the board (2026-09-17)
		 * and hangs - no STARTIPD ever sets. One payload byte is the
		 * shape every device here uses (register/command pointer). */
		if (num != 1U) {
			inst->busy = false;
			return ARM_DRIVER_ERROR_UNSUPPORTED;
		}
		inst->pending = true;
		inst->pending_byte = data[0];
		inst->xfer_count = 1U;
		inst->busy = false;
		return ARM_DRIVER_OK;
	} else {
		ret = rk_i2c_master_tx(inst, addr, data, num);
	}

	inst->busy = false;
	return ret;
}

static int32_t rk_i2c_do_receive(struct rk_i2c *inst, uint32_t addr,
				 uint8_t *data, uint32_t num, bool xfer_pending)
{
	int32_t ret;

	ret = rk_i2c_check_args(inst, addr);
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}
	if (num == 0U) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	inst->busy = true;
	ret = rk_i2c_master_rx(inst, addr, data, num, xfer_pending);
	inst->busy = false;
	return ret;
}

static int32_t rk_i2c_do_control(struct rk_i2c *inst, uint32_t control,
				 uint32_t arg)
{
	switch (control) {
	case ARM_I2C_BUS_SPEED: {
		uint32_t scl;

		if (arg == ARM_I2C_BUS_SPEED_STANDARD) {
			scl = 100000U;
		} else if (arg == ARM_I2C_BUS_SPEED_FAST) {
			scl = 400000U;
		} else if (arg == ARM_I2C_BUS_SPEED_FAST_PLUS) {
			scl = 1000000U;
		} else {
			return ARM_DRIVER_ERROR_UNSUPPORTED;
		}
		if (rk_i2c_calc_timing(inst, scl) != ARM_DRIVER_OK) {
			return ARM_DRIVER_ERROR_PARAMETER;
		}
		inst->scl_hz = scl;
		return ARM_DRIVER_OK;
	}
	case ARM_I2C_ABORT_TRANSFER:
		if (inst->powered) {
			(void)rk_i2c_send_stop(inst);
			rk_i2c_disable(inst);
		}
		inst->pending = false;
		inst->busy = false;
		return ARM_DRIVER_OK;
	case ARM_I2C_OWN_ADDRESS:
	case ARM_I2C_BUS_CLEAR:
	default:
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}
}

static ARM_I2C_STATUS rk_i2c_do_get_status(const struct rk_i2c *inst)
{
	ARM_I2C_STATUS st = { 0 };

	st.busy = inst->busy;
	st.mode = 1U;	/* master */
	return st;
}

/* --- instances ----------------------------------------------------------------- */

static const struct rk_i2c_plat rk_i2c_plat0 = {
	.name = "i2c0",
	.base = 0xFDD40000UL,
	.scl_hz = 100000U,
	.platform_init = rk_i2c_platform_init_0,
};

static const struct rk_i2c_plat rk_i2c_plat1 = {
	.name = "i2c1",
	.base = 0xFE5A0000UL,
	.scl_hz = 100000U,
	.platform_init = rk_i2c_platform_init_1,
};

static struct rk_i2c g_rk_i2c0 = { .plat = &rk_i2c_plat0 };
static struct rk_i2c g_rk_i2c1 = { .plat = &rk_i2c_plat1 };

/*
 * The CMSIS ops struct carries no "this" pointer, so each instance gets its
 * own trampolines; the macro generates them (same pattern as dwc_eqos.h).
 */
#define RK_I2C_DECLARE_INSTANCE(n)                                             \
static ARM_DRIVER_VERSION rk_i2c##n##_get_version(void)                        \
{                                                                              \
	return (ARM_DRIVER_VERSION){ ARM_I2C_API_VERSION, RK_I2C_DRV_VERSION };\
}                                                                              \
static ARM_I2C_CAPABILITIES rk_i2c##n##_get_capabilities(void)                 \
{                                                                              \
	/* 7-bit addressing only */                                            \
	return (ARM_I2C_CAPABILITIES){ 0 };                                    \
}                                                                              \
static int32_t rk_i2c##n##_initialize(ARM_I2C_SignalEvent_t cb)                \
{                                                                              \
	return rk_i2c_initialize(&g_rk_i2c##n, cb);                            \
}                                                                              \
static int32_t rk_i2c##n##_uninitialize(void)                                  \
{                                                                              \
	return rk_i2c_uninitialize(&g_rk_i2c##n);                              \
}                                                                              \
static int32_t rk_i2c##n##_power_control(ARM_POWER_STATE state)                \
{                                                                              \
	return rk_i2c_power_control(&g_rk_i2c##n, state);                      \
}                                                                              \
static int32_t rk_i2c##n##_master_transmit(uint32_t addr, const uint8_t *data, \
					   uint32_t num, bool xfer_pending)    \
{                                                                              \
	return rk_i2c_do_transmit(&g_rk_i2c##n, addr, data, num,               \
				  xfer_pending);                               \
}                                                                              \
static int32_t rk_i2c##n##_master_receive(uint32_t addr, uint8_t *data,        \
					  uint32_t num, bool xfer_pending)     \
{                                                                              \
	return rk_i2c_do_receive(&g_rk_i2c##n, addr, data, num,                \
				 xfer_pending);                                \
}                                                                              \
static int32_t rk_i2c##n##_slave_transmit(const uint8_t *data, uint32_t num)   \
{                                                                              \
	(void)data; (void)num;                                                 \
	return ARM_DRIVER_ERROR_UNSUPPORTED;                                   \
}                                                                              \
static int32_t rk_i2c##n##_slave_receive(uint8_t *data, uint32_t num)          \
{                                                                              \
	(void)data; (void)num;                                                 \
	return ARM_DRIVER_ERROR_UNSUPPORTED;                                   \
}                                                                              \
static int32_t rk_i2c##n##_get_data_count(void)                                \
{                                                                              \
	return (int32_t)g_rk_i2c##n.xfer_count;                                \
}                                                                              \
static int32_t rk_i2c##n##_control(uint32_t control, uint32_t arg)             \
{                                                                              \
	return rk_i2c_do_control(&g_rk_i2c##n, control, arg);                  \
}                                                                              \
static ARM_I2C_STATUS rk_i2c##n##_get_status(void)                             \
{                                                                              \
	return rk_i2c_do_get_status(&g_rk_i2c##n);                             \
}                                                                              \
ARM_DRIVER_I2C Driver_I2C##n = {                                               \
	rk_i2c##n##_get_version,                                               \
	rk_i2c##n##_get_capabilities,                                          \
	rk_i2c##n##_initialize,                                                \
	rk_i2c##n##_uninitialize,                                              \
	rk_i2c##n##_power_control,                                             \
	rk_i2c##n##_master_transmit,                                           \
	rk_i2c##n##_master_receive,                                            \
	rk_i2c##n##_slave_transmit,                                            \
	rk_i2c##n##_slave_receive,                                             \
	rk_i2c##n##_get_data_count,                                            \
	rk_i2c##n##_control,                                                   \
	rk_i2c##n##_get_status,                                                \
}

RK_I2C_DECLARE_INSTANCE(0);
RK_I2C_DECLARE_INSTANCE(1);
