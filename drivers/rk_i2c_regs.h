/*
 * @file   rk_i2c_regs.h
 * @brief  RK3568 I2C controller register map and platform coordinates.
 *
 * The controller is Rockchip's own I2C v5 block (compatible
 * "rockchip,rk3399-i2c"), NOT Synopsys DesignWare - the CON/CLKDIV/MRXADDR
 * layout below has no IC_CON/IC_TAR family anywhere in it. (An older design
 * note pointed at a DesignWare reference driver; that misidentification is
 * corrected in decision D29.)
 *
 * Every offset and bit field was verified against TRM Part1 Ch22 and against
 * the author's own working RK3568 port in the standalone line, and carried
 * over line-for-line (D20 policy: register sequences are the debugged truth;
 * vocabulary is all that changes).
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#ifndef FREEWEBCAMERA_RK_I2C_REGS_H
#define FREEWEBCAMERA_RK_I2C_REGS_H

#include <stdint.h>

/* --- register block (i2c0 @ 0xFDD40000, i2c1 @ 0xFE5A0000) ---------------- */

#define RK_I2C_CON			0x000U	/* control */
#define RK_I2C_CLKDIV			0x004U	/* SCL divider */
#define RK_I2C_MRXADDR			0x008U	/* master-read slave address */
#define RK_I2C_MRXRADDR			0x00CU	/* master-read register bytes */
#define RK_I2C_MTXCNT			0x010U	/* tx byte count (write triggers) */
#define RK_I2C_MRXCNT			0x014U	/* rx byte count (write triggers) */
#define RK_I2C_IEN			0x018U	/* interrupt enable */
#define RK_I2C_IPD			0x01CU	/* interrupt pending, W1C */
#define RK_I2C_FCNT			0x020U	/* FIFO count */
#define RK_I2C_TXDATA			0x100U	/* TXDATA0..7 @0x100..0x11C */
#define RK_I2C_RXDATA			0x200U	/* RXDATA0..7 @0x200..0x21C */

/* FIFO holds 32 bytes packed 4/word, little-endian, first byte in bits[7:0]. */
#define RK_I2C_FIFO_SIZE		32U

/* CON bits */
#define RK_I2C_CON_EN			(1UL << 0)
#define RK_I2C_CON_MOD_TX		(0UL << 1)
#define RK_I2C_CON_MOD_TRX		(1UL << 1)	/* addr + restart + read */
#define RK_I2C_CON_MOD_RX		(2UL << 1)	/* continue a split read */
#define RK_I2C_CON_START		(1UL << 3)
#define RK_I2C_CON_STOP			(1UL << 4)
#define RK_I2C_CON_LASTACK		(1UL << 5)	/* NAK the final rx byte */
#define RK_I2C_CON_SDA_CFG(x)		((uint32_t)(x) << 8)
#define RK_I2C_CON_STA_CFG(x)		((uint32_t)(x) << 12)
#define RK_I2C_CON_VERSION_SHIFT	16U	/* reset value 0x003 = v5 */

/* CLKDIV: SCL = PCLK / (8 * (CLKDIVL + 1 + CLKDIVH + 1)) */
#define RK_I2C_CLKDIV_HIGH_SHIFT	16U

/* IEN/IPD bits (same positions in both registers; IPD is W1C).
 * bits 0/1 are slave-mode only; master uses bits 2..6. */
#define RK_I2C_INT_MBTF			(1UL << 2)
#define RK_I2C_INT_MBRF			(1UL << 3)
#define RK_I2C_INT_START		(1UL << 4)
#define RK_I2C_INT_STOP			(1UL << 5)
#define RK_I2C_INT_NAKRCV		(1UL << 6)
#define RK_I2C_IPD_ALL_CLEAN		0x7FU

/* MRXADDR / MRXRADDR: bit24 marks the packed byte(s) valid. */
#define RK_I2C_MRX_VALID		(1UL << 24)

