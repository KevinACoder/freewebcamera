/*
 * @file   rk_tsadc.c
 * @brief  RK3568 TSADC driver (polling, auto-conversion mode).
 *
 * PROVENANCE. The init sequence is rk_tsadcv7_initialize as run on this very
 * board by two operating systems: the mainline Linux rockchip_thermal driver
 * and the BSD-licensed NetBSD rk_tsadc.c (v1.16, Matthew R. Green - carried
 * here under its BSD terms with attribution; NetBSD measured CPU 33.75 C /
 * GPU 32.5 C on this board on 2026-09-04, Linux 38.3 / 36.7 C on 2026-09-14).
 * The RK3568 code table is the Linux driver's rk3568_code_table, as tabulated
 * in the NetBSD file. Sequence fidelity over style, per the usual policy:
 *
 *   USER_CON = 0xfc0 (97 us inter-convert latency, >= the 90 us TRM floor)
 *   GRF 0x600 hiword writes: TSEN on, 15 us, then ANA_REG0/1/2, 100 us
 *   auto period 1622 (2.5 ms) for both windows, debounce 4
 *   warn 75 C / tshut 95 C comparators armed, TSHUT routed to the GPIO tap
 *     (polarity low-active) - the exact configuration Linux and NetBSD ran
 *   AUTO_CON: auto status + per-source low-trigger enables, then AUTO_EN |
 *     Q_SEL (Q_SEL is the version-3-and-later quick-conversion bit)
 *
 * Clocks (coordinates from the clk-rk3568 tree):
 *   CLKSEL_CON51 @0x2CC - TSEN mux[5:4]=1 (gpll_100m), div[2:0]=5 (/6 =
 *   16.67 MHz, the closest achievable to the 17 MHz assigned rate);
 *   TSADC div[14:8]=23 (16.67 M / 24 = 694 kHz ~ the 700 kHz assigned rate)
 *   CLKGATE_CON26 @0x368 - bit4 pclk_tsadc, bit5 clk_tsadc_tsen, bit6
 *   clk_tsadc: write 0 to open
 * No soft reset is issued: mainline does not touch a tsadc reset either, and
 * the sensor comes up through the GRF taps alone.
 *
 * Polling only; the interrupt line is left unclaimed.
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "board.h"
#include "regs.h"

#include "rk_tsadc.h"

/* --- register block -------------------------------------------------------- */

#define TSADC_USER_CON		0x00U
#define TSADC_AUTO_CON		0x04U
#define TSADC_INT_EN		0x08U
#define TSADC_INT_PD		0x0CU
#define TSADC_DATA0		0x20U
#define TSADC_DATA1		0x24U
#define TSADC_COMP0_INT		0x30U
#define TSADC_COMP1_INT		0x34U
#define TSADC_COMP0_SHUT	0x40U
#define TSADC_COMP1_SHUT	0x44U
#define TSADC_HIGH_INT_DEBOUNCE	0x60U
#define TSADC_HIGH_TSHUT_DEBOUNCE 0x64U
#define TSADC_AUTO_PERIOD	0x68U
#define TSADC_AUTO_PERIOD_HT	0x6CU

/* AUTO_CON bits */
#define TSADC_AUTO_CON_AUTO_STATUS	(1UL << 16)
#define TSADC_AUTO_CON_SRC1_LT_EN	(1UL << 13)
#define TSADC_AUTO_CON_SRC0_LT_EN	(1UL << 12)
#define TSADC_AUTO_CON_TSHUT_POLARITY	(1UL << 8)
#define TSADC_AUTO_CON_Q_SEL		(1UL << 1)
#define TSADC_AUTO_CON_AUTO_EN		(1UL << 0)

/* INT_EN bits (tshut routing) */
#define TSADC_INT_EN_TSHUT_2CRU_SRC1	(1UL << 9)
#define TSADC_INT_EN_TSHUT_2CRU_SRC0	(1UL << 8)
#define TSADC_INT_EN_TSHUT_2GPIO_SRC1	(1UL << 5)
#define TSADC_INT_EN_TSHUT_2GPIO_SRC0	(1UL << 4)

