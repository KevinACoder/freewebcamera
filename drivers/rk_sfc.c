/*
 * @file   rk_sfc.c
 * @brief  RK3568 SFC controller + SPI-NOR device layer (READ ONLY), behind
 *         the standard CMSIS-Driver ARM_DRIVER_FLASH interface.
 *
 * PROVENANCE AND PORTING POLICY (decision D20, same pattern as dwc_mshc.c).
 * The controller sequences below are the author's own RK3568 port, debugged
 * on this board in the standalone line (2026-08-10) and carried over
 * line-for-line; only the vocabulary changed (regs.h primitives, explicit
 * dcache maintenance, CMSIS-Driver Flash facade). The measured facts that
 * shape the code, each of which cost board time to learn:
 *
 *   - NO RCVR reset and NO MODE write at init: the boot ROM leaves the SFC
 *     with working sampling/timing configuration, and a controller reset
 *     clears it - after that, reads of some addresses return 0xFF forever.
 *     Init is exactly: clocks, iomux, CTRL=0, LEN_CTRL TRB_SEL (ver >= 4).
 *
 *   - SHIFTPHASE=1 (sampling on the negedge): with posedge sampling the
 *     first data word intermittently reads 0x00FFFFFF.
 *
 *   - Register write order per chunk is LEN_EXT -> CTRL -> CMD -> ADDR.
 *     ADDR after CMD, always: the other order decodes as a transaction
 *     error (RISR bit6) and the engine wedges busy.
 *
 *   - One indirect sequence moves at most 16 KiB; larger reads truncate
 *     silently (measured 47%/73% byte loss at 32/64 KiB). Reads are chunked.
 *
 *   - The DMA path (>= 64 B) is the reliable bulk path; the FIFO path is
 *     only proven under 64 B. DMA writes memory behind the cache: the
 *     buffer is invalidated after completion (the identity map makes
 *     VA == PA, which the DMA_ADDR register needs). The first RX word needs
 *     a ~10 us settle after the level report, or partial bytes are read.
 *
 *   - The flash may be left in a continuous-read mode by the boot ROM: the
 *     first JEDEC read then returns garbage (measured 0xE2...). Init reads
 *     JEDEC once and discards it (priming), then reads the real ID.
 *
 * The expected part is the W25Q64DW (Winbond, 8 MiB, JEDEC EF 60 17) - the
 * same ID Linux reports for this flash. The init records whatever ID is
 * actually read and does not gate on it.
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "regs.h"

#include "Driver_Flash.h"
#include "rk_sfc.h"
#include "rk_sfc_regs.h"

#define SFC_DRV_VERSION		ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0)

/* NOR geometry: W25Q64DW, uniform 4 KiB sectors, 256-byte page */
#define SFC_FLASH_SIZE		(8U * 1024U * 1024U)
#define SFC_SECTOR_SIZE		4096U
#define SFC_PAGE_SIZE		256U

struct rk_sfc_dev {
	bool inited;
	bool powered;
	uint8_t jedec[3];
	ARM_Flash_SignalEvent_t cb_event;
};

static struct rk_sfc_dev g_sfc;

/* --- low-level helpers ------------------------------------------------------- */

static void rk_sfc_udelay(uint32_t us)
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

static void rk_sfc_clrset(uintptr_t reg, uint32_t mask, uint32_t val)
{
	reg_wr32(reg, ((mask & 0xFFFFU) << 16) | (val & mask));
}

