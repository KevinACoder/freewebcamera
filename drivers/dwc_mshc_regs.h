/*
 * @file   dwc_mshc_regs.h
 * @brief  DWC MSHC (SDHCI 4.20a register interface) register map and bit
 *         definitions, RK3568 eMMC controller @0xFE310000, TRM Part2 Ch7.
 *
 * Bit definitions agree with u-boot drivers/mmc/sdhci.c +
 * rockchip_sdhci.c and linux sdhci-of-dwcmshc.c; the CRU/GRF coordinates
 * are this SoC's. Kept alongside the driver they belong to (D27).
 *
 * ACCESS WIDTH IS PART OF THE SEQUENCE. The SDHCI register file mixes
 * 8/16/32-bit widths, and its odd-offset registers fault on a 32-bit store
 * (Device memory forbids unaligned access). Every register below is
 * annotated with its width and must be reached with the matching accessor.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWC_MSHC_REGS_H
#define FREEWEBCAMERA_DWC_MSHC_REGS_H

#include <stdbool.h>
#include <stdint.h>

#include "regs.h"

/* Rockchip CRU/GRF use a hiword write-enable store (see dwc_mmc_regs.h). */
#define RK3568_CRU_BASE_ADDR		0xFDD20000UL
#define RK3568_GRF_BASE_ADDR		0xFDC60000UL

/* CRU: clksel_con[28] CCLK_EMMC mux [14:12], BCLK mux [9:8];
 * clkgate_con[9] ACLK/HCLK/BCLK/CCLK/TCLK bits 5-9;
 * softrst_con[7] SRST_A/H/B/C/T_EMMC (117-121) bits 5-9. */
#define DWMSHC_CRU_CLKSEL_CON28_OFF	0x170U
#define DWMSHC_CRU_CLKGATE_CON9_OFF	0x324U
#define DWMSHC_CRU_SOFTRST_CON7_OFF	0x41CU
/* output (DRV) and input (SAMPLE) clock phase for the eMMC controller */
#define DWMSHC_CRU_EMMC_CON0_OFF	0x598U
#define DWMSHC_CRU_EMMC_CON1_OFF	0x59CU

#define DWMSHC_CRU_CCLK_EMMC_MUX_MASK	GENMASK(14, 12)
#define DWMSHC_CRU_CCLK_EMMC_MUX_SHIFT	12U
#define DWMSHC_CRU_BCLK_EMMC_MUX_MASK	GENMASK(9, 8)
#define DWMSHC_CRU_BCLK_EMMC_MUX_SHIFT	8U

#define DWMSHC_CRU_CCLK_EMMC_SEL_24M	0U
#define DWMSHC_CRU_CCLK_EMMC_SEL_200M	1U
#define DWMSHC_CRU_CCLK_EMMC_SEL_150M	2U
#define DWMSHC_CRU_CCLK_EMMC_SEL_100M	3U
#define DWMSHC_CRU_CCLK_EMMC_SEL_50M	4U
#define DWMSHC_CRU_CCLK_EMMC_SEL_375K	5U

#define DWMSHC_CRU_BCLK_EMMC_SEL_200M	0U
#define DWMSHC_CRU_BCLK_EMMC_SEL_150M	1U
#define DWMSHC_CRU_BCLK_EMMC_SEL_125M	2U

#define DWMSHC_CRU_EMMC_ACLK_GATE	(1u << 5)
#define DWMSHC_CRU_EMMC_HCLK_GATE	(1u << 6)
#define DWMSHC_CRU_EMMC_BCLK_GATE	(1u << 7)
#define DWMSHC_CRU_EMMC_CCLK_GATE	(1u << 8)
#define DWMSHC_CRU_EMMC_TCLK_GATE	(1u << 9)
#define DWMSHC_CRU_EMMC_GATE_ALL \
	(DWMSHC_CRU_EMMC_ACLK_GATE | DWMSHC_CRU_EMMC_HCLK_GATE | \
	 DWMSHC_CRU_EMMC_BCLK_GATE | DWMSHC_CRU_EMMC_CCLK_GATE | \
	 DWMSHC_CRU_EMMC_TCLK_GATE)

