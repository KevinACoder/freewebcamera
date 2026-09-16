/*
 * @file   dwc_ahci.c
 * @brief  DesignWare AHCI driver for the on-chip SATA ports.
 *
 * See drivers/dwc_ahci.h for the provenance. The shape of the driver is the
 * embox original's, kept deliberately: a port comes under control by stopping
 * its engine, pointing it at this driver's command list and received-FIS area,
 * and starting it again; commands run one at a time through slot 0 in polled
 * mode; every transfer is staged through a page-aligned bounce buffer, which
 * also keeps DMA off caller buffers of any alignment.
 *
 * Four things here are load-bearing:
 *
 *   - The PHY and the link belong to the firmware. U-Boot's preboot runs
 *     `scsi scan`, which powers the combo PHYs and locks their PLLs; a cold
 *     PHY bring-up from software was measured to fail on this board. The
 *     driver therefore only takes over the command engine of a link whose
 *     PxSSTS.DET already says "PHY ready", and never touches PHY, CRU or PMU.
 *   - DMA coherency is by hand. The HBA fetches the command list, FIS and PRD
 *     over a non-coherent port and writes data the same way, so the driver
 *     flushes before the device may read memory and invalidates before reading
 *     what the device wrote. The command table flush covers the PRD at +0x80,
 *     not just the FIS: a shorter flush leaves the device fetching a stale
 *     transfer address.
 *   - PxIS and PxSERR are write-1-to-clear, and PxCMD is read-modify-written:
 *     a plain store of the command register would clear the power/spin-up bits
 *     the firmware set.
 *   - The 32-bit DMA address is the virtual address. The kernel is identity
 *     mapped and the HBA has a 32-bit address space; any buffer outside the
 *     low 4 GiB would silently DMA elsewhere.
 *
 * Two additions to the ported behaviour, both marked at their site:
 *   - Flush() issues ATA FLUSH CACHE EXT, because the frozen ahci.h interface
 *     has a Flush and the block layer above it (FatFs) asks for one; the embox
 *     original had no flush at all.
 *   - GetCapabilities() reports the link generation read out of PxSSTS, which
 *     the frozen interface asks for and the original never needed.
 *
 * And one thing dropped: the embox driver's private ioctl that reported the
 * medium size in bytes (its block layer's own size ioctl answered with an int,
 * which a 119 GB disk does not fit). include/blkdev.h reports a 64-bit sector
 * count in its capabilities instead, so there is nothing to work around.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "Driver_Common.h"
#include "ahci.h"
#include "board.h"
#include "regs.h"

#include "dwc_ahci.h"

/* HBA memory registers (AHCI 1.3.1) */
#define HBA_CAP 0x00
#define HBA_PI	0x0c
#define HBA_VS	0x10

/* Port register block: HBA base + 0x100 + port * 0x80 */
#define PORT_REG_BASE	0x100
#define PORT_REG_STRIDE	0x80

#define PX_CLB	0x00
#define PX_CLBU	0x04
#define PX_FB	0x08
#define PX_FBU	0x0c
#define PX_IS	0x10
#define PX_CMD	0x18
#define PX_TFD	0x20
#define PX_SIG	0x24
#define PX_SSTS	0x28
#define PX_SERR	0x30
#define PX_CI	0x38

#define PXCMD_ST	(1u << 0)
#define PXCMD_SUD	(1u << 1)
#define PXCMD_POD	(1u << 2)
#define PXCMD_FRE	(1u << 4)
#define PXCMD_FR	(1u << 14)
#define PXCMD_CR	(1u << 15)
#define PXCMD_ICC_MASK	(0xfu << 28)
#define PXCMD_ICC_ACTIVE (1u << 28)

#define PXIS_TFES (1u << 30)

#define PXTFD_ERR (1u << 0)
#define PXTFD_DRQ (1u << 3)
#define PXTFD_BSY (1u << 7)

#define SSTS_DET_MASK	0xf
#define SSTS_DET_PHYRDY 3
#define SSTS_SPEED_SHIFT 4

#define PXSIG_ATA 0x0101

/* ATA commands and registers (ACS) */
#define ATA_CMD_IDENTIFY	0xec
#define ATA_CMD_READ_DMA_EXT	0x25
#define ATA_CMD_WRITE_DMA_EXT	0x35
#define ATA_CMD_FLUSH_CACHE_EXT	0xea