/* RCVR reset: error-recovery only (never called at init - see file header). */
static int32_t rk_sfc_recover(uintptr_t base)
{
	uint32_t loop;

	reg_wr32(base + SFC_RCVR, SFC_RCVR_RESET);
	for (loop = SFC_RCVR_TIMEOUT_US; loop != 0U; loop--) {
		if ((reg_rd32(base + SFC_RCVR) & SFC_RCVR_RESET) == 0U) {
			reg_wr32(base + SFC_ICLR, 0xFFFFFFFFU);
			return ARM_DRIVER_OK;
		}
		/* tight poll: no delay, the reset self-clears in a few cycles */
	}
	board_log("sfc: controller reset never finished");
	return ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t rk_sfc_wait_idle(uintptr_t base)
{
	uint32_t loop;

	for (loop = SFC_SR_TIMEOUT_US; loop != 0U; loop--) {
		if ((reg_rd32(base + SFC_SR) & SFC_SR_BUSY) == 0U) {
			return ARM_DRIVER_OK;
		}
		rk_sfc_udelay(1);
	}
	board_log("sfc: wait idle timeout");
	return ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t rk_sfc_read_fifo(uintptr_t base, uint8_t *buf, uint32_t len)
{
	uint32_t bytes = len & 0x3U;
	uint32_t dwords = len >> 2;
	uint32_t rx_level;
	uint32_t read_words;
	uint32_t tmp = 0U;
	uint32_t i;
	uint32_t loop;

	if (dwords != 0U) {
		/* the first FIFO entry reports level on its first byte; a
		 * ~10 us settle guarantees the whole word has arrived
		 * (otherwise partial words are read: 0x00FFFFFF) */
		for (loop = SFC_FIFO_TIMEOUT_US; loop != 0U; loop--) {
			if ((reg_rd32(base + SFC_FSR) & SFC_FSR_RXLV_MASK) != 0U) {
				break;
			}
		}
		if (loop == 0U) {
			board_log("sfc: rx fifo timeout");
			return ARM_DRIVER_ERROR_TIMEOUT;
		}
		rk_sfc_udelay(10);
	}

	while (dwords != 0U) {
		for (loop = SFC_FIFO_TIMEOUT_US; loop != 0U; loop--) {
			if ((reg_rd32(base + SFC_FSR) & SFC_FSR_RXLV_MASK) != 0U) {
				break;
			}
		}
		if (loop == 0U) {
			board_log("sfc: rx fifo timeout");
			return ARM_DRIVER_ERROR_TIMEOUT;
		}
		rx_level = (reg_rd32(base + SFC_FSR) & SFC_FSR_RXLV_MASK) >>
			   SFC_FSR_RXLV_SHIFT;
		read_words = (rx_level < dwords) ? rx_level : dwords;
		for (i = 0U; i < read_words; i++) {
			tmp = reg_rd32(base + SFC_DATA);
			memcpy(buf, &tmp, sizeof(tmp));
			buf += 4U;
		}
		dwords -= read_words;
	}

	if (bytes != 0U) {
		for (loop = SFC_FIFO_TIMEOUT_US; loop != 0U; loop--) {
			if ((reg_rd32(base + SFC_FSR) & SFC_FSR_RXLV_MASK) != 0U) {
				break;
			}
		}
		if (loop == 0U) {
			board_log("sfc: rx fifo timeout");
			return ARM_DRIVER_ERROR_TIMEOUT;
		}
		rk_sfc_udelay(10);	/* tail word: same partial-word guard */
		tmp = reg_rd32(base + SFC_DATA);
		memcpy(buf, &tmp, bytes);
	}

	return ARM_DRIVER_OK;
}

/*
 * Execute one read operation (opcodes: the whitelist in rk_sfc_regs.h only).
 * Chunks at 16 KiB; >= 64 B per chunk goes through DMA with a dcache
 * invalidate after completion. There is deliberately no write direction.
 */
static int32_t rk_sfc_exec_read(uint32_t opcode, uint32_t addr_nbytes,
				uint32_t addr, uint32_t dummy_cycles,
				uint8_t *data, uint32_t len)
{
	uintptr_t base = SFC_BASE;
	uint32_t ctrl = SFC_CTRL_SHIFTPHASE;	/* x1 widths, negedge sampling */
	uint32_t cmd;
	uint32_t addr_bits = 0U;
	uint32_t remaining = len;
	uint32_t offset = 0U;

	if (addr_nbytes != 0U && addr_nbytes != 3U && addr_nbytes != 4U) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (addr_nbytes == 3U) {
		addr_bits = SFC_CMD_ADDR_24BIT;
	} else if (addr_nbytes == 4U) {
		addr_bits = SFC_CMD_ADDR_32BIT;
	}

	cmd = (SFC_CS0 << SFC_CMD_CS_SHIFT) |
	      (addr_bits << SFC_CMD_ADDR_SHIFT) |
	      ((dummy_cycles & 0xFU) << SFC_CMD_DUMMY_SHIFT) |
	      (opcode & 0xFFU);

	while (remaining != 0U) {
		uint32_t clen = (remaining > SFC_CHUNK_LIMIT) ?
				SFC_CHUNK_LIMIT : remaining;
		uint32_t dma_len = 0U;
		uint32_t fifo_len;
		int32_t ret;

		/* DMA moves whole words only; the 1-3 byte tail of a chunk runs
		 * as its own FIFO chunk (a non-word-aligned LEN_EXT leaves the
		 * word stream misphased - measured 2026-09-17: a 598 B DMA
		 * read returned 3 leading bytes from before the address). */
		if (clen >= SFC_DMA_THRESHOLD) {
			dma_len = clen & ~0x3U;
			fifo_len = clen - dma_len;
		} else {
			fifo_len = clen;
		}

		/* order is load-bearing: LEN_EXT -> CTRL -> CMD -> ADDR */
		reg_wr32(base + SFC_LEN_EXT, clen);
		reg_wr32(base + SFC_CTRL, ctrl);
		reg_wr32(base + SFC_CMD, cmd);
		if (addr_nbytes != 0U) {
			reg_wr32(base + SFC_ADDR, addr + offset);
		}

		if (dma_len != 0U) {
			uintptr_t dma_buf = (uintptr_t)(void *)(data + offset);
			uint32_t loop;

			/* let the command+address phase fully drain before the
			 * DMA starts sampling data: without this settle the
			 * first word carries phase residue (same hazard the
			 * FIFO path guards with its settle) */
			rk_sfc_udelay(10);

			reg_wr32(base + SFC_ICLR, 0xFFFFFFFFU);
			reg_wr32(base + SFC_DMA_ADDR, (uint32_t)dma_buf);
			reg_wr32(base + SFC_DMA_TRIGGER, 1U);

			for (loop = SFC_DMA_TIMEOUT_US; loop != 0U; loop--) {
				if ((reg_rd32(base + SFC_RISR) &
				     SFC_RISR_DMA_DONE) != 0U) {
					break;
				}
			}
			reg_wr32(base + SFC_ICLR, 0xFFFFFFFFU);
			if (loop == 0U) {
				board_log("sfc: dma timeout");
				return ARM_DRIVER_ERROR_TIMEOUT;
			}
			/* DMA wrote memory behind the cache */
			board_dcache_invalidate(dma_buf, dma_len);

			if (fifo_len != 0U) {
				/* tail bytes: drain the same sequence's FIFO */
				reg_wr32(base + SFC_LEN_EXT, fifo_len);
				reg_wr32(base + SFC_CTRL, ctrl);
				reg_wr32(base + SFC_CMD, cmd);
				reg_wr32(base + SFC_ADDR,
					 addr + offset + dma_len);
				ret = rk_sfc_read_fifo(base, data + offset + dma_len,
						       fifo_len);
				if (ret != ARM_DRIVER_OK) {
					return ret;
				}
			}
		} else {
			ret = rk_sfc_read_fifo(base, data + offset, fifo_len);
			if (ret != ARM_DRIVER_OK) {
				return ret;
			}
		}

		/* the next CMD must not land while the engine is still busy */
		ret = rk_sfc_wait_idle(base);
		if (ret != ARM_DRIVER_OK) {
			(void)rk_sfc_recover(base);
			return ret;
		}

		remaining -= clen;
		offset += clen;
	}

	return rk_sfc_wait_idle(base);
}

/* --- init and NOR probe ------------------------------------------------------- */

static int32_t rk_sfc_hw_init(void)
{
	uintptr_t base = SFC_BASE;
	uintptr_t cru = SFC_CRU_BASE;
	uintptr_t grf = SFC_GRF_BASE;
	uint32_t version;

	/* clocks: sclk_sfc from the 24 MHz crystal (no DLL at this rate),
	 * then open the hclk/sclk gates */
	rk_sfc_clrset(cru + SFC_CRU_CLKSEL_CON28,
		      SFC_CLKSEL_MUX_MASK, SFC_CLKSEL_MUX_XIN24M);
	rk_sfc_clrset(cru + SFC_CRU_CLKGATE_CON9, SFC_GATE_MASK, 0U);

	/* iomux: CLK/D0/D1/CS0/D3-ball. GPIO1_C7 intentionally stays with the
	 * eMMC group (shared ball, see rk_sfc_regs.h). */
	rk_sfc_clrset(grf + SFC_GRF_GPIO1D_IOMUX_L,
		      SFC_GRF_GPIO1D_IOMUX_L_MASK, SFC_GRF_GPIO1D_IOMUX_L_VAL);
	rk_sfc_clrset(grf + SFC_GRF_GPIO1D_IOMUX_H,
		      SFC_GRF_GPIO1D_IOMUX_H_MASK, SFC_GRF_GPIO1D_IOMUX_H_VAL);

	/* no RCVR reset, no MODE write - boot ROM state is load-bearing */
	reg_wr32(base + SFC_CTRL, 0U);
	version = reg_rd32(base + SFC_VER) & 0xFFFFU;
	if (version >= 4U) {
		reg_wr32(base + SFC_LEN_CTRL, SFC_LEN_CTRL_TRB_SEL);
	}

	board_log("sfc: controller ready (ver %u, x1, read-only)", version);
	return ARM_DRIVER_OK;
}

static int32_t rk_sfc_nor_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	return rk_sfc_exec_read(SFC_OP_READ_DATA, 3U, addr, 0U, buf, len);
}

static int32_t rk_sfc_probe(void)
{
	uint8_t scratch[3];

	/* priming read: the boot ROM may have left continuous-read mode on,
	 * which corrupts the first JEDEC transaction (measured 0xE2...) */
	(void)rk_sfc_exec_read(SFC_OP_JEDEC_ID, 0U, 0U, 0U, scratch, sizeof(scratch));

	if (rk_sfc_exec_read(SFC_OP_JEDEC_ID, 0U, 0U, 0U, g_sfc.jedec,
			     sizeof(g_sfc.jedec)) != ARM_DRIVER_OK) {
		return ARM_DRIVER_ERROR;
	}

	board_log("sfc: jedec id %02x %02x %02x (W25Q64DW expects ef 60 17)",
		  g_sfc.jedec[0], g_sfc.jedec[1], g_sfc.jedec[2]);
	return ARM_DRIVER_OK;
}

int rk_sfc_get_jedec(uint8_t id[3])
{
	if (!g_sfc.inited || id == NULL) {
		return -1;
	}
	id[0] = g_sfc.jedec[0];
	id[1] = g_sfc.jedec[1];
	id[2] = g_sfc.jedec[2];
	return 0;
}

int rk_sfc_read_sfdp(uint32_t addr, void *buf, uint32_t len)
{
	if (!g_sfc.inited || buf == NULL) {
		return -1;
	}
	return (rk_sfc_exec_read(SFC_OP_SFDP, 3U, addr, 8U,
				 (uint8_t *)buf, len) == ARM_DRIVER_OK) ? 0 : -1;
}

static int32_t rk_sfc_initialize(ARM_Flash_SignalEvent_t cb_event)
{
	int32_t ret;

	g_sfc.cb_event = cb_event;	/* polling: never signalled */
	if (g_sfc.inited) {
		return ARM_DRIVER_OK;
	}

	ret = rk_sfc_hw_init();
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}
	ret = rk_sfc_probe();
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}
	g_sfc.inited = true;
	g_sfc.powered = true;
	return ARM_DRIVER_OK;
}

