/*
 * @file   dwc_mmc_regs.h
 * @brief  DW-MMC (DesignWare Mobile Storage Host) register map and bit
 *         definitions, RK3568 sdmmc0/sdmmc1, TRM Part2 Ch6.
 *
 * Bit definitions agree with u-boot drivers/mmc/dw_mmc.c and linux dw_mmc.h;
 * the CRU coordinates are this SoC's. Kept alongside the driver they belong
 * to (D27): the register sequences in dwc_mmc.c are only meaningful against
 * these exact fields.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWC_MMC_REGS_H
#define FREEWEBCAMERA_DWC_MMC_REGS_H

#include <stdint.h>

#include "regs.h"

/* Rockchip CRU/GRF registers use a hiword write-enable: the write carries
 * ((mask & 0xffff) << 16) | (val & mask). CRU base 0xFDD20000. */
#define RK3568_CRU_BASE_ADDR		0xFDD20000UL

#define DWMMC_CRU_CLKSEL_CON_OFF	0x178U /* clksel_con[30] */
#define DWMMC_CRU_GATE_CON_OFF		0x33CU /* clkgate_con[15] */
#define DWMMC_CRU_SOFTRST_CON_OFF	0x434U /* softrst_con[13] */

#define DWMMC_CRU_SDMMC0_MUX_MASK	GENMASK(10, 8)
#define DWMMC_CRU_SDMMC0_MUX_SHIFT	8U
#define DWMMC_CRU_SDMMC1_MUX_MASK	GENMASK(14, 12)
#define DWMMC_CRU_SDMMC1_MUX_SHIFT	12U

#define DWMMC_CRU_SDMMC0_HCLK_GATE	(1u << 0)
#define DWMMC_CRU_SDMMC0_CLK_GATE	(1u << 1)
#define DWMMC_CRU_SDMMC1_HCLK_GATE	(1u << 2)
#define DWMMC_CRU_SDMMC1_CLK_GATE	(1u << 3)

#define DWMMC_CRU_SDMMC0_SOFTRST	(1u << 4)
#define DWMMC_CRU_SDMMC1_SOFTRST	(1u << 6)

/* CLK_SDMMCx source selection (clksel_con[30]). */
#define DWMMC_CRU_SDMMC_SEL_24M		0U
#define DWMMC_CRU_SDMMC_SEL_400M	1U
#define DWMMC_CRU_SDMMC_SEL_300M	2U
#define DWMMC_CRU_SDMMC_SEL_100M	3U
#define DWMMC_CRU_SDMMC_SEL_50M		4U
#define DWMMC_CRU_SDMMC_SEL_750K	5U

/* Sample phase (CRU sdmmc0_con[1] / sdmmc1_con[1], linux clk-mmc-phase). */
#define DWMMC_CRU_SDMMC0_PHASE_CON_OFF	0x584U
#define DWMMC_CRU_SDMMC1_PHASE_CON_OFF	0x58CU

/* --- register map (offsets from the controller base) ----------------------- */

#define DWMMC_CTRL_OFFSET		0x000U
#define DWMMC_PWREN_OFFSET		0x004U
#define DWMMC_CLKDIV_OFFSET		0x008U
#define DWMMC_CLKSRC_OFFSET		0x00CU
#define DWMMC_CLKENA_OFFSET		0x010U
#define DWMMC_TMOUT_OFFSET		0x014U
#define DWMMC_CTYPE_OFFSET		0x018U
#define DWMMC_BLKSIZ_OFFSET		0x01CU
#define DWMMC_BYTCNT_OFFSET		0x020U
#define DWMMC_INTMASK_OFFSET		0x024U
#define DWMMC_CMDARG_OFFSET		0x028U
#define DWMMC_CMD_OFFSET		0x02CU
#define DWMMC_RESP0_OFFSET		0x030U
#define DWMMC_RESP1_OFFSET		0x034U
#define DWMMC_RESP2_OFFSET		0x038U
#define DWMMC_RESP3_OFFSET		0x03CU
#define DWMMC_MINTSTS_OFFSET		0x040U
#define DWMMC_RINTSTS_OFFSET		0x044U
#define DWMMC_STATUS_OFFSET		0x048U

/* Rockchip extensions (dw_mmc-rockchip SDMMC_TIMING_CON0/1): the card
 * clock drive/sample phases, HIWORD-encoded like the CRU (value bits
 * [10:1], write-enable bits [26:17]). Ground truth = the vendor kernel's
 * dw_mmc-rockchip.c set_ios: drive 90 degrees, sample 0 degrees
 * (rk356x.dtsi carries no default-sample-phase). */
#define DWMMC_TIMING_CON0_OFFSET	0x130U
#define DWMMC_TIMING_CON1_OFFSET	0x134U
#define DWMMC_TIMING_HIWORD(raw)					\
	((((uint32_t) (raw) & 0x7ffu) << 1) | (0x7ffu << 17))
