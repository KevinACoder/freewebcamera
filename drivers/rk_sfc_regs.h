/*
 * @file   rk_sfc_regs.h
 * @brief  RK3568 SFC (serial flash controller) register map and platform
 *         coordinates.
 *
 * The SFC is a flash-protocol engine (indirect command mode), not a generic
 * SPI master - there is no LUT on this IP, every operation is described by a
 * single CMD descriptor. Offsets and bit fields verified against TRM Part1
 * Ch27 and the author's working standalone-line port, carried over
 * line-for-line (D20 policy).
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#ifndef FREEWEBCAMERA_RK_SFC_REGS_H
#define FREEWEBCAMERA_RK_SFC_REGS_H

#include <stdint.h>

/* --- register block (base 0xFE300000) -------------------------------------- */

#define SFC_CTRL			0x000U	/* CS0 control */
#define SFC_IMR				0x004U	/* interrupt mask */
#define SFC_ICLR			0x008U	/* interrupt clear (W1C) */
#define SFC_FTLR			0x00CU	/* FIFO thresholds */
#define SFC_RCVR			0x010U	/* recover (self-clearing reset) */
#define SFC_ISR				0x01CU	/* interrupt status */
#define SFC_FSR				0x020U	/* FIFO status */
#define SFC_SR				0x024U	/* status: bit0 busy */
#define SFC_RISR			0x028U	/* raw interrupt status */
#define SFC_VER				0x02CU	/* version (rk3568 resets to 4) */
#define SFC_MODE			0x05CU	/* working mode (reset = indirect) */
#define SFC_DMA_TRIGGER			0x080U
#define SFC_DMA_ADDR			0x084U
#define SFC_LEN_CTRL			0x088U	/* bit0 TRB_SEL (ver >= 4) */
#define SFC_LEN_EXT			0x08CU	/* total transfer bytes */
#define SFC_CMD				0x100U	/* indirect command descriptor */
#define SFC_ADDR			0x104U
#define SFC_DATA			0x108U	/* FIFO window */

/* CTRL: DATB[13:12] ADRB[11:10] CMDB[9:8] IDLE_CYCLE[7:4]
 * SHIFTPHASE[1] SPIM[0]; x1 bus width keeps all width fields at 0. */
#define SFC_CTRL_SHIFTPHASE		(1UL << 1)

#define SFC_RCVR_RESET			0x1U

/* FSR: TX level [13:8], RX level [20:16] (counts of 32-bit FIFO entries) */
#define SFC_FSR_TXLV_MASK		0x3F00U
#define SFC_FSR_TXLV_SHIFT		8U
#define SFC_FSR_RXLV_MASK		0x1F0000U
#define SFC_FSR_RXLV_SHIFT		16U

#define SFC_SR_BUSY			0x1U
#define SFC_LEN_CTRL_TRB_SEL		0x1U
#define SFC_RISR_DMA_DONE		(1UL << 7)

/* CMD descriptor: CS[31:30] ADDRB[15:14] WR[12] DUMMY[11:8] OPCODE[7:0] */
#define SFC_CMD_CS_SHIFT		30U
#define SFC_CMD_ADDR_SHIFT		14U
#define SFC_CMD_ADDR_24BIT		1U
#define SFC_CMD_ADDR_32BIT		2U
#define SFC_CMD_DIR_WR			(1UL << 12)
#define SFC_CMD_DUMMY_SHIFT		8U

/* Single indirect sequence moves at most 16 KiB of data (measured: larger
 * reads silently truncate - 32 KB lost 47%, 64 KB lost 73% of bytes). */
#define SFC_CHUNK_LIMIT			(16U * 1024U)

/* Reads of at least this many bytes go through DMA; the FIFO path is only
 * proven below it (measured on this board: reliable under 64 B). */
#define SFC_DMA_THRESHOLD		0x40U

/* Poll budgets (us), the u-boot-scale numbers the port was debugged with */
#define SFC_RCVR_TIMEOUT_US		1000U
#define SFC_FIFO_TIMEOUT_US		1000U
#define SFC_SR_TIMEOUT_US		100000U
#define SFC_DMA_TIMEOUT_US		1000000U

/* --- platform coordinates (hiword write-enable) ---------------------------- */

#define SFC_CRU_BASE			0xFDD20000UL
#define SFC_CRU_CLKSEL_CON28		0x170U	/* sclk_sfc mux [6:4] */
#define SFC_CLKSEL_MUX_MASK		0x0070U
#define SFC_CLKSEL_MUX_XIN24M		0U	/* 24 MHz, no DLL needed */
#define SFC_CRU_CLKGATE_CON9		0x324U	/* bit2=hclk_sfc bit4=sclk_sfc */
#define SFC_GATE_MASK			0x0014U

/* GRF iomux: CLK=GPIO1_D0, D0=D1, D1=D2, CS0=D3 (fn1), D3-ball=D4 (fn1).
 * GPIO1_C7 (fspi_d2, fn2) is deliberately NOT muxed: on this board the same
 * ball is eMMC rstnout and stays with the eMMC group - x1 reads never touch
 * data lines 2/3 (see dwc_mshc.c for the other side of that trade). */
#define SFC_GRF_BASE			0xFDC60000UL
#define SFC_GRF_GPIO1D_IOMUX_L		0x18U
#define SFC_GRF_GPIO1D_IOMUX_L_MASK	0xFFFFU
#define SFC_GRF_GPIO1D_IOMUX_L_VAL	0x1111U
#define SFC_GRF_GPIO1D_IOMUX_H		0x1CU
#define SFC_GRF_GPIO1D_IOMUX_H_MASK	0x0007U
#define SFC_GRF_GPIO1D_IOMUX_H_VAL	0x0001U

#define SFC_BASE			0xFE300000UL
#define SFC_CS0				0U

/* --- read-only command whitelist -------------------------------------------
 * The flash is the boot medium (miniloader + U-Boot FIT @0x100000). Anything
 * outside this list never reaches the wire; there is no write path at all.
 */
#define SFC_OP_JEDEC_ID			0x9FU
#define SFC_OP_SFDP			0x5AU
#define SFC_OP_READ_STATUS		0x05U
#define SFC_OP_READ_DATA		0x03U

#endif /* FREEWEBCAMERA_RK_SFC_REGS_H */