#define ATA_DEV_LBA 0x40

#define FIS_TYPE_H2D 0x27

#define AHCI_SECTOR_SIZE 512

/* The command list is allocated for all 32 slots even though only slot 0 is
 * used: the HBA requires the list to be 1 KiB aligned. */
#define AHCI_CLB_SIZE	     (32 * 32)
#define AHCI_FB_SIZE	     256
#define AHCI_CMD_TABLE_SIZE  256
#define AHCI_IDENT_SIZE	     AHCI_SECTOR_SIZE

/* The command table holds the command FIS first and the PRD table at offset
 * 0x80 (AHCI 1.3.1). */
#define AHCI_CMD_TABLE_PRDT 0x80

/* One PRD entry describes the whole payload of a command. */
#define AHCI_CHUNK_SIZE 0x10000

/* Bounded busy waits, the pattern the other on-board drivers use: a port that
 * does not answer must report instead of hanging the boot. The command budget
 * covers the worst case where the attached SSD stalls to reclaim flash. */
#define AHCI_REG_TRIES 100000u
#define AHCI_CMD_TRIES 20000000u

struct ahci_port {
	uintptr_t regs;		/* port register block */
	uint64_t nblocks;	/* device size in logical sectors */
	uint32_t sector_size;	/* logical sector size, the ATA address unit */
	uint8_t link_speed;	/* PxSSTS speed: 1 = Gen1, 2 = Gen2, 3 = Gen3 */
	uint8_t present;	/* link up and a device identified */
	uint8_t *clb;
	uint8_t *fb;
	uint8_t *cmdtab;
	uint8_t *xfer;
};

static struct ahci_port ahci_ports[DWC_AHCI_PORT_MAX];
static uint32_t ahci_port_count;
static int ahci_probed;

/* DMA areas: the list and the received-FIS area must be aligned to their own
 * size, and the bounce buffers are page aligned so a transfer never shares a
 * cache line with another structure. */
static uint8_t ahci_clb[DWC_AHCI_PORT_MAX][AHCI_CLB_SIZE]
	__attribute__((aligned(1024)));
static uint8_t ahci_fb[DWC_AHCI_PORT_MAX][AHCI_FB_SIZE]
	__attribute__((aligned(256)));
static uint8_t ahci_cmdtab[DWC_AHCI_PORT_MAX][AHCI_CMD_TABLE_SIZE]
	__attribute__((aligned(256)));
static uint8_t ahci_xfer[DWC_AHCI_PORT_MAX][AHCI_CHUNK_SIZE]
	__attribute__((aligned(4096)));
static uint8_t ahci_ident[AHCI_IDENT_SIZE] __attribute__((aligned(4096)));

/* The kernel runs identity mapped and the HBA has a 32-bit address space, so a
 * DMA address is the virtual address itself. */
static uint32_t ahci_dma_addr(const void *va)
{
	return (uint32_t)(uintptr_t)va;
}

static inline uintptr_t ahci_port_reg(const struct ahci_port *ap, uint32_t off)
{
	return ap->regs + off;
}

/* PxIS and PxSERR are write-1-to-clear. */
static void ahci_port_clear_irq(struct ahci_port *ap)
{
	reg_wr32(ahci_port_reg(ap, PX_IS), ~0u);
	reg_wr32(ahci_port_reg(ap, PX_SERR), ~0u);
}

/* Quiesce the command engine and wait until the HBA acknowledges with CR and
 * FR clear. */
static void ahci_port_stop(struct ahci_port *ap)
{
	uint32_t cmd;
	uint32_t tries;

	cmd = reg_rd32(ahci_port_reg(ap, PX_CMD));
	reg_wr32(ahci_port_reg(ap, PX_CMD), cmd & ~(PXCMD_ST | PXCMD_FRE));

	for (tries = 0; tries < AHCI_REG_TRIES; tries++) {
		cmd = reg_rd32(ahci_port_reg(ap, PX_CMD));
		if ((cmd & (PXCMD_FR | PXCMD_CR)) == 0) {
			return;
		}
	}

	board_log("ahci: port engine did not stop (PxCMD 0x%08x)\n",
		  reg_rd32(ahci_port_reg(ap, PX_CMD)));
}