#define DWMSHC_CRU_EMMC_SOFTRST_A	(1u << 5)
#define DWMSHC_CRU_EMMC_SOFTRST_H	(1u << 6)
#define DWMSHC_CRU_EMMC_SOFTRST_B	(1u << 7)
#define DWMSHC_CRU_EMMC_SOFTRST_C	(1u << 8)
#define DWMSHC_CRU_EMMC_SOFTRST_T	(1u << 9)
#define DWMSHC_CRU_EMMC_SOFTRST_ALL \
	(DWMSHC_CRU_EMMC_SOFTRST_A | DWMSHC_CRU_EMMC_SOFTRST_H | \
	 DWMSHC_CRU_EMMC_SOFTRST_B | DWMSHC_CRU_EMMC_SOFTRST_C | \
	 DWMSHC_CRU_EMMC_SOFTRST_T)

/* GRF: eMMC pin group (GPIO1_B4~B7 + C0~C7, fn1). Iomux is 4-bit fields with
 * hiword write-enable; pull is 2 bits per pin; drive strength 8 bits per pin
 * (2 pins per register), level 2 = 0x07 (linux formula (1<<(level+1))-1). */
#define DWMSHC_GRF_GPIO1B_IOMUX_H_OFF	0x0CU /* GPIO1_B4~B7 */
#define DWMSHC_GRF_GPIO1C_IOMUX_L_OFF	0x10U /* GPIO1_C0~C3 */
#define DWMSHC_GRF_GPIO1C_IOMUX_H_OFF	0x14U /* GPIO1_C4~C7 */

#define DWMSHC_GRF_GPIO1B_PULL_OFF	0x84U /* B0~B7, 2-bit/pin */
#define DWMSHC_GRF_GPIO1C_PULL_OFF	0x88U /* C0~C7, 2-bit/pin */

#define DWMSHC_GRF_GPIO1B_DS_2_OFF	0x218U /* B4/B5 */
#define DWMSHC_GRF_GPIO1B_DS_3_OFF	0x21CU /* B6/B7 */
#define DWMSHC_GRF_GPIO1C_DS_0_OFF	0x220U /* C0/C1 */
#define DWMSHC_GRF_GPIO1C_DS_1_OFF	0x224U /* C2/C3 */
#define DWMSHC_GRF_GPIO1C_DS_2_OFF	0x228U /* C4/C5 */
#define DWMSHC_GRF_GPIO1C_DS_3_OFF	0x22CU /* C6/C7 */

/* --- register map (offsets from the controller base) ----------------------- */

#define DWMSHC_SDMASA_OFFSET		0x000U /* 32-bit: SDMA system address */
#define DWMSHC_BLOCKSIZE_OFFSET		0x004U /* 16-bit */
#define DWMSHC_BLOCKCOUNT_OFFSET	0x006U /* 16-bit */
#define DWMSHC_ARGUMENT_OFFSET		0x008U /* 32-bit */
#define DWMSHC_XFER_MODE_OFFSET		0x00CU /* 16-bit */
#define DWMSHC_CMD_OFFSET		0x00EU /* 16-bit */
#define DWMSHC_RESP01_OFFSET		0x010U /* 32-bit */
#define DWMSHC_RESP23_OFFSET		0x014U /* 32-bit */
#define DWMSHC_RESP45_OFFSET		0x018U /* 32-bit */
#define DWMSHC_RESP67_OFFSET		0x01CU /* 32-bit */
#define DWMSHC_BUF_DATA_OFFSET		0x020U /* 32-bit */
#define DWMSHC_PSTATE_OFFSET		0x024U /* 32-bit */
#define DWMSHC_HOST_CTRL1_OFFSET	0x028U /* 8-bit */
#define DWMSHC_PWR_CTRL_OFFSET		0x029U /* 8-bit */
#define DWMSHC_BGAP_CTRL_OFFSET		0x02AU /* 8-bit */
#define DWMSHC_CLK_CTRL_OFFSET		0x02CU /* 16-bit */
#define DWMSHC_TOUT_CTRL_OFFSET		0x02EU /* 8-bit */
#define DWMSHC_SW_RST_OFFSET		0x02FU /* 8-bit */
#define DWMSHC_NORMAL_INT_STAT_OFF	0x030U /* 16-bit */
#define DWMSHC_ERR_INT_STAT_OFF		0x032U /* 16-bit */
#define DWMSHC_NORMAL_INT_EN_OFF	0x034U /* 16-bit */
#define DWMSHC_ERR_INT_EN_OFF		0x036U /* 16-bit */
#define DWMSHC_NORMAL_INT_SIG_OFF	0x038U /* 16-bit */
#define DWMSHC_ERR_INT_SIG_OFF		0x03AU /* 16-bit */
#define DWMSHC_AUTO_CMD_STAT_OFF	0x03CU /* 16-bit */
#define DWMSHC_HOST_CTRL2_OFFSET	0x03EU /* 16-bit */
#define DWMSHC_CAPABILITIES1_OFF	0x040U /* 32-bit */
#define DWMSHC_CAPABILITIES2_OFF	0x044U /* 32-bit */
#define DWMSHC_ADMA_ERR_STAT_OFF	0x054U /* 8-bit */
#define DWMSHC_ADMA_SA_OFFSET		0x058U /* 32-bit */
#define DWMSHC_HOST_CNTRL_VERS_OFF	0x0FEU /* 16-bit */