#define DWMMC_TIMING_CON_DRIVE_90	DWMMC_TIMING_HIWORD(1u)
#define DWMMC_TIMING_CON_SAMPLE_0	DWMMC_TIMING_HIWORD(0u)
#define DWMMC_FIFOTH_OFFSET		0x04CU
#define DWMMC_CDETECT_OFFSET		0x050U
#define DWMMC_WRTPRT_OFFSET		0x054U
#define DWMMC_DEBNCE_OFFSET		0x064U
#define DWMMC_USRID_OFFSET		0x068U
#define DWMMC_VERID_OFFSET		0x06CU
#define DWMMC_HCON_OFFSET		0x070U
#define DWMMC_UHSREG_OFFSET		0x074U
#define DWMMC_RSTN_OFFSET		0x078U
#define DWMMC_BMOD_OFFSET		0x080U
#define DWMMC_PLDMND_OFFSET		0x084U
#define DWMMC_DBADDR_OFFSET		0x088U
#define DWMMC_IDSTS_OFFSET		0x08CU
#define DWMMC_IDINTEN_OFFSET		0x090U
#define DWMMC_DSCADDR_OFFSET		0x094U
#define DWMMC_BUFADDR_OFFSET		0x098U
#define DWMMC_CARDTHRCTL_OFFSET		0x100U

/* --- CTRL ------------------------------------------------------------------- */

#define DWMMC_CTRL_CONTROLLER_RESET	(1u << 0)
#define DWMMC_CTRL_FIFO_RESET		(1u << 1)
#define DWMMC_CTRL_DMA_RESET		(1u << 2)
#define DWMMC_CTRL_INT_ENABLE		(1u << 4)
#define DWMMC_CTRL_DMA_ENABLE		(1u << 5)
#define DWMMC_CTRL_READ_WAIT		(1u << 6)
#define DWMMC_CTRL_USE_IDMAC		(1u << 25)
#define DWMMC_CTRL_ALL_RESET_FLAGS \
	(DWMMC_CTRL_CONTROLLER_RESET | DWMMC_CTRL_FIFO_RESET | DWMMC_CTRL_DMA_RESET)

/* --- CMD (TRM 6.4.2, index in [5:0]) ---------------------------------------- */

#define DWMMC_CMD_INDX(n)		((n) & 0x3FU)
#define DWMMC_CMD_RESP_EXP		(1u << 6)
#define DWMMC_CMD_RESP_LENGTH		(1u << 7)
#define DWMMC_CMD_CHECK_CRC		(1u << 8)
#define DWMMC_CMD_DATA_EXP		(1u << 9)
#define DWMMC_CMD_RW			(1u << 10)
#define DWMMC_CMD_TRANS_MODE		(1u << 11)
#define DWMMC_CMD_SEND_AUTO_STOP	(1u << 12)
#define DWMMC_CMD_WAIT_PRV_DAT		(1u << 13)
#define DWMMC_CMD_STOP_ABORT		(1u << 14)
#define DWMMC_CMD_SEND_INIT		(1u << 15)
#define DWMMC_CMD_UPD_CLK		(1u << 21)
#define DWMMC_CMD_VOLT_SWITCH		(1u << 28)
#define DWMMC_CMD_USE_HOLD_REG		(1u << 29)
#define DWMMC_CMD_START			(1u << 31)

/* --- UHS_REG (0x74): 1.8V signal select ------------------------------------- */

#define DWMMC_UHSREG_18V		(1u << 0)

/* --- RINTSTS / INTMSK -------------------------------------------------------- */

#define DWMMC_INTMSK_CD			(1u << 0)
#define DWMMC_INTMSK_RE			(1u << 1)
#define DWMMC_INTMSK_CDONE		(1u << 2)
#define DWMMC_INTMSK_DTO		(1u << 3)
#define DWMMC_INTMSK_TXDR		(1u << 4)
#define DWMMC_INTMSK_RXDR		(1u << 5)
#define DWMMC_INTMSK_RCRC		(1u << 6)
#define DWMMC_INTMSK_DCRC		(1u << 7)
#define DWMMC_INTMSK_RTO		(1u << 8)
#define DWMMC_INTMSK_DRTO		(1u << 9)
#define DWMMC_INTMSK_HTO		(1u << 10)
#define DWMMC_INTMSK_FRUN		(1u << 11)
#define DWMMC_INTMSK_HLE		(1u << 12)
#define DWMMC_INTMSK_SBE		(1u << 13)
#define DWMMC_INTMSK_ACD		(1u << 14)
#define DWMMC_INTMSK_EBE		(1u << 15)
#define DWMMC_INTMSK_SDIO_INT		(1u << 16)

#define DWMMC_DATA_ERR_FLAGS \
	(DWMMC_INTMSK_EBE | DWMMC_INTMSK_SBE | DWMMC_INTMSK_HLE | \
	 DWMMC_INTMSK_FRUN | DWMMC_INTMSK_DCRC | DWMMC_INTMSK_RCRC)
#define DWMMC_DATA_TOUT_FLAGS		(DWMMC_INTMSK_HTO | DWMMC_INTMSK_DRTO)
#define DWMMC_CMD_ERR_FLAGS		(DWMMC_INTMSK_RTO | DWMMC_INTMSK_RE)
#define DWMMC_INTMSK_ALL		0xFFFFFFFFU