/* Point the engine at this driver's structures and start it. The power and
 * spin-up bits keep the values the firmware gave them, so the command register
 * is updated, not overwritten. */
static void ahci_port_start(struct ahci_port *ap)
{
	uint32_t cmd;
	uint32_t tries;

	ahci_port_stop(ap);
	ahci_port_clear_irq(ap);

	reg_wr32(ahci_port_reg(ap, PX_CLB), ahci_dma_addr(ap->clb));
	reg_wr32(ahci_port_reg(ap, PX_CLBU), 0);
	reg_wr32(ahci_port_reg(ap, PX_FB), ahci_dma_addr(ap->fb));
	reg_wr32(ahci_port_reg(ap, PX_FBU), 0);

	cmd = reg_rd32(ahci_port_reg(ap, PX_CMD));
	cmd = (cmd & ~PXCMD_ICC_MASK) | PXCMD_ICC_ACTIVE | PXCMD_POD | PXCMD_SUD |
	      PXCMD_FRE | PXCMD_ST;
	reg_wr32(ahci_port_reg(ap, PX_CMD), cmd);
	reg_dsb();

	/* Let the engine pick up the list and FIS pointers before the first
	 * command is posted. */
	for (tries = 0; tries < AHCI_REG_TRIES; tries++) {
		if (reg_rd32(ahci_port_reg(ap, PX_CMD)) & PXCMD_CR) {
			return;
		}
	}

	board_log("ahci: port engine did not start (PxCMD 0x%08x)\n",
		  reg_rd32(ahci_port_reg(ap, PX_CMD)));
}

/* The device must have finished the previous command before the next one is
 * posted into the slot. */
static int32_t ahci_port_ready(struct ahci_port *ap)
{
	uint32_t tries;

	for (tries = 0; tries < AHCI_REG_TRIES; tries++) {
		uint32_t tfd = reg_rd32(ahci_port_reg(ap, PX_TFD));

		if ((tfd & (PXTFD_BSY | PXTFD_DRQ)) == 0 &&
		    (reg_rd32(ahci_port_reg(ap, PX_CI)) & 1u) == 0) {
			return ARM_DRIVER_OK;
		}
	}

	board_log("ahci: port busy before command (PxTFD 0x%08x PxCI 0x%08x)\n",
		  reg_rd32(ahci_port_reg(ap, PX_TFD)),
		  reg_rd32(ahci_port_reg(ap, PX_CI)));
	return ARM_DRIVER_ERROR_BUSY;
}

/*
 * Run one ATA command through slot 0:
 *   ata_cmd   ATA command code
 *   dev_reg   device register value (ATA_DEV_LBA for the 48-bit LBA ops)
 *   is_write  1: host-to-device DMA, 0: device-to-host DMA
 *   buf       data buffer, len bytes, at most AHCI_CHUNK_SIZE, NULL for a
 *             command with no data transfer (flush)
 *   len       payload size in bytes
 *   nsect     ATA sector count field (in device sectors)
 *   lba       starting 48-bit LBA
 */