/* DWC MSHC vendor region (TRM Ch7 / linux sdhci-of-dwcmshc.c), 32-bit. */
#define DWMSHC_HOST_CTRL3_OFFSET	0x508U /* bit0 CMD_CONFLICT_CHECK */
#define DWMSHC_EMMC_CTRL_OFFSET		0x52CU /* bit0 CARD_IS_EMMC, bit2 RST_N, bit3 RST_N_OE */
#define DWMSHC_AT_CTRL_OFFSET		0x540U /* auto-tuning (>=100MHz) */
#define DWMSHC_MISC_CON_OFFSET		0x81CU /* bit1 INTCLK_EN */
#define DWMSHC_DLL_CTRL_OFFSET		0x800U
#define DWMSHC_DLL_RXCLK_OFFSET		0x804U
#define DWMSHC_DLL_TXCLK_OFFSET		0x808U
#define DWMSHC_DLL_STRBIN_OFFSET	0x80CU
#define DWMSHC_DLL_CMDOUT_OFFSET	0x810U
#define DWMSHC_DLL_STATUS0_OFFSET	0x840U

/* --- XFER_MODE (0x0C, 16-bit) ----------------------------------------------- */

#define DWMSHC_XFER_DMA_EN		(1u << 0)
#define DWMSHC_XFER_BLK_CNT_EN		(1u << 1)
#define DWMSHC_XFER_AUTO_CMD_EN(x)	((x) << 2)
#define DWMSHC_XFER_DATA_DIR_READ	(1u << 4)
#define DWMSHC_XFER_MULTI_BLK		(1u << 5)

/* --- CMD (0x0E, 16-bit) ------------------------------------------------------ */

#define DWMSHC_CMD_INDEX(x)		(((x) & 0x3FU) << 8)
#define DWMSHC_CMD_RESP_TYPE_NONE	0U
#define DWMSHC_CMD_RESP_TYPE_136	(1u << 0)
#define DWMSHC_CMD_RESP_TYPE_48		(1u << 1)
#define DWMSHC_CMD_RESP_TYPE_48_BUSY	((1u << 1) | (1u << 0))
#define DWMSHC_CMD_CRC_CHK		(1u << 3)
#define DWMSHC_CMD_IDX_CHK		(1u << 4)
#define DWMSHC_CMD_DATA_PRESENT		(1u << 5)
#define DWMSHC_CMD_TYPE_ABORT		(3U << 6)

/* --- PSTATE (0x24, 32-bit) ---------------------------------------------------- */

#define DWMSHC_PSTATE_CMD_INHIBIT	(1u << 0)
#define DWMSHC_PSTATE_DAT_INHIBIT	(1u << 1)
#define DWMSHC_PSTATE_DATA_0_LVL	(1u << 20)
#define DWMSHC_PSTATE_CARD_INSERTED	(1u << 16)
#define DWMSHC_PSTATE_CARD_STABLE	(1u << 17)
#define DWMSHC_PSTATE_DAT_INHIBIT_ALL	GENMASK(2, 1)

/* --- HOST_CTRL1 (0x28, 8-bit) -------------------------------------------------- */

#define DWMSHC_HC1_DAT_XFER_WIDTH	(1u << 1) /* 4-bit bus */
#define DWMSHC_HC1_HIGH_SPEED		(1u << 2)
#define DWMSHC_HC1_DMA_SEL_SDMA		0U
#define DWMSHC_HC1_DMA_SEL_ADMA2	(2U << 3)
#define DWMSHC_HC1_DMA_SEL_ADMA2_3	(3U << 3)
#define DWMSHC_HC1_EXT_DAT_XFER		(1u << 5) /* 8-bit bus */