#define TSADC_DATA_MASK		0xFFFUL

/* rk_tsadcv7 constants (Linux/NetBSD, run on this board) */
#define RK3568_USER_INTER_PD_SOC	0xFC0U	/* 97 us, >= 90 us TRM floor */
#define RK3568_AUTO_PERIOD_TIME		48750U	/* 2.03 ms at the actual
						 * 24 MHz clk_tsadc: must
						 * exceed the 97 us latency
						 * (see the period comment in
						 * rk_tsadc_init) */
#define TSADC_HT_DEBOUNCE_COUNT		4U
#define TSADC_WARN_MILLIC		75000	/* armed, like both OSes ran */
#define TSADC_TSHUT_MILLIC		95000

/* GRF: sensor enable taps at 0x600, hiword write enable; TRM 18.5 wants
 * >= 10 us between the TSEN tap and the ANA taps and >= 90 us after them. */
#define RK3568_GRF_BASE			0xFDC60000UL
#define RK3568_GRF_TSADC_CON		0x0600U
#define RK3568_GRF_TSADC_TSEN		(0x10001UL << 8)
#define RK3568_GRF_TSADC_ANA_REG0	(0x10001UL << 0)
#define RK3568_GRF_TSADC_ANA_REG1	(0x10001UL << 1)
#define RK3568_GRF_TSADC_ANA_REG2	(0x10001UL << 2)

/* CRU note (2026-09-17 board experiment): the tsadc clock selector
 * CLKSEL_CON51 is IMMUTABLE from EL1 on this board - hiword, plain and
 * full-write-enable stores all read back 0 while neighbouring CON50/CON52
 * hold real values. The reference run that actually produced temperatures
 * here (NetBSD, 33.75 C, via its fixed-rate CRU stub) ran with CON51 at its
 * reset default (24 MHz straight through to sensor and ADC), so the default
 * clock state is what every working OS on this board has actually used. This
 * driver therefore touches NO CRU registers: the GRF taps and the register
 * sequence below are what matters. */

#define TSADC_BASE			0xFE710000UL

/* --- code table (Linux rk3568_code_table, via NetBSD rk_tsadc.c) ----------- */

struct rk_tsadc_entry {
	uint32_t data;
	int32_t temp_mc;
};

static const struct rk_tsadc_entry rk3568_table[] = {
	{ 0,	-40000 }, { 1584, -40000 }, { 1620, -35000 }, { 1652, -30000 },
	{ 1688, -25000 }, { 1720, -20000 }, { 1756, -15000 }, { 1788, -10000 },
	{ 1824,	 -5000 }, { 1856,      0 }, { 1892,   5000 }, { 1924,  10000 },
	{ 1956,	 15000 }, { 1992,  20000 }, { 2024,  25000 }, { 2060,  30000 },
	{ 2092,	 35000 }, { 2128,  40000 }, { 2160,  45000 }, { 2196,  50000 },
	{ 2228,	 55000 }, { 2264,  60000 }, { 2300,  65000 }, { 2332,  70000 },
	{ 2368,	 75000 }, { 2400,  80000 }, { 2436,  85000 }, { 2468,  90000 },
	{ 2500,	 95000 }, { 2536, 100000 }, { 2572, 105000 }, { 2604, 110000 },
	{ 2636, 115000 }, { 2672, 120000 }, { 2704, 125000 },
};

#define RK3568_TABLE_SIZE	(sizeof(rk3568_table) / sizeof(rk3568_table[0]))
#define RK3568_DATA_MIN		1584U
#define RK3568_DATA_MAX		2704U

static bool tsadc_inited;

static void rk_tsadc_udelay(uint32_t us)
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

/* Linear interpolation both ways: the manual's 5-degree table says the
 * spacing is linear between entries. */