/* --- CMSIS-Driver ARM_DRIVER_FLASH facade (read-only) ------------------------ */

static ARM_DRIVER_VERSION rk_sfc_get_version(void)
{
	return (ARM_DRIVER_VERSION){ ARM_FLASH_API_VERSION, SFC_DRV_VERSION };
}

static ARM_FLASH_CAPABILITIES rk_sfc_get_capabilities(void)
{
	/* no erase_chip, no events, 8-bit data */
	return (ARM_FLASH_CAPABILITIES){ 0 };
}

static int32_t rk_sfc_uninitialize(void)
{
	/* the boot medium cannot actually be powered down; drop the flag only */
	g_sfc.powered = false;
	return ARM_DRIVER_OK;
}

static int32_t rk_sfc_power_control(ARM_POWER_STATE state)
{
	switch (state) {
	case ARM_POWER_FULL:
		return rk_sfc_initialize(NULL);
	case ARM_POWER_OFF:
	case ARM_POWER_LOW:
	default:
		return ARM_DRIVER_OK;	/* boot medium: nothing to turn off */
	}
}

static int32_t rk_sfc_read_data(uint32_t addr, void *data, uint32_t cnt)
{
	if (!g_sfc.inited) {
		return ARM_DRIVER_ERROR;
	}
	if (data == NULL || cnt == 0U) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (addr >= SFC_FLASH_SIZE || (SFC_FLASH_SIZE - addr) < cnt) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (rk_sfc_nor_read(addr, (uint8_t *)data, cnt) != ARM_DRIVER_OK) {
		return ARM_DRIVER_ERROR;
	}
	return (int32_t)cnt;
}