/* --- PWR_CTRL (0x29, 8-bit) ----------------------------------------------------- */

/* SD Bus Voltage Select lives in bits[3:1]: 0b101 = 1.8V, 0b110 = 3.0V,
 * 0b111 = 3.3V. (The standalone line carried these as "(x << 1)" written into
 * a byte macro, which lands one bit too high and selects 3.0V when asked for
 * 3.3V; the board's eMMC IO rail is vcc_1v8, and u-boot programs 0x0b =
 * 1.8V | bus power.) */
#define DWMSHC_PWR_VDD_1_8V		(0x5U << 1)
#define DWMSHC_PWR_VDD_3_0V		(0x6U << 1)
#define DWMSHC_PWR_VDD_3_3V		(0x7U << 1)
#define DWMSHC_PWR_SD_BUS_POWER		(1u << 0)

/* --- CLK_CTRL (0x2C, 16-bit) ----------------------------------------------------- */

#define DWMSHC_CLK_INT_EN		(1u << 0)
#define DWMSHC_CLK_INT_STABLE		(1u << 1)
#define DWMSHC_CLK_SD_EN		(1u << 2)
#define DWMSHC_CLK_PROG_MODE		(1u << 5)
#define DWMSHC_CLK_FREQ_SEL(x)		(((x) & 0x3FFU) << 8)

/* --- SW_RST (0x2F, 8-bit) --------------------------------------------------------- */

#define DWMSHC_SW_RST_ALL		(1u << 0)
#define DWMSHC_SW_RST_CMD		(1u << 1)
#define DWMSHC_SW_RST_DAT		(1u << 2)

/* --- NORMAL_INT_STAT (0x30, 16-bit) ------------------------------------------------ */

#define DWMSHC_NINT_CMD_COMPLETE	(1u << 0)
#define DWMSHC_NINT_XFER_COMPLETE	(1u << 1)
#define DWMSHC_NINT_BGAP_EVENT		(1u << 2)
#define DWMSHC_NINT_DMA_INTERRUPT	(1u << 3)
#define DWMSHC_NINT_BUF_WR_READY	(1u << 4)
#define DWMSHC_NINT_BUF_RD_READY	(1u << 5)
#define DWMSHC_NINT_CARD_INSERTION	(1u << 6)
#define DWMSHC_NINT_CARD_REMOVAL	(1u << 7)
#define DWMSHC_NINT_CARD_INTERRUPT	(1u << 8)
#define DWMSHC_NINT_CQE_EVENT		(1u << 14)
#define DWMSHC_NINT_ERR_INT		(1u << 15)
#define DWMSHC_NINT_ALL			0xFFFFU

/* --- ERR_INT_STAT (0x32, 16-bit) ---------------------------------------------------- */

#define DWMSHC_EINT_CMD_CRC		(1u << 0)
#define DWMSHC_EINT_CMD_TIMEOUT		(1u << 1)
#define DWMSHC_EINT_CMD_END_BIT		(1u << 2)
#define DWMSHC_EINT_CMD_IDX		(1u << 3)
#define DWMSHC_EINT_DATA_TIMEOUT	(1u << 4)
#define DWMSHC_EINT_DATA_CRC		(1u << 5)
#define DWMSHC_EINT_DATA_END_BIT	(1u << 6)
#define DWMSHC_EINT_CURRENT_LIMIT	(1u << 7)
#define DWMSHC_EINT_AUTO_CMD_ERR	(1u << 8)
#define DWMSHC_EINT_ADMA_ERR		(1u << 9)
#define DWMSHC_EINT_TUNING_ERR		(1u << 10)
#define DWMSHC_EINT_RESP_ERR		(1u << 11)
#define DWMSHC_EINT_ALL			0xFFFFU

#define DWMSHC_CMD_ERR_FLAGS \
	(DWMSHC_EINT_CMD_CRC | DWMSHC_EINT_CMD_TIMEOUT | DWMSHC_EINT_CMD_END_BIT | \
	 DWMSHC_EINT_CMD_IDX | DWMSHC_EINT_RESP_ERR)
#define DWMSHC_DATA_ERR_FLAGS \
	(DWMSHC_EINT_DATA_TIMEOUT | DWMSHC_EINT_DATA_CRC | \
	 DWMSHC_EINT_DATA_END_BIT | DWMSHC_EINT_ADMA_ERR)

/* --- HOST_CTRL2 (0x3E, 16-bit) ------------------------------------------------------ */