static int32_t rk_tsadc_data_to_temp(uint32_t data)
{
	unsigned int i;

	if (data < RK3568_DATA_MIN || data > RK3568_DATA_MAX) {
		return INT32_MIN;	/* out of range: sensor not converting */
	}
	for (i = 1U; i < RK3568_TABLE_SIZE; i++) {
		if (rk3568_table[i].data >= data) {
			int32_t temprange;
			uint32_t datarange, datadiff;

			if (rk3568_table[i].data == data) {
				return rk3568_table[i].temp_mc;
			}
			temprange = rk3568_table[i].temp_mc -
				    rk3568_table[i - 1U].temp_mc;
			datarange = rk3568_table[i].data -
				    rk3568_table[i - 1U].data;
			datadiff = data - rk3568_table[i - 1U].data;
			return rk3568_table[i - 1U].temp_mc +
			       (int32_t)((uint32_t)temprange * datadiff / datarange);
		}
	}
	return INT32_MIN;
}

static uint32_t rk_tsadc_temp_to_data(int32_t temp_mc)
{
	unsigned int i;

	for (i = 1U; i < RK3568_TABLE_SIZE; i++) {
		if (rk3568_table[i].temp_mc >= temp_mc) {
			uint32_t datarange;
			int32_t temprange, tempdiff;

			if (rk3568_table[i].temp_mc == temp_mc) {
				return rk3568_table[i].data;
			}
			datarange = rk3568_table[i].data -
				    rk3568_table[i - 1U].data;
			temprange = rk3568_table[i].temp_mc -
				    rk3568_table[i - 1U].temp_mc;
			tempdiff = temp_mc - rk3568_table[i - 1U].temp_mc;
			return rk3568_table[i - 1U].data +
			       (uint32_t)((int32_t)datarange * tempdiff / temprange);
		}
	}
	return TSADC_DATA_MASK;
}