static int32_t ahci_port_command(struct ahci_port *ap, uint8_t ata_cmd,
				 uint8_t dev_reg, int is_write, void *buf,
				 size_t len, uint16_t nsect, uint64_t lba)
{
	uint8_t *fis = ap->cmdtab;
	uint32_t *prdt = (uint32_t *)(ap->cmdtab + AHCI_CMD_TABLE_PRDT);
	uint32_t *cl = (uint32_t *)ap->clb;
	uintptr_t pxci = ahci_port_reg(ap, PX_CI);
	uintptr_t pxis = ahci_port_reg(ap, PX_IS);
	uint32_t prdtl = (len != 0) ? 1u : 0u;
	uint32_t tries;
	int32_t ret;

	if (ahci_port_ready(ap) != ARM_DRIVER_OK) {
		return ARM_DRIVER_ERROR_BUSY;
	}

	memset(ap->cmdtab, 0, AHCI_CMD_TABLE_SIZE);

	/* Host-to-device register FIS (type 0x27); the C bit makes the device
	 * latch the command register. */
	fis[0] = FIS_TYPE_H2D;
	fis[1] = 1 << 7;
	fis[2] = ata_cmd;
	fis[4] = (uint8_t)lba;
	fis[5] = (uint8_t)(lba >> 8);
	fis[6] = (uint8_t)(lba >> 16);
	fis[7] = dev_reg;
	fis[8] = (uint8_t)(lba >> 24);
	fis[9] = (uint8_t)(lba >> 32);
	fis[10] = (uint8_t)(lba >> 40);
	fis[12] = (uint8_t)nsect;
	fis[13] = (uint8_t)(nsect >> 8);

	/* One PRD covering the whole payload: DBC holds the byte count minus
	 * one in bits 21:0, bit 31 asks for an interrupt on completion (the
	 * interrupt itself stays masked at the HBA). A command with no data
	 * (flush) carries no PRD at all. */
	if (prdtl != 0) {
		prdt[0] = ahci_dma_addr(buf);
		prdt[1] = 0;
		prdt[2] = 0;
		prdt[3] = ((uint32_t)len - 1) | (1u << 31);
	}

	/* Command header: 5-dword command FIS, one PRD (or none), and the W
	 * bit selects the direction of a DMA command. */
	cl[0] = 5 | (prdtl << 16) | (is_write ? (1u << 6) : 0);
	cl[1] = 0;
	cl[2] = ahci_dma_addr(ap->cmdtab);
	cl[3] = 0;

	board_dcache_flush((uintptr_t)ap->cmdtab, AHCI_CMD_TABLE_SIZE);
	board_dcache_flush((uintptr_t)ap->clb, AHCI_CLB_SIZE);
	if (prdtl != 0 && buf != NULL) {
		if (is_write) {
			board_dcache_flush((uintptr_t)buf, len);
		} else {
			/* The device owns the buffer until the command
			 * completes: drop the stale lines before it DMAs and
			 * take them out again before the data is read. */
			board_dcache_invalidate((uintptr_t)buf, len);
		}
	}
	reg_dsb();

	ahci_port_clear_irq(ap);

	/* Issue slot 0 and wait for the HBA to release it. */
	reg_wr32(pxci, 1u);
	for (tries = 0; tries < AHCI_CMD_TRIES; tries++) {
		if ((reg_rd32(pxci) & 1u) == 0) {
			break;
		}
		if (reg_rd32(pxis) & PXIS_TFES) {
			break;
		}
	}

	reg_dsb();

	if (reg_rd32(pxis) & PXIS_TFES) {
		board_log("ahci: command %#x failed (PxTFD 0x%08x PxSERR 0x%08x)\n",
			  ata_cmd, reg_rd32(ahci_port_reg(ap, PX_TFD)),
			  reg_rd32(ahci_port_reg(ap, PX_SERR)));
		ret = ARM_DRIVER_ERROR;
	} else if (tries == AHCI_CMD_TRIES) {
		board_log("ahci: command %#x timed out (PxCI 0x%08x PxTFD 0x%08x)\n",
			  ata_cmd, reg_rd32(pxci),
			  reg_rd32(ahci_port_reg(ap, PX_TFD)));
		ret = ARM_DRIVER_ERROR_TIMEOUT;
	} else if (reg_rd32(ahci_port_reg(ap, PX_TFD)) & PXTFD_ERR) {
		board_log("ahci: command %#x error (PxTFD 0x%08x PxSERR 0x%08x)\n",
			  ata_cmd, reg_rd32(ahci_port_reg(ap, PX_TFD)),
			  reg_rd32(ahci_port_reg(ap, PX_SERR)));
		ret = ARM_DRIVER_ERROR;
	} else {
		ret = ARM_DRIVER_OK;
	}

	ahci_port_clear_irq(ap);

	if (ret == ARM_DRIVER_OK && prdtl != 0 && buf != NULL && !is_write) {
		board_dcache_invalidate((uintptr_t)buf, len);
	}

	return ret;
}