#define DWMSHC_HC2_UHS_MODE(x)		((x) & 0x7U) /* 1=SDR25/HS, 3=SDR104/HS200, 7=HS400 */
#define DWMSHC_HC2_DRV_TYPE(x)		((x) << 4)
#define DWMSHC_HC2_EXEC_TUNING		(1u << 6)
#define DWMSHC_HC2_TUNED_CLK		(1u << 7)
#define DWMSHC_HC2_SIGNALING_1_8V	(1u << 3)
#define DWMSHC_HC2_HOST_VER4_EN		(1u << 12)

/* --- CAPABILITIES1 (0x40, 32-bit) ----------------------------------------------------- */

#define DWMSHC_CAP1_8BIT		(1u << 18)
#define DWMSHC_CAP1_ADMA2		(1u << 19)
#define DWMSHC_CAP1_HIGH_SPEED		(1u << 21)
#define DWMSHC_CAP1_SDMA		(1u << 22)

/* --- DLL (0x800+, u-boot/linux dwcmshc consistent) ------------------------------------- */

#define DWMSHC_DLL_START		(1u << 0)
#define DWMSHC_DLL_SRST			(1u << 1)
#define DWMSHC_DLL_INCRMENT(x)		((x) << 8)
#define DWMSHC_DLL_START_POINT(x)	((x) << 16)
#define DWMSHC_DLL_BYPASS		(1u << 24)
#define DWMSHC_DLL_DLYENA		(1u << 27)

#define DWMSHC_DLL_RXCLK_ORI_GATE	(1u << 31)
#define DWMSHC_DLL_RXCLK_SRC_SEL	(1u << 29)
#define DWMSHC_DLL_TAP_VALUE_SEL	(1u << 25)
#define DWMSHC_DLL_TAP_VALUE_OFFSET	8U
#define DWMSHC_DLL_RXCLK_DLYENA		(1u << 27)
#define DWMSHC_DLL_RXCLK_NO_INVERTER	(1u << 29)

#define DWMSHC_DLL_TXCLK_TAPNUM_FROM_SW	(1u << 24)
#define DWMSHC_DLL_TXCLK_DLYENA		(1u << 27)
#define DWMSHC_DLL_STRBIN_DELAY_NUM_SEL	(1u << 26)
#define DWMSHC_DLL_STRBIN_DELAY_NUM_OFF	16U
#define DWMSHC_DLL_STRBIN_TAPNUM_FROM_SW (1u << 24)

#define DWMSHC_DLL_STATUS_LOCKED	(1u << 8)
#define DWMSHC_DLL_STATUS_TIMEOUT	(1u << 9)
#define DWMSHC_DLL_STATUS_LOCK_VAL(x)	((x) & 0xFFU)

#define DWMSHC_MISC_INTCLK_EN		(1u << 1)

#ifndef GENMASK
#define GENMASK(h, l) (((1u << ((h) - (l) + 1U)) - 1U) << (l))
#endif

/* --- accessors ---------------------------------------------------------------------- */

#define DWMSHC_READ_REG(addr, reg_offset) reg_rd32((addr) + (uintptr_t)(reg_offset))
#define DWMSHC_WRITE_REG(addr, reg_offset, value) \
	reg_wr32((addr) + (uintptr_t)(reg_offset), (uint32_t)(value))

#define DWMSHC_READ_HALF(addr, reg_offset) reg_rd16((addr) + (uintptr_t)(reg_offset))
#define DWMSHC_WRITE_HALF(addr, reg_offset, value) \
	reg_wr16((addr) + (uintptr_t)(reg_offset), (uint16_t)(value))

#define DWMSHC_READ_BYTE(addr, reg_offset) reg_rd8((addr) + (uintptr_t)(reg_offset))
#define DWMSHC_WRITE_BYTE(addr, reg_offset, value) \
	reg_wr8((addr) + (uintptr_t)(reg_offset), (uint8_t)(value))

/* Clear all interrupt status (write-one-clear, 16-bit registers). */
static inline void dwc_mshc_clear_interrupt_status(uintptr_t base_addr)
{
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_NORMAL_INT_STAT_OFF, DWMSHC_NINT_ALL);
	DWMSHC_WRITE_HALF(base_addr, DWMSHC_ERR_INT_STAT_OFF, DWMSHC_EINT_ALL);
}

#endif /* FREEWEBCAMERA_DWC_MSHC_REGS_H */