/* --- platform coordinates ------------------------------------------------- */
/* CRU/GRF family: hiword write-enable - value in the low 16 bits, per-bit
 * write enable in the high 16 bits ((mask << 16) | value). */

#define RK_I2C_PMUGRF_BASE		0xFDC20000UL
#define RK_I2C_PMUGRF_GPIO0B_IOMUX_L	0x08U
#define RK_I2C_PMUGRF_GPIO0B_IOMUX_H	0x0CU

/* i2c0 (PMU domain): GPIO0_B1=SCL / B2=SDA, function 1 */
#define RK_I2C0_IOMUX_L_MASK		0x0FF0U
#define RK_I2C0_IOMUX_L_VAL		0x0110U

/* i2c1: GPIO0_B3=SCL (IOMUX_L bits[14:12]) / B4=SDA (IOMUX_H bits[2:0]), fn1 */
#define RK_I2C1_IOMUX_L_MASK		0x7000U
#define RK_I2C1_IOMUX_L_VAL		0x1000U
#define RK_I2C1_IOMUX_H_MASK		0x0007U
#define RK_I2C1_IOMUX_H_VAL		0x0001U

/* i2c0 clocks live in the PMUCRU: CLKSEL_CON(x)=x*4+0x100,
 * CLKGATE_CON(x)=x*4+0x180, SOFTRST_CON(x)=x*4+0x200. */
#define RK_I2C0_PMUCRU_BASE		0xFDD00000UL
#define RK_I2C0_CLKSEL_CON3		0x10CU	/* clk_i2c0 div bits[6:0] */
#define RK_I2C0_CLKSEL_DIV_MASK		0x007FU
#define RK_I2C0_CLKSEL_DIV_100M		1U	/* ppll 200M / (1+1) */
#define RK_I2C0_CLKGATE_CON1		0x184U	/* bit0=pclk_i2c0 bit1=clk_i2c0 */
#define RK_I2C0_GATE_MASK		0x3U
#define RK_I2C0_SOFTRST_CON0		0x200U	/* bit3=p_rst bit4=rst */
#define RK_I2C0_RST_MASK		((1UL << 3) | (1UL << 4))

/* i2c1 clocks live in the CRU: CLKSEL_CON(x)=x*4+0x100,
 * CLKGATE_CON(x)=x*4+0x300, SOFTRST_CON(x)=x*4+0x400. */
#define RK_I2C1_CRU_BASE		0xFDD20000UL
#define RK_I2C1_CLKSEL_CON71		0x21CU	/* clk_i2c mux bits[9:8] */
#define RK_I2C1_MUX_SHIFT		8U
#define RK_I2C1_MUX_MASK		(0x3UL << RK_I2C1_MUX_SHIFT)
#define RK_I2C1_MUX_SEL_100M		1U	/* gpll_100m (nominal 100M) */
#define RK_I2C1_CLKGATE_CON30		0x378U	/* bit0=pclk_i2c1 bit1=clk_i2c1 */
#define RK_I2C1_GATE_MASK		0x3U
#define RK_I2C1_CLKGATE_CON32		0x380U	/* bit10 = shared clk_i2c */
#define RK_I2C1_SHARED_GATE_BIT		10U
#define RK_I2C1_SOFTRST_CON22		0x458U	/* bit2=p_rst bit3=rst */
#define RK_I2C1_RST_MASK		((1UL << 2) | (1UL << 3))

/* i2c0/i2c1 nominal reference for the SCL divider (both buses use the same
 * value; the ~1% actual deviation on i2c1 lands SCL at ~101 kHz). */
#define RK_I2C_REF_CLK_HZ		100000000U

/* Per-wait poll budget: a 32-byte chunk at 100 kHz takes < 3 ms. */
#define RK_I2C_TIMEOUT_US		50000U

#endif /* FREEWEBCAMERA_RK_I2C_REGS_H */