/* Issue IDENTIFY DEVICE and parse the geometry out of the response. */
static int32_t ahci_port_identify(struct ahci_port *ap)
{
	const uint16_t *w = (const uint16_t *)ahci_ident;
	uint64_t nblocks;
	uint32_t blk_size;
	uint32_t sig;
	int32_t ret;

	/* A non-ATA signature means an ATAPI device or a port multiplier.
	 * Zero just means the HBA has not latched one, in which case the
	 * IDENTIFY below is the better probe. */
	sig = reg_rd32(ahci_port_reg(ap, PX_SIG)) & 0xffff;
	if (sig != 0 && sig != PXSIG_ATA) {
		board_log("ahci: port signature 0x%08x is not an ATA device\n", sig);
		return ARM_DRIVER_ERROR;
	}

	ret = ahci_port_command(ap, ATA_CMD_IDENTIFY, 0, 0, ahci_ident,
				AHCI_IDENT_SIZE, 1, 0);
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}

	/* Prefer the 48-bit LBA count, fall back to the 28-bit one. */
	nblocks = 0;
	if ((w[83] & (1u << 10)) && (w[86] & (1u << 10))) {
		nblocks = (uint64_t)w[100] | ((uint64_t)w[101] << 16) |
			  ((uint64_t)w[102] << 32) | ((uint64_t)w[103] << 48);
	}
	if (nblocks == 0) {
		nblocks = (uint64_t)w[60] | ((uint64_t)w[61] << 16);
	}
	if (nblocks == 0) {
		board_log("ahci: device reports a zero sector count\n");
		return ARM_DRIVER_ERROR;
	}

	/* 512 bytes per sector by default; words 117/118 carry the size when
	 * word 106 reports a larger logical sector. */
	blk_size = AHCI_SECTOR_SIZE;
	if (w[106] & (1u << 12)) {
		blk_size = (uint32_t)w[117] | ((uint32_t)w[118] << 16);
	}

	ap->nblocks = nblocks;
	ap->sector_size = blk_size;

	return ARM_DRIVER_OK;
}

/* One READ/WRITE DMA EXT command, at most AHCI_CHUNK_SIZE bytes. */
static int32_t ahci_transfer(struct ahci_port *ap, int is_write, void *buf,
			     size_t len, uint64_t lba)
{
	uint16_t nsect;

	if (len == 0 || len > AHCI_CHUNK_SIZE || (len % ap->sector_size) != 0) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	nsect = (uint16_t)(len / ap->sector_size);

	return ahci_port_command(ap, is_write ? ATA_CMD_WRITE_DMA_EXT
					      : ATA_CMD_READ_DMA_EXT,
				 ATA_DEV_LBA, is_write, buf, len, nsect, lba);
}

/* Copy a block range in or out through the bounce buffer, one command-sized
 * chunk at a time: the HBA and the caller never share a cache line, and the
 * 64 KiB chunk is what one PRD entry can describe. */
static int32_t ahci_rw(struct ahci_port *ap, int is_write, uint64_t lba,
		       void *data, uint32_t bytes)
{
	uint8_t *buf = (uint8_t *)data;
	uint32_t done = 0;

	if (ap->present == 0) {
		return ARM_DRIVER_ERROR;
	}
	if (data == NULL || bytes == 0 ||
	    (bytes % ap->sector_size) != 0 || lba >= ap->nblocks) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	while (done < bytes) {
		uint32_t chunk = bytes - done;
		int32_t ret;

		if (chunk > AHCI_CHUNK_SIZE) {
			chunk = AHCI_CHUNK_SIZE;
		}

		if (is_write) {
			memcpy(ap->xfer, buf + done, chunk);
			ret = ahci_transfer(ap, 1, ap->xfer, chunk,
					    lba + done / ap->sector_size);
		} else {
			ret = ahci_transfer(ap, 0, ap->xfer, chunk,
					    lba + done / ap->sector_size);
			if (ret == ARM_DRIVER_OK) {
				memcpy(buf + done, ap->xfer, chunk);
			}
		}
		if (ret != ARM_DRIVER_OK) {
			return ret;
		}
		done += chunk;
	}

	return ARM_DRIVER_OK;
}

/* ATA FLUSH CACHE EXT: a non-data command, so it carries no PRD. Added for the
 * frozen interface's Flush; the embox original had none. */
static int32_t ahci_port_flush(struct ahci_port *ap)
{
	return ahci_port_command(ap, ATA_CMD_FLUSH_CACHE_EXT, ATA_DEV_LBA, 0, NULL,
				 0, 0, 0);
}

/* --- probe ---------------------------------------------------------------- */

