/*
 * @file   itsdump.c
 * @brief  Dump the GICv3 ITS delivery state for post-mortem debugging.
 *
 * Ported from the author's embox itsdump (feat-nvme branch, commit
 * 717ba1138d). Every table the dump prints is invalidated in the dcache
 * first: the ITS and the redistributor own their contents and write them
 * over non-coherent ports, so a cached read shows stale data. The same
 * methodology closed the silent-ITS bug (KI-025), and this command exists
 * to keep it one shell command away:
 *
 *   itsdump [devid ...] - register state, the DTE (and ITT entries) of
 *   every attached device, and the prop/pend state of the LPI window.
 *
 * Table geometry (prop/pend addresses, device table) comes from the
 * driver, not from constants here, so the dump cannot drift from what
 * the driver actually programmed.
 */

#include <stdint.h>

#include "board.h"
#include "gicv3_its.h"
#include "regs.h"

/* ITS register offsets (kept local: this file reads, never writes). */
#define GITS_CTLR		0x0000
#define GITS_TYPER		0x0008
#define GITS_CWRITER		0x0088
#define GITS_CREADR		0x0090
#define GITS_BASER(n)		(0x0100 + (n) * 8)

/* Redistributor LPI accounting (RD_base frame offsets). */
#define GICR_PROPBASER		0x0070
#define GICR_PENDBASER		0x0078

/* Parse "0xE001" / "57345"; -1 on garbage. */
static int dump_parse_devid(const char *s)
{
	uint32_t value = 0U;
	unsigned int base = 10U;

	if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
		base = 16U;
		s += 2;
	}
	if (*s == '\0') {
		return -1;
	}
	for (; *s != '\0'; s++) {
		uint32_t digit;

		if ((*s >= '0') && (*s <= '9')) {
			digit = (uint32_t)(*s - '0');
		} else if ((base == 16U) && (*s >= 'a') && (*s <= 'f')) {
			digit = (uint32_t)(*s - 'a') + 10U;
		} else if ((base == 16U) && (*s >= 'A') && (*s <= 'F')) {
			digit = (uint32_t)(*s - 'A') + 10U;
		} else {
			return -1;
		}
		value = (value * base) + digit;
	}
	return (int)value;
}

static void dump_its_regs(void)
{
	its_log("GITS CTLR %08x TYPER %08x%08x\n",
		reg_rd32(BOARD_ITS_BASE + GITS_CTLR),
		(uint32_t)(reg_rd64(BOARD_ITS_BASE + GITS_TYPER) >> 32),
		(uint32_t)reg_rd64(BOARD_ITS_BASE + GITS_TYPER));
	its_log("GITS CWRITER %08x CREADR %08x\n",
		reg_rd32(BOARD_ITS_BASE + GITS_CWRITER),
		reg_rd32(BOARD_ITS_BASE + GITS_CREADR));
	its_log("GITS BASER0 %08x%08x BASER1 %08x%08x\n",
		(uint32_t)(reg_rd64(BOARD_ITS_BASE + GITS_BASER(0)) >> 32),
		(uint32_t)reg_rd64(BOARD_ITS_BASE + GITS_BASER(0)),
		(uint32_t)(reg_rd64(BOARD_ITS_BASE + GITS_BASER(1)) >> 32),
		(uint32_t)reg_rd64(BOARD_ITS_BASE + GITS_BASER(1)));
	its_log("GICR PROPBASE %08x%08x PENDBASE %08x%08x\n",
		(uint32_t)(reg_rd64(BOARD_GICR_BASE + GICR_PROPBASER) >> 32),
		(uint32_t)reg_rd64(BOARD_GICR_BASE + GICR_PROPBASER),
		(uint32_t)(reg_rd64(BOARD_GICR_BASE + GICR_PENDBASER) >> 32),
		(uint32_t)reg_rd64(BOARD_GICR_BASE + GICR_PENDBASER));
}

/* Print the device table entry and, when the driver knows where the
 * device's ITT is, the interrupt translation entries themselves. The
 * DTE/ITE contents are written by the ITS over a non-coherent port, so
 * both are invalidated before the read. */
static void dump_dte(uint32_t devid, uint64_t baser0)
{
	/* The GIC-600 readback reports IMPDEF flags in the upper address
	 * field (bits 51:48 read as 0x7 here); keep the low 28 bits,
	 * plenty for DDR. */
	uint64_t addr = ((baser0 >> 12) & ((1ULL << 28) - 1ULL)) << 12;
	uint64_t *entry = (uint64_t *)(uintptr_t)(addr +
						  ((uint64_t)devid * 8U));
	uint64_t itt = its_device_itt(devid);
	unsigned int i;

	board_dcache_invalidate((uintptr_t)entry, 8U);
	its_log("DTE[%04x] @%llx = %08x%08x\n", devid,
		(unsigned long long)(uintptr_t)entry,
		(uint32_t)(*entry >> 32), (uint32_t)*entry);

	if (itt == 0U) {
		return;
	}
	board_dcache_invalidate(itt, 8U * 8U);
	for (i = 0U; i < 8U; i++) {
		uint64_t ite = *(uint64_t *)(uintptr_t)(itt + (8U * i));

		its_log("ITE[%04x:%u] @%llx = %08x%08x\n", devid, i,
			(unsigned long long)(itt + (8U * i)),
			(uint32_t)(ite >> 32), (uint32_t)ite);
	}
}

/* prop/pend for the whole driver-owned LPI window. Both tables are
 * invalidated before the byte reads: pend is hardware-written by the
 * redistributor, and reading prop through the cache after a dump that
 * ran before some other agent's write would show yesterday's news. */
static void dump_lpi_state(void)
{
	uint64_t prop = its_prop_table();
	uint64_t pend = its_pend_table();
	uint32_t i;

	board_dcache_invalidate((uintptr_t)prop, ITS_LPI_QUANTITY);
	board_dcache_invalidate((uintptr_t)pend, ITS_LPI_QUANTITY / 8U);

	for (i = 0U; i < ITS_LPI_QUANTITY; i++) {
		uint8_t p = *(volatile uint8_t *)(uintptr_t)(prop + i);
		uint8_t pending = *(volatile uint8_t *)
				  (uintptr_t)(pend + (i / 8U));

		if ((p != 0U) ||
		    ((pending & (uint8_t)(1U << (i % 8U))) != 0U)) {
			its_log("LPI %u (intid %u): prop %02x pend %u\n", i,
				IRQ_LPI_INTID_FIRST + i, p,
				(pending >> (i % 8U)) & 1U);
		}
	}
}

int its_dump_cmd(int argc, char **argv)
{
	uint64_t baser0 = reg_rd64(BOARD_ITS_BASE + GITS_BASER(0));
	uint32_t attached[ITS_MAX_DEVICES];
	uint32_t dev_count;
	uint32_t i;
	int a;

	dump_its_regs();

	/* Every attached device, then any extra ids named on the command
	 * line (a DTE the driver never mapped reads as invalid, which is
	 * itself the answer). */
	dev_count = its_attached_devices(attached, ITS_MAX_DEVICES);
	for (i = 0U; i < dev_count; i++) {
		dump_dte(attached[i], baser0);
	}
	for (a = 1; a < argc; a++) {
		int devid = dump_parse_devid(argv[a]);

		if (devid >= 0) {
			dump_dte((uint32_t)devid, baser0);
		} else {
			its_log("itsdump: ignore bad devid '%s'\n", argv[a]);
		}
	}

	dump_lpi_state();

	return 0;
}