int rk_tsadc_init(void)
{
	uintptr_t base = TSADC_BASE;
	uintptr_t grf = RK3568_GRF_BASE + RK3568_GRF_TSADC_CON;
	uint32_t val;
	uint32_t warn_data, tshut_data;

	if (tsadc_inited) {
		return 0;
	}

	/* NOTE ON CLOCKS (2026-09-17 board experiment): the tsadc clock
	 * selector CLKSEL_CON51 is IMMUTABLE from EL1 on this board - hiword,
	 * plain and full-write-enable stores all read back 0 while the
	 * neighbouring CON50/CON52 hold real values. The reference run that
	 * actually produced temperatures on this board (NetBSD, 33.75 C) ran
	 * from its fixed-rate CRU stub, i.e. with this register at its reset
	 * default (xin24m /24 MHz straight through to both the sensor and the
	 * ADC). The sensor converts fine at that default - so we do not touch
	 * the CRU at all. Board facts kept: none of the CRU writes here are
	 * load-bearing; the GRF taps and the register sequence below are. */

	/* user conversion latency */
	reg_wr32(base + TSADC_USER_CON, RK3568_USER_INTER_PD_SOC);

	/* GRF taps: TSEN first, pause, then the analogue regs (TRM 18.5) */
	reg_wr32(grf, RK3568_GRF_TSADC_TSEN);
	rk_tsadc_udelay(15);
	reg_wr32(grf, RK3568_GRF_TSADC_ANA_REG0);
	reg_wr32(grf, RK3568_GRF_TSADC_ANA_REG1);
	reg_wr32(grf, RK3568_GRF_TSADC_ANA_REG2);
	rk_tsadc_udelay(100);

	/* sampling periods + debounce.
	 *
	 * The auto period interacts with the (immutable, default) 24 MHz
	 * clk_tsadc: the Linux-tuned 1622 ticks is 2.5 ms at the intended
	 * 700 kHz, but 67.5 us at 24 MHz - shorter than the 97 us conversion
	 * latency programmed in USER_CON, which deadlocks the auto engine
	 * (DATA stays 0 with every register otherwise correct: measured
	 * 2026-09-17, five boots). Pick a period that clears the latency at
	 * the ACTUAL clock: 48750 ticks = 2.03 ms at 24 MHz. */
	reg_wr32(base + TSADC_AUTO_PERIOD, RK3568_AUTO_PERIOD_TIME);
	reg_wr32(base + TSADC_AUTO_PERIOD_HT, RK3568_AUTO_PERIOD_TIME);
	reg_wr32(base + TSADC_HIGH_INT_DEBOUNCE, TSADC_HT_DEBOUNCE_COUNT);
	reg_wr32(base + TSADC_HIGH_TSHUT_DEBOUNCE, TSADC_HT_DEBOUNCE_COUNT);

	/* arm the warn/tshut comparators at the same points both OSes ran */
	warn_data = rk_tsadc_temp_to_data(TSADC_WARN_MILLIC);
	tshut_data = rk_tsadc_temp_to_data(TSADC_TSHUT_MILLIC);
	reg_wr32(base + TSADC_COMP0_INT, warn_data);
	reg_wr32(base + TSADC_COMP1_INT, warn_data);
	reg_wr32(base + TSADC_COMP0_SHUT, tshut_data);
	reg_wr32(base + TSADC_COMP1_SHUT, tshut_data);

	/* TSHUT routed to the GPIO tap, polarity low-active (defaults both
	 * OSes ran with; keeps the CRU reset path out of the picture) */
	val = reg_rd32(base + TSADC_INT_EN);
	val &= ~(TSADC_INT_EN_TSHUT_2CRU_SRC1 | TSADC_INT_EN_TSHUT_2CRU_SRC0);
	val |= TSADC_INT_EN_TSHUT_2GPIO_SRC1 | TSADC_INT_EN_TSHUT_2GPIO_SRC0;
	reg_wr32(base + TSADC_INT_EN, val);

	val = reg_rd32(base + TSADC_AUTO_CON);
	val &= ~TSADC_AUTO_CON_TSHUT_POLARITY;
	val |= TSADC_AUTO_CON_SRC0_LT_EN | TSADC_AUTO_CON_SRC1_LT_EN;
	reg_wr32(base + TSADC_AUTO_CON, val);

	/* auto status + low-trigger enables, then enable auto conversion */
	val = reg_rd32(base + TSADC_AUTO_CON);
	val |= TSADC_AUTO_CON_AUTO_STATUS |
	       TSADC_AUTO_CON_SRC0_LT_EN | TSADC_AUTO_CON_SRC1_LT_EN;
	reg_wr32(base + TSADC_AUTO_CON, val);

	val = reg_rd32(base + TSADC_AUTO_CON);
	val |= TSADC_AUTO_CON_AUTO_EN | TSADC_AUTO_CON_Q_SEL;
	reg_wr32(base + TSADC_AUTO_CON, val);

	/* first conversions need a couple of auto periods */
	rk_tsadc_udelay(10000);

	tsadc_inited = true;
	board_log("tsadc: ready (auto period %u ticks)", RK3568_AUTO_PERIOD_TIME);
	return 0;
}

int rk_tsadc_read_mc(int src, int32_t *temp_mc)
{
	uintptr_t base = TSADC_BASE;
	uint32_t reg = (src == RK_TSADC_SRC_GPU) ? TSADC_DATA1 : TSADC_DATA0;
	unsigned int retry;

	if (!tsadc_inited || temp_mc == NULL) {
		return -1;
	}

	for (retry = 0U; retry < 10U; retry++) {
		uint32_t data = reg_rd32(base + reg) & TSADC_DATA_MASK;
		int32_t t = rk_tsadc_data_to_temp(data);

		if (t != INT32_MIN) {
			*temp_mc = t;
			return 0;
		}
		rk_tsadc_udelay(5000);	/* two auto periods */
	}
	return -2;	/* sensor not converting */
}