static int32_t rk_sfc_program_data(uint32_t addr, const void *data, uint32_t cnt)
{
	(void)addr; (void)data; (void)cnt;
	/* Hard gate: this flash is the boot medium. There is no write path
	 * behind this return, and the NOR opcode whitelist has no program
	 * command either (decision D31). */
	board_log("sfc: program refused (driver is read-only by design)");
	return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t rk_sfc_erase_sector(uint32_t addr)
{
	(void)addr;
	board_log("sfc: erase refused (driver is read-only by design)");
	return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t rk_sfc_erase_chip(void)
{
	board_log("sfc: erase refused (driver is read-only by design)");
	return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static ARM_FLASH_STATUS rk_sfc_get_status(void)
{
	ARM_FLASH_STATUS st = { 0 };

	return st;	/* polling driver: never busy between calls */
}

static ARM_FLASH_INFO sfc_flash_info = {
	.sector_info = NULL,		/* uniform sectors */
	.sector_count = SFC_FLASH_SIZE / SFC_SECTOR_SIZE,
	.sector_size = SFC_SECTOR_SIZE,
	.page_size = SFC_PAGE_SIZE,
	.program_unit = 1U,
	.erased_value = 0xFFU,
};

static ARM_FLASH_INFO *rk_sfc_get_info(void)
{
	return &sfc_flash_info;
}

ARM_DRIVER_FLASH Driver_Flash0 = {
	rk_sfc_get_version,
	rk_sfc_get_capabilities,
	rk_sfc_initialize,
	rk_sfc_uninitialize,
	rk_sfc_power_control,
	rk_sfc_read_data,
	rk_sfc_program_data,
	rk_sfc_erase_sector,
	rk_sfc_erase_chip,
	rk_sfc_get_status,
	rk_sfc_get_info,
};