/* Common per-port bring-up: take the engine over and read the geometry. */
static int32_t ahci_port_probe(struct ahci_port *ap)
{
	uint32_t ssts;

	ssts = reg_rd32(ahci_port_reg(ap, PX_SSTS));
	if ((ssts & SSTS_DET_MASK) != SSTS_DET_PHYRDY) {
		/* The link is the firmware's work; a port whose link never
		 * trained (no device attached, unwired controller) is left
		 * alone. */
		return ARM_DRIVER_ERROR;
	}

	memset(ap->clb, 0, AHCI_CLB_SIZE);
	memset(ap->fb, 0, AHCI_FB_SIZE);
	memset(ap->cmdtab, 0, AHCI_CMD_TABLE_SIZE);

	ahci_port_start(ap);

	if (ahci_port_identify(ap) != ARM_DRIVER_OK) {
		return ARM_DRIVER_ERROR;
	}

	ap->link_speed = (uint8_t)((ssts >> SSTS_SPEED_SHIFT) & 0xfu);
	ap->present = 1;

	return ARM_DRIVER_OK;
}

void dwc_ahci_init(void)
{
	uint32_t ctrl;
	uint32_t port;

	if (ahci_probed) {
		return;
	}
	ahci_probed = 1;

	for (ctrl = 0; ctrl < dwc_ahci_plat.ctrl_count; ctrl++) {
		uintptr_t base = dwc_ahci_plat.base_addr +
				 (uintptr_t)ctrl * dwc_ahci_plat.stride;
		uint32_t cap = reg_rd32(base + HBA_CAP);
		uint32_t vs = reg_rd32(base + HBA_VS);
		uint32_t pi = reg_rd32(base + HBA_PI);

		board_log("ahci%u: HBA at 0x%lx, version %u.%u%u, %u slot(s),"
			  " ports 0x%x\n",
			  ctrl, (unsigned long)base, (vs >> 16) & 0xffffu,
			  (vs >> 8) & 0xffu, vs & 0xffu,
			  ((cap >> 8) & 0x1fu) + 1u, pi);

		for (port = 0; port < dwc_ahci_plat.port_count; port++) {
			uint32_t idx = ctrl * dwc_ahci_plat.port_count + port;
			struct ahci_port *ap;

			if ((pi & (1u << port)) == 0) {
				continue;
			}
			if (idx >= DWC_AHCI_PORT_MAX) {
				board_log("ahci: port table full, ctrl %u port %u"
					  " ignored\n", ctrl, port);
				break;
			}

			ap = &ahci_ports[idx];
			memset(ap, 0, sizeof(*ap));
			ap->regs = base + PORT_REG_BASE + port * PORT_REG_STRIDE;
			ap->clb = ahci_clb[idx];
			ap->fb = ahci_fb[idx];
			ap->cmdtab = ahci_cmdtab[idx];
			ap->xfer = ahci_xfer[idx];

			if (ahci_port_probe(ap) != ARM_DRIVER_OK) {
				board_log("ahci%u: port %u has no usable ATA"
					  " device (SSTS 0x%08x), skipping\n",
					  ctrl, port,
					  reg_rd32(ahci_port_reg(ap, PX_SSTS)));
				continue;
			}

			board_log("ahci%u: port %u, %llu sectors of %u bytes"
				  " (%llu MiB), Gen%u\n",
				  ctrl, port, (unsigned long long)ap->nblocks,
				  ap->sector_size,
				  (unsigned long long)(ap->nblocks *
						       ap->sector_size >> 20),
				  ap->link_speed);
			ahci_port_count++;
		}
	}

	if (ahci_port_count == 0) {
		board_log("ahci: no SATA port came up\n");
	}
}

uint32_t dwc_ahci_port_count(void)
{
	return ahci_port_count;
}

/* --- CMSIS ARM_DRIVER_AHCI (include/ahci.h) -------------------------------- */

/* The frozen interface numbers ports, not controllers: index n is the n-th
 * port that exists on the board (0 and 1 here, one per controller). */
static struct ahci_port *port_of(uint32_t port)
{
	if (port >= DWC_AHCI_PORT_MAX || !ahci_ports[port].present) {
		return NULL;
	}

	return &ahci_ports[port];
}