/* --- STATUS ------------------------------------------------------------------ */

#define DWMMC_STATUS_DATA_BUSY		(1u << 9)
#define DWMMC_STATUS_GET_FCNT(x)	(((x) >> 17) & 0x1FFFU)

/* --- FIFOTH ------------------------------------------------------------------ */

#define DWMMC_FIFOTH_MSIZE(x)		((x) << 28)
#define DWMMC_FIFOTH_RX_WMARK(x)	((x) << 16)
#define DWMMC_FIFOTH_TX_WMARK(x)	((x) & 0xFFFF)

/* --- CLKENA ------------------------------------------------------------------ */

#define DWMMC_CLKEN_ENABLE		(1u << 0)
#define DWMMC_CLKEN_LOW_PWR		(1u << 16)

/* --- CTYPE (card 0 fields only, single card per controller) ------------------ */

#define DWMMC_CTYPE_1BIT		0U
#define DWMMC_CTYPE_4BIT		(1u << 0)
#define DWMMC_CTYPE_8BIT		(1u << 16)

/* --- IDMAC (BMOD / IDSTS / IDINTEN) ------------------------------------------ */

#define DWMMC_IDMAC_SWRESET		(1u << 0)
#define DWMMC_IDMAC_FB			(1u << 1)
#define DWMMC_IDMAC_ENABLE		(1u << 7)

#define DWMMC_IDMAC_INT_TI		(1u << 0)
#define DWMMC_IDMAC_INT_RI		(1u << 1)
#define DWMMC_IDMAC_INT_FBE		(1u << 2)
#define DWMMC_IDMAC_INT_DU		(1u << 4)
#define DWMMC_IDMAC_INT_CES		(1u << 5)
#define DWMMC_IDMAC_INT_NI		(1u << 8)
#define DWMMC_IDMAC_INT_AI		(1u << 9)
/* fatal IDMAC status: fatal bus error, descriptor unavailable, card
 * error summary, abnormal interrupt */
#define DWMMC_IDMAC_ERR_FLAGS \
	(DWMMC_IDMAC_INT_FBE | DWMMC_IDMAC_INT_DU | \
	 DWMMC_IDMAC_INT_CES | DWMMC_IDMAC_INT_AI)

/* 4 KiB per descriptor, burst up to 0x1fff. */
#define DWMMC_DMA_DESC_MAX_DATA_LEN	0x1000U
#define DWMMC_DMA_DESC_CNT_MASK		0x1FFFU

/* 32-bit IDMAC descriptor: des0 control word, des1 size, des2 buffer bus
 * address, des3 next-descriptor bus address. */
struct dwc_mmc_idma_desc {
	uint32_t flags;
	uint32_t cnt;
	uint32_t addr;
	uint32_t next_addr;
} __attribute__((packed)) __attribute__((aligned(4)));

#define DWMMC_IDMAC_DES0_DIC		(1u << 1)
#define DWMMC_IDMAC_DES0_LD		(1u << 2)
#define DWMMC_IDMAC_DES0_FD		(1u << 3)
#define DWMMC_IDMAC_DES0_CH		(1u << 4)
#define DWMMC_IDMAC_DES0_ER		(1u << 5)
#define DWMMC_IDMAC_DES0_CES		(1u << 30)
#define DWMMC_IDMAC_DES0_OWN		(1u << 31)

#ifndef GENMASK
#define GENMASK(h, l) (((1u << ((h) - (l) + 1U)) - 1U) << (l))
#endif

/* --- accessors --------------------------------------------------------------- */

#define DWMMC_READ_REG(addr, reg_offset)	reg_rd32((addr) + (uintptr_t)(reg_offset))
#define DWMMC_WRITE_REG(addr, reg_offset, value) \
	reg_wr32((addr) + (uintptr_t)(reg_offset), (uint32_t)(value))

/* CDETECT bit 0: 0 = card present. */
static inline bool dwc_mmc_card_exists(uintptr_t base_addr)
{
	return (0U == (DWMMC_READ_REG(base_addr, DWMMC_CDETECT_OFFSET) & 0x1U));
}

/* Clear all raw interrupt state (write-one-clear) EXCEPT the SDIO card
 * interrupt (bit 16): a pending card interrupt must survive pre-transfer
 * clears, or the DAT1 level would be silently swallowed between frames
 * (M11 r4 interrupt mode). */
#define DWMMC_RINTSTS_CLEAR_ALL	\
	(DWMMC_INTMSK_ALL & ~(uint32_t)DWMMC_INTMSK_SDIO_INT)

static inline void dwc_mmc_clear_interrupt_status(uintptr_t base_addr)
{
	DWMMC_WRITE_REG(base_addr, DWMMC_RINTSTS_OFFSET, DWMMC_RINTSTS_CLEAR_ALL);
	DWMMC_WRITE_REG(base_addr, DWMMC_IDSTS_OFFSET, 0xFFFFFFFFU);
}

#endif /* FREEWEBCAMERA_DWC_MMC_REGS_H */