static ARM_DRIVER_VERSION ahci_get_version(void)
{
	return (ARM_DRIVER_VERSION){ .api = AHCI_API_VERSION, .drv = 0x0100 };
}

static AHCI_CAPABILITIES ahci_get_capabilities(uint32_t port)
{
	AHCI_CAPABILITIES caps;
	struct ahci_port *ap = port_of(port);

	memset(&caps, 0, sizeof(caps));
	if (ap == NULL) {
		return caps;
	}

	caps.sector_count = ap->nblocks;
	caps.sector_size = ap->sector_size;
	caps.max_transfer = AHCI_CHUNK_SIZE;
	caps.link_speed = ap->link_speed;
	caps.port_count = (uint8_t)dwc_ahci_plat.port_count;
	caps.supports_ncq = 0;	/* slot 0 only, one command in flight */

	return caps;
}

static int32_t ahci_initialize(uint32_t port, AHCI_SignalEvent_t cb_event)
{
	/* The event callback belongs to the asynchronous model the frozen
	 * header describes; this implementation completes synchronously in the
	 * caller's context and never raises an event. Accepting the callback
	 * keeps the call shape honest for a future interrupt-driven variant. */
	(void)cb_event;

	dwc_ahci_init();

	return port_of(port) != NULL ? ARM_DRIVER_OK : ARM_DRIVER_ERROR;
}

static int32_t ahci_uninitialize(uint32_t port)
{
	if (port >= DWC_AHCI_PORT_MAX) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	/* The engine is left running: firmware owned the link and the port is
	 * usable as-is. */
	return ARM_DRIVER_OK;
}

static int32_t ahci_power_control(uint32_t port, uint32_t state)
{
	if (port >= DWC_AHCI_PORT_MAX) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (state != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}

	return ARM_DRIVER_OK;
}

static int32_t ahci_port_start_op(uint32_t port)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL) {
		return ARM_DRIVER_ERROR;
	}

	return ahci_port_probe(ap);
}

static int32_t ahci_get_signature(uint32_t port, uint32_t *signature)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL || signature == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	*signature = reg_rd32(ahci_port_reg(ap, PX_SIG)) & 0xffffu;

	return ARM_DRIVER_OK;
}

static int32_t ahci_identify(uint32_t port, void *buffer)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL || buffer == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	/* Into the driver's own buffer first, then out: the response is DMA'd
	 * and the caller's buffer has no alignment contract. */
	if (ahci_port_command(ap, ATA_CMD_IDENTIFY, 0, 0, ahci_ident,
			      AHCI_IDENT_SIZE, 1, 0) != ARM_DRIVER_OK) {
		return ARM_DRIVER_ERROR;
	}
	memcpy(buffer, ahci_ident, AHCI_IDENT_SIZE);

	return ARM_DRIVER_OK;
}

static int32_t ahci_read_blocks(uint32_t port, uint64_t lba, void *data,
				uint32_t block_count)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL) {
		return ARM_DRIVER_ERROR;
	}

	return ahci_rw(ap, 0, lba, data, block_count * ap->sector_size);
}

static int32_t ahci_write_blocks(uint32_t port, uint64_t lba, const void *data,
				 uint32_t block_count)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL) {
		return ARM_DRIVER_ERROR;
	}

	return ahci_rw(ap, 1, lba, (void *)data, block_count * ap->sector_size);
}

static int32_t ahci_flush(uint32_t port)
{
	struct ahci_port *ap = port_of(port);

	if (ap == NULL) {
		return ARM_DRIVER_ERROR;
	}

	return ahci_port_flush(ap);
}

static int32_t ahci_control(uint32_t port, uint32_t control, uint32_t arg)
{
	(void)control;
	(void)arg;

	if (port >= DWC_AHCI_PORT_MAX) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return ARM_DRIVER_ERROR_UNSUPPORTED;
}

ARM_DRIVER_AHCI Driver_AHCI = {
	ahci_get_version,
	ahci_get_capabilities,
	ahci_initialize,
	ahci_uninitialize,
	ahci_power_control,
	ahci_port_start_op,
	ahci_get_signature,
	ahci_identify,
	ahci_read_blocks,
	ahci_write_blocks,
	ahci_flush,
	ahci_control,
};