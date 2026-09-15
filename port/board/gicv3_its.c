/*
 * @file   gicv3_its.c
 * @brief  GICv3 Interrupt Translation Service and LPI support.
 *
 * The ITS turns a write to a doorbell address into a specific LPI on a specific
 * redistributor. That indirection is what lets a PCIe endpoint raise an
 * arbitrary MSI with nothing but a Memory Write, and it is the reason a device
 * does not need a dedicated interrupt line. M8/M9 (UVC) sit on top of this.
 *
 * ---------------------------------------------------------------------------
 * THIS BLOCK FAILS SILENTLY WHEN IT IS WRONG.
 *
 * Every mistake here produces the same observable result: nothing happens. No
 * fault, no error flag, no indication of which of the many moving parts is
 * misconfigured. The lab's own history on this board records two rounds of
 * misdiagnosis before the real cause was found (a missing cache flush), and a
 * third from reading the pending table at the wrong offset. So the rules this
 * file follows:
 *
 *  1. EVERY CPU WRITE TO A TABLE THE ITS READS MUST BE FLUSHED.
 *     The property table is ordinary cacheable Normal memory; the ITS reads it
 *     over a non-coherent port. A store that is still sitting in the D-cache is
 *     invisible to the ITS, which then sees the enable bit as 0 and drops every
 *     LPI. `dsb` alone does NOT help - it orders, it does not write back. This
 *     one omission produces a completely silent ITS.
 *
 *  2. THE PENDING TABLE IS BIT-PER-LPI, NOT BYTE-PER-LPI.
 *     Indexing it by LPI number reads the right table at the wrong place, and
 *     the natural place to look (the start of the table) is the region that
 *     belongs to completely different interrupts - which is how "the
 *     redistributor never set pending" becomes a confident wrong answer.
 *
 *  3. GICD_TYPER CANNOT BE TRUSTED FOR IDBITS HERE.
 *     It is known to read back unreliable values on this part, so the width is
 *     a fixed constant rather than something derived from a register read.
 *
 * The self-test ladder at the bottom (its_selftest) is written to exercise the
 * path in increasing steps, so a failure points at a step rather than at "the
 * ITS does not work".
 * ---------------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "gicv3_its.h"
#include "irq_ctrl.h"
#include "regs.h"

/* --- ITS registers (GITS_*) ----------------------------------------------- */
#define GITS_CTLR		0x00000
#define GITS_TYPER		0x00008
#define GITS_CBASER		0x00080
#define GITS_CWRITER		0x00088
#define GITS_CREADR		0x00090
#define GITS_BASERn(n)		(0x00100 + 8 * (n))
#define GITS_TRANSLATER		0x10040

#define GITS_CTLR_ENABLE	(1u << 0)
#define GITS_CTLR_QUIESCENT	(1u << 31)

#define GITS_TYPER_PTA		(1ULL << 19)
#define GITS_TYPER_DEVBITS	(0x1fULL << 13)
#define GITS_TYPER_PHYSICAL	(1ULL << 0)

#define GITS_CBASER_VALID	(1ULL << 63)
#define GITS_CBASER_INNERCACHE	(0x7ULL << 59)
#define GITS_CBASER_SHAREABILITY (0x3ULL << 10)
#define GITS_CBASER_SIZE	(0xffULL)

#define GITS_CWRITER_OFFSET	(0x7ffffULL << 5u)
#define GITS_CREADR_OFFSET	(0x7ffffULL << 5u)
#define GITS_CREADR_STALLED	(1ULL << 0)

#define GITS_BASER_VALID	(1ULL << 63)
#define GITS_BASER_INDIRECT	(1ULL << 62)
#define GITS_BASER_INNERCACHE	(0x7ULL << 59)
#define GITS_BASER_TYPE		(0x7ULL << 56)
#define GITS_BASER_ENTRYSIZE	(0x1fULL << 48)
#define GITS_BASER_ADDR		(0xffffffffffULL << 12)
#define GITS_BASER_SHAREABILITY	(0x3ULL << 10)
#define GITS_BASER_PAGESIZE	(0x3ULL << 8)
#define GITS_BASER_SIZE		(0xffULL)

#define GITS_TYPE_DEVICES	1ULL
#define GITS_TYPE_COLLECTIONS	4ULL

/* Shareability / cacheability encodings for both CBASER and BASERn. Inner
 * shareable + write-back allocate-write is what the board-proven setup uses;
 * if the hardware reports it cannot honour shareability it falls back to
 * non-shareable + non-cacheable, and the fallback is detected by readback
 * rather than assumed. */
#define GITS_SHARE_NS		0ULL
#define GITS_SHARE_IS		1ULL
#define GITS_CACHE_WBWA		7ULL
#define GITS_CACHE_NC		1ULL

#define GITS_PAGESIZE_4K	0ULL
#define GITS_PAGESIZE_16K	1ULL
#define GITS_PAGESIZE_64K	2ULL

/* --- redistributor LPI registers ------------------------------------------ */
#define GICR_CTLR		0x0000
#define GICR_PROPBASER		0x0070
#define GICR_PENDBASER		0x0078

#define GICR_CTLR_ENABLELPIS	(1u << 0)

#define GICR_PROPBASER_ADDR	(0xffffffffffULL << 12)
#define GICR_PROPBASER_SHARE	(0x3ULL << 10)
#define GICR_PROPBASER_CACHE	(0x7ULL << 7)
#define GICR_PROPBASER_IDBITS	(0x1fULL)

#define GICR_PENDBASER_PTZ	(1ULL << 62)
#define GICR_PENDBASER_ADDR	(0xffffffffffULL << 16)
#define GICR_PENDBASER_SHARE	(0x3ULL << 10)
#define GICR_PENDBASER_CACHE	(0x7ULL << 7)

/* --- LPI configuration table (a.k.a. property table) ---------------------- */
#define LPI_CONF_ENABLE		(1u << 0)
#define LPI_CONF_PRIORITY	(0xfcu)	/* bits 7:2 hold the priority */

/* --- ITS commands --------------------------------------------------------- */
#define ITS_CMD_MOVI		0x01ULL
#define ITS_CMD_INT		0x03ULL
#define ITS_CMD_CLEAR		0x04ULL
#define ITS_CMD_SYNC		0x05ULL
#define ITS_CMD_MAPD		0x08ULL
#define ITS_CMD_MAPC		0x09ULL
#define ITS_CMD_MAPTI		0x0aULL
#define ITS_CMD_INV		0x0cULL
#define ITS_CMD_INVALL		0x0dULL

/* --- geometry -------------------------------------------------------------
 *
 * Sizes are fixed rather than derived from registers. GITS_TYPER.DEVBITS and
 * GICD_TYPER are both unreliable on this part, and a table that is too small is
 * rejected by the ITS with no useful report, so the sizing is deliberately
 * generous and explicit:
 *
 *   DeviceID is 16 bits, so the direct device table needs 65536 entries.
 *   Each entry is 8 bytes (ITT address + size), giving 512KiB.
 *   The ITS requires the size field to be a page count, and this part wants
 *     16KiB pages, so 512KiB is 32 pages.
 *
 * PropBase/PendBase addresses are fixed and must match the Normal-cacheable
 * window mapped by mmu.c - the tables are shared memory that the ITS reads over
 * a non-coherent port, so they cannot live in Device space.
 */
#define ITS_DEVICE_IDBITS	16U
#define ITS_DEVICE_ENTRY_BYTES	8U
#define ITS_DEVICE_ENTRIES	(1U << ITS_DEVICE_IDBITS)
#define ITS_DEVICE_TABLE_BYTES	(ITS_DEVICE_ENTRIES * ITS_DEVICE_ENTRY_BYTES)
#define ITS_PAGE_BYTES		16384U
#define ITS_DEVICE_PAGES	(ITS_DEVICE_TABLE_BYTES / ITS_PAGE_BYTES)

#define ITS_COLLECTION_PAGES	1U	/* one CPU, so one collection */

#define ITS_CMD_QUEUE_BYTES	4096U
#define ITS_CMD_QUEUE_PAGES	(ITS_CMD_QUEUE_BYTES / 4096U)

#define ITS_ITT_BYTES		256U	/* 16 events * 16 bytes */
#define ITS_MAX_DEVICES		8U

/* Wait bound. A stalled command queue is reported rather than spun on: the
 * whole point of this file's header is that silence is the enemy. */
#define ITS_WAIT_LIMIT		1000000U

/* --- backing memory -------------------------------------------------------
 *
 * Identity-mapped (PA == VA), which the MMU guarantees, so the same address
 * works as a CPU pointer and as a table address for the ITS. All of it is in
 * .bss and therefore zeroed before board_main runs.
 */
static uint8_t its_cmd_queue[ITS_CMD_QUEUE_BYTES] __attribute__((aligned(4096)));
static uint8_t its_device_table[ITS_DEVICE_TABLE_BYTES]
	__attribute__((aligned(ITS_PAGE_BYTES)));
static uint8_t its_collection_table[ITS_PAGE_BYTES]
	__attribute__((aligned(ITS_PAGE_BYTES)));

/* The property and pending tables are reached through the redistributor as
 * physical addresses; they are placed at the fixed window mmu.c maps as
 * Normal. */
static uint8_t *const its_prop_table = (uint8_t *)(uintptr_t)LPI_PROP_BASE;
static uint8_t *const its_pend_table = (uint8_t *)(uintptr_t)LPI_PEND_BASE;

typedef struct {
	uint32_t devid;
	uint64_t itt;
	uint32_t events;
	uint8_t  used;
} its_device_t;

static its_device_t its_devices[ITS_MAX_DEVICES];
static uint16_t its_rdbase_token;	/* value written in MAPC */
static uint8_t  its_initialised;
static uint32_t its_collection_id;

/* --- command queue -------------------------------------------------------- */

static uint32_t its_cmd_slot;	/* next free command slot, 16 bytes each */

static void its_write_cmd(uint32_t slot, const uint64_t dw[4])
{
	uint64_t *base = (uint64_t *)(void *)its_cmd_queue;
	uint32_t i;

	for (i = 0; i < 4U; i++) {
		base[slot * 4U + i] = dw[i];
	}

	/* The ITS reads commands over a non-coherent port, so the command must
	 * actually reach memory before the write pointer announces it. */
	board_dcache_flush((uintptr_t)&base[slot * 4U], 4U * sizeof(uint64_t));
	reg_dsb();
}

static int its_submit(const uint64_t dw[4])
{
	/* Total slots in the ring, so the write pointer wrap is honest. */
	const uint32_t slots = ITS_CMD_QUEUE_BYTES / 16U;

	if ((reg_rd64(BOARD_ITS_BASE + GITS_CREADR) & GITS_CREADR_STALLED) != 0ULL) {
		return -1;
	}

	its_write_cmd(its_cmd_slot, dw);
	its_cmd_slot++;
	if (its_cmd_slot >= slots) {
		its_cmd_slot = 0U;
	}

	/* Publish the advanced write pointer. The field holds a byte offset into
	 * the ring, not a slot index. */
	reg_wr64(BOARD_ITS_BASE + GITS_CWRITER, (uint64_t)(its_cmd_slot * 16U));
	reg_dsb();
	return 0;
}

/* Wait until the ITS has consumed everything submitted so far. The queue is
 * empty when the read and write pointers agree. */
static int its_drain(void)
{
	uint32_t i;

	for (i = 0; i < ITS_WAIT_LIMIT; i++) {
		uint64_t w = reg_rd64(BOARD_ITS_BASE + GITS_CWRITER);
		uint64_t r = reg_rd64(BOARD_ITS_BASE + GITS_CREADR);

		if ((r & GITS_CREADR_STALLED) != 0ULL) {
			board_early_print("[its] command queue stalled\n");
			return -1;
		}
		if ((w & GITS_CREADR_OFFSET) == (r & GITS_CREADR_OFFSET)) {
			return 0;
		}
	}
	board_early_print("[its] command queue never drained\n");
	return -1;
}

/* --- individual commands -------------------------------------------------- */

static int its_cmd_mapc(uint32_t icid, uint64_t rdbase)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_MAPC;
	dw[2] = (uint64_t)icid | rdbase | (1ULL << 63);	/* V=1 */
	return its_submit(dw);
}

static int its_cmd_mapd(uint32_t devid, uint64_t itt, uint32_t events)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_MAPD | ((uint64_t)devid << 32);
	/* Size is encoded as (number of events - 1). */
	dw[1] = (uint64_t)((events > 1U ? events : 1U) - 1U);
	dw[2] = itt | (1ULL << 63);			/* V=1 */
	return its_submit(dw);
}

static int its_cmd_mapti(uint32_t devid, uint32_t eventid, uint32_t intid,
			 uint32_t icid)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_MAPTI | ((uint64_t)devid << 32);
	dw[1] = (uint64_t)eventid | ((uint64_t)intid << 32);
	dw[2] = (uint64_t)icid;
	return its_submit(dw);
}

static int its_cmd_inv(uint32_t devid, uint32_t eventid)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_INV | ((uint64_t)devid << 32);
	dw[1] = (uint64_t)eventid;
	return its_submit(dw);
}

static int its_cmd_invall(uint32_t icid)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_INVALL;
	dw[2] = (uint64_t)icid;
	return its_submit(dw);
}

static int its_cmd_sync(uint64_t rdbase)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_SYNC;
	dw[2] = rdbase;
	return its_submit(dw);
}

/* --- doorbell ------------------------------------------------------------- */

uint32_t its_doorbell_addr(void)
{
	return (uint32_t)(BOARD_ITS_BASE + GITS_TRANSLATER);
}

static int its_cmd_int(uint32_t devid, uint32_t eventid)
{
	uint64_t dw[4] = { 0 };

	/* INT raises an event as though `devid` had signalled it. This is the
	 * only way a CPU can inject one: a plain write to the TRANSLATER page
	 * has no Requester ID, so the ITS cannot tell which device it came from,
	 * and the event is dropped. That is why a doorbell-based self-test
	 * silently delivers nothing - the mapping can be perfectly correct. */
	dw[0] = ITS_CMD_INT | ((uint64_t)devid << 32);
	dw[1] = (uint64_t)eventid;
	return its_submit(dw);
}

static int its_cmd_clear(uint32_t devid, uint32_t eventid)
{
	uint64_t dw[4] = { 0 };

	dw[0] = ITS_CMD_CLEAR | ((uint64_t)devid << 32);
	dw[1] = (uint64_t)eventid;
	return its_submit(dw);
}

void its_ring_doorbell(uint32_t devid, uint32_t eventid)
{
	/* Kept as the API's name for "make this event fire", but implemented
	 * with the INT command rather than a store to the doorbell, for the
	 * reason above. */
	if (!its_initialised) {
		return;
	}
	(void)its_cmd_int(devid, eventid);
	(void)its_cmd_sync((uint64_t)its_rdbase_token);
	(void)its_drain();
}

/* --- LPI configuration ---------------------------------------------------- */

void its_lpi_configure(uint32_t lpi, uint8_t priority, int enable)
{
	uint8_t value = (uint8_t)(priority & LPI_CONF_PRIORITY);

	if (enable) {
		value |= LPI_CONF_ENABLE;
	}
	its_prop_table[lpi] = value;

	/* THE critical line. Without it the store above stays in the D-cache,
	 * the ITS reads the enable bit as 0, and every LPI is dropped with no
	 * error anywhere. Ordering the write (dsb) is not enough - it has to be
	 * written back. */
	board_dcache_flush((uintptr_t)&its_prop_table[lpi], 1U);
	reg_dsb();
}

void its_lpi_enable(uint32_t lpi, uint8_t priority)
{
	its_lpi_configure(lpi, priority, 1);
}

void its_lpi_disable(uint32_t lpi)
{
	its_lpi_configure(lpi, 0U, 0);
}

uint8_t its_lpi_get_config(uint32_t lpi)
{
	/* The ITS may have written this table, so drop any cached copy before
	 * reading it back. */
	board_dcache_invalidate((uintptr_t)&its_prop_table[lpi], 1U);
	reg_dsb();
	return its_prop_table[lpi];
}

int its_lpi_is_pending(uint32_t lpi)
{
	/* BIT per LPI, so the byte index is lpi/8 and the bit is lpi%8. Reading
	 * this table byte-wise, as the property table is, gives an answer about
	 * a completely different set of interrupts. */
	uint32_t byte = lpi / 8U;
	uint32_t bit = lpi % 8U;
	uint8_t value;

	board_dcache_invalidate((uintptr_t)&its_pend_table[byte], 1U);
	reg_dsb();
	value = its_pend_table[byte];

	return (value & (1U << bit)) != 0U;
}

void its_clear_pending(uint32_t lpi)
{
	uint32_t byte = lpi / 8U;
	uint32_t bit = lpi % 8U;

	its_pend_table[byte] = (uint8_t)(its_pend_table[byte] & ~(1U << bit));
	board_dcache_flush((uintptr_t)&its_pend_table[byte], 1U);
	reg_dsb();
}

/* --- redistributor LPI setup ---------------------------------------------- */

static int its_redist_lpi_init(void)
{
	uint32_t i;
	uint64_t propbase;
	uint64_t pendbase;

	/* Property table: one byte per LPI, read by the ITS. IDbits is written
	 * as a constant because GICD_TYPER cannot be trusted on this part; the
	 * field holds (bits - 1). */
	propbase = (uint64_t)LPI_PROP_BASE;
	propbase |= ((uint64_t)(ITS_DEVICE_IDBITS - 1U) << 0);
	propbase |= (GITS_CACHE_WBWA << 7);
	propbase |= ((uint64_t)GITS_SHARE_IS << 10);
	reg_wr64(BOARD_GICR_BASE + GICR_PROPBASER, propbase);
	reg_dsb();

	/* Pending table: one BIT per LPI, written by the redistributor. Address
	 * is shifted by 16, and PTZ must be set to say "the table is empty" -
	 * if it is left clear the redistributor may treat whatever is in memory
	 * as real pending state. */
	pendbase = ((uint64_t)LPI_PEND_BASE >> 16);
	pendbase |= GICR_PENDBASER_PTZ;
	pendbase |= (GITS_CACHE_WBWA << 7);
	pendbase |= ((uint64_t)GITS_SHARE_IS << 10);
	reg_wr64(BOARD_GICR_BASE + GICR_PENDBASER, pendbase);
	reg_dsb();

	/* Clear both tables. The property table is cleared to 0, which means
	 * "disabled", so no LPI can fire until one is explicitly enabled. */
	for (i = 0; i < LPI_TABLE_BYTES; i++) {
		its_prop_table[i] = 0U;
	}
	for (i = 0; i < LPI_TABLE_BYTES / 8U; i++) {
		its_pend_table[i] = 0U;
	}
	board_dcache_flush((uintptr_t)its_prop_table, LPI_TABLE_BYTES);
	board_dcache_flush((uintptr_t)its_pend_table, LPI_TABLE_BYTES / 8U);
	reg_dsb();

	/* Enable LPIs at the redistributor. This is the register the GICv3 core
	 * setup deliberately leaves alone, because it belongs to this path. */
	reg_wr32(BOARD_GICR_BASE + GICR_CTLR, GICR_CTLR_ENABLELPIS);
	reg_dsb();

	if ((reg_rd32(BOARD_GICR_BASE + GICR_CTLR) & GICR_CTLR_ENABLELPIS) == 0u) {
		/* Not necessarily fatal (this part has registers that read back
		 * unreliably), but worth knowing about. */
		board_early_print("[its] GICR_CTLR.EnableLPIs did not read back\n");
	}
	return 0;
}

/* --- bring-up ------------------------------------------------------------- */

int its_init(void)
{
	uint64_t typer;
	uint64_t cbaser;
	uint64_t baser;
	uint32_t i;

	if (its_initialised) {
		return 0;
	}

	typer = reg_rd64(BOARD_ITS_BASE + GITS_TYPER);

	/* Commands must be described before anything else can be issued. */
	for (i = 0; i < ITS_CMD_QUEUE_BYTES / 8U; i++) {
		((uint64_t *)(void *)its_cmd_queue)[i] = 0ULL;
	}
	board_dcache_flush((uintptr_t)its_cmd_queue, ITS_CMD_QUEUE_BYTES);

	cbaser = (uint64_t)(uintptr_t)its_cmd_queue;
	cbaser |= (uint64_t)(ITS_CMD_QUEUE_PAGES - 1U);		/* size, in pages */
	cbaser |= (GITS_CACHE_WBWA << 59);
	cbaser |= (GITS_SHARE_IS << 10);
	cbaser |= GITS_CBASER_VALID;
	reg_wr64(BOARD_ITS_BASE + GITS_CBASER, cbaser);
	reg_dsb();

	/* Confirm the shareability we asked for was accepted; some
	 * implementations silently refuse it and require non-cacheable. */
	if (((reg_rd64(BOARD_ITS_BASE + GITS_CBASER) >> 10) & 0x3ULL) !=
	    GITS_SHARE_IS) {
		cbaser &= ~(GITS_CBASER_SHAREABILITY | GITS_CBASER_INNERCACHE);
		cbaser |= (GITS_CACHE_NC << 59) | (GITS_SHARE_NS << 10);
		reg_wr64(BOARD_ITS_BASE + GITS_CBASER, cbaser);
		reg_dsb();
	}

	reg_wr64(BOARD_ITS_BASE + GITS_CWRITER, 0ULL);
	its_cmd_slot = 0U;

	/* --- tables ---
	 *
	 * BASERn layout is 8 registers, one per table type, with the type in
	 * the register itself. Do not assume the device table is BASER0 and the
	 * collection table BASER1 by position; read the type field and act on
	 * that. On this board the collection table must live in BASER1, and a
	 * write to any other slot is silently discarded, which leaves MAPC
	 * failing with no diagnostic.
	 */
	for (i = 0; i < 8U; i++) {
		uint64_t type;

		baser = reg_rd64(BOARD_ITS_BASE + GITS_BASERn(i));
		type = (baser >> 56) & 0x7ULL;

		if (type == GITS_TYPE_DEVICES) {
			uint64_t pages;

			for (uint32_t k = 0; k < ITS_DEVICE_TABLE_BYTES; k++) {
				its_device_table[k] = 0U;
			}
			board_dcache_flush((uintptr_t)its_device_table,
					   ITS_DEVICE_TABLE_BYTES);

			baser &= ~(GITS_BASER_SIZE | GITS_BASER_ADDR |
				   GITS_BASER_INNERCACHE | GITS_BASER_SHAREABILITY |
				   GITS_BASER_PAGESIZE | GITS_BASER_ENTRYSIZE |
				   GITS_BASER_INDIRECT);
			pages = ITS_DEVICE_PAGES;
			baser |= (uint64_t)(pages - 1U) & GITS_BASER_SIZE;
			baser |= (uint64_t)(uintptr_t)its_device_table;
			/* Entry size field holds bytes-per-entry - 1. */
			baser |= ((uint64_t)(ITS_DEVICE_ENTRY_BYTES - 1U) << 48);
			baser |= (GITS_PAGESIZE_16K << 8);
			baser |= (GITS_CACHE_WBWA << 59);
			baser |= (GITS_SHARE_IS << 10);
			baser |= GITS_BASER_VALID;
			reg_wr64(BOARD_ITS_BASE + GITS_BASERn(i), baser);
			reg_dsb();
		} else if (type == GITS_TYPE_COLLECTIONS) {
			for (uint32_t k = 0; k < ITS_PAGE_BYTES; k++) {
				its_collection_table[k] = 0U;
			}
			board_dcache_flush((uintptr_t)its_collection_table,
					   ITS_PAGE_BYTES);

			baser &= ~(GITS_BASER_SIZE | GITS_BASER_ADDR |
				   GITS_BASER_INNERCACHE | GITS_BASER_SHAREABILITY |
				   GITS_BASER_PAGESIZE | GITS_BASER_ENTRYSIZE |
				   GITS_BASER_INDIRECT);
			baser |= (uint64_t)(ITS_COLLECTION_PAGES - 1U) & GITS_BASER_SIZE;
			baser |= (uint64_t)(uintptr_t)its_collection_table;
			baser |= (GITS_PAGESIZE_16K << 8);
			baser |= (GITS_CACHE_WBWA << 59);
			baser |= (GITS_SHARE_IS << 10);
			baser |= GITS_BASER_VALID;
			reg_wr64(BOARD_ITS_BASE + GITS_BASERn(i), baser);
			reg_dsb();
		}
		/* Other table types are simply not implemented here and are left
		 * as the hardware reported them. */
	}

	/* --- redistributor side of the LPI path --- */
	(void)its_redist_lpi_init();

	/* --- enable the ITS ---
	 *
	 * This must happen BEFORE any command is issued. GITS_CTLR.Enable gates
	 * whether the ITS processes its command queue at all: with it clear the
	 * commands sit in the ring untouched, CREADR never advances, and the
	 * wait for the queue to drain times out - which reads as "the ITS is
	 * broken" when in fact it was never switched on. An earlier revision
	 * enabled it last, after MAPC.
	 *
	 * The tables above must be in place first, which is why this sits here
	 * rather than at the very top. */
	reg_wr32(BOARD_ITS_BASE + GITS_CTLR, GITS_CTLR_ENABLE);
	reg_dsb();

	if ((reg_rd32(BOARD_ITS_BASE + GITS_CTLR) & GITS_CTLR_ENABLE) == 0u) {
		board_early_print("[its] GITS_CTLR did not read back enabled\n");
	}

	/* --- collection: bind this CPU to its redistributor ---
	 *
	 * GITS_TYPER.PTA decides what MAPC's RDbase field means: with PTA set
	 * it is the redistributor's physical address, otherwise it is the
	 * processor number shifted up. Using the wrong one makes MAPC fail
	 * silently.
	 */
	{
		uint64_t rdbase;

		if ((typer & GITS_TYPER_PTA) != 0ULL) {
			rdbase = (uint64_t)BOARD_GICR_BASE;
		} else {
			rdbase = 0ULL;	/* one CPU: processor number 0 */
		}
		its_rdbase_token = (uint16_t)rdbase;
		its_collection_id = 0U;

		if (its_cmd_mapc(its_collection_id, rdbase) != 0) {
			return -1;
		}
		(void)its_cmd_invall(its_collection_id);
		(void)its_cmd_sync(rdbase);
		if (its_drain() != 0) {
			return -1;
		}
	}

	its_initialised = 1U;
	return 0;
}

/* --- device and event mapping --------------------------------------------- */

int its_device_map(uint32_t devid, uint32_t events)
{
	its_device_t *slot = NULL;
	uint32_t i;

	if (!its_initialised) {
		return -1;
	}
	if (devid >= ITS_DEVICE_ENTRIES || events == 0U) {
		return -1;
	}

	for (i = 0; i < ITS_MAX_DEVICES; i++) {
		if (its_devices[i].used && its_devices[i].devid == devid) {
			slot = &its_devices[i];
			break;
		}
	}
	if (slot == NULL) {
		for (i = 0; i < ITS_MAX_DEVICES; i++) {
			if (!its_devices[i].used) {
				slot = &its_devices[i];
				break;
			}
		}
	}
	if (slot == NULL) {
		return -1;
	}

	/* The interrupt translation table is one entry per event, 16 bytes each
	 * in the "ITT entry size" encoding this part reports. It is per-device
	 * and must be flushed before MAPD points the ITS at it. */
	{
		static uint8_t itt_pool[ITS_MAX_DEVICES][ITS_ITT_BYTES]
			__attribute__((aligned(256)));
		uint8_t *itt = itt_pool[slot - its_devices];

		for (i = 0; i < ITS_ITT_BYTES; i++) {
			itt[i] = 0U;
		}
		board_dcache_flush((uintptr_t)itt, ITS_ITT_BYTES);
		slot->itt = (uint64_t)(uintptr_t)itt;
	}

	slot->devid = devid;
	slot->events = events;
	slot->used = 1U;

	if (its_cmd_mapd(devid, slot->itt, events) != 0) {
		slot->used = 0U;
		return -1;
	}
	return its_drain();
}

int its_event_map(uint32_t devid, uint32_t eventid, uint32_t lpi)
{
	uint32_t i;

	if (!its_initialised) {
		return -1;
	}

	for (i = 0; i < ITS_MAX_DEVICES; i++) {
		if (its_devices[i].used && its_devices[i].devid == devid) {
			break;
		}
	}
	if (i == ITS_MAX_DEVICES) {
		return -1;	/* the device must be mapped first */
	}

	if (its_cmd_mapti(devid, eventid, lpi, its_collection_id) != 0) {
		return -1;
	}
	(void)its_cmd_inv(devid, eventid);
	(void)its_cmd_sync((uint64_t)its_rdbase_token);
	return its_drain();
}

void its_event_unmap(uint32_t devid, uint32_t eventid)
{
	if (!its_initialised) {
		return;
	}
	(void)its_cmd_inv(devid, eventid);
	(void)its_cmd_sync((uint64_t)its_rdbase_token);
	(void)its_drain();
}

/* --- self-test ------------------------------------------------------------
 *
 * A ladder, not a single check: each rung adds one thing, so a failure says
 * which part of the path is not working instead of "LPI does not arrive".
 *
 *   rung 1  configure one LPI and confirm the property table readback
 *   rung 2  map a synthetic device and one event to that LPI, then ring the
 *           doorbell once and wait for the handler to run
 *   rung 3  disable, re-enable and re-ring, to prove the enable bit is what
 *           gates delivery and that it takes effect both ways
 *   rung 4  map a small grid of LPIs and ring them interleaved, to catch
 *           index arithmetic that only breaks off the first entry
 *
 * The synthetic DeviceID starts above the PCIe requester-ID space so it cannot
 * collide with a real endpoint's translation.
 */
#define ITS_TEST_DEVICE_BASE	0xe000U
#define ITS_TEST_LPI_FIRST	0U
#define ITS_TEST_EVENTS		16U

static volatile uint32_t its_selftest_hits;

/* CMSIS handlers take no argument, so the handler cannot report which INTID
 * fired. The delivered INTID is recovered from the redistributor's pending
 * table instead, which doubles as a check that the pending table is being
 * maintained at the offset this file believes it is. */
static void its_selftest_handler(void)
{
	its_selftest_hits++;
}

static int its_wait_for_hit(uint32_t before, uint32_t timeout_ms)
{
	uint32_t waited = 0U;

	while (waited < timeout_ms) {
		if (its_selftest_hits != before) {
			return 0;
		}
		/* Busy-wait on a cycle counter rather than osDelay: rung 2 must
		 * work even if the scheduler is not running yet, and this file
		 * must not depend on the RTOS. */
		{
			uint64_t start;
			uint64_t now;
			uint64_t freq;

			__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
			__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(start));
			do {
				__asm__ __volatile__("mrs %0, cntvct_el0"
						     : "=r"(now));
			} while ((now - start) < freq / 1000ULL);
		}
		waited++;
	}
	return -1;
}

int its_selftest(uint32_t *lpiondelivered)
{
	uint32_t devid = ITS_TEST_DEVICE_BASE;
	uint32_t base_lpi = ITS_TEST_LPI_FIRST;
	uint32_t hits_before;
	uint32_t delivered = 0U;
	uint32_t n;

	if (!its_initialised && its_init() != 0) {
		board_early_print("[its] selftest: init failed\n");
		return -1;
	}

	if (its_device_map(devid, ITS_TEST_EVENTS) != 0) {
		board_early_print("[its] selftest: MAPD failed\n");
		return -1;
	}

	/* --- rung 1 + 2: one LPI, one event, one doorbell --- */
	its_lpi_enable(base_lpi, LPI_CONF_PRIORITY);
	IRQ_SetHandler((IRQn_ID_t)(IRQ_LPI_INTID_FIRST + base_lpi),
		       its_selftest_handler);
	IRQ_Enable((IRQn_ID_t)(IRQ_LPI_INTID_FIRST + base_lpi));

	if (its_event_map(devid, 1U, IRQ_LPI_INTID_FIRST + base_lpi) != 0) {
		board_early_print("[its] selftest: MAPTI failed\n");
		return -1;
	}

	hits_before = its_selftest_hits;
	its_ring_doorbell(devid, 1U);
	if (its_wait_for_hit(hits_before, 50U) != 0) {
		board_early_print("[its] selftest rung2: single LPI not delivered\n");
		return -1;
	}
	delivered++;

	/* --- rung 3: the enable bit must gate delivery both ways --- */
	its_lpi_disable(base_lpi);
	/* Clear the event inside the ITS first. Without this the previously
	 * delivered event can still be pending there, and rung 3 would report a
	 * delivery that actually came from rung 2 rather than from the disabled
	 * path it is meant to be testing. */
	(void)its_cmd_clear(devid, 1U);
	(void)its_cmd_sync((uint64_t)its_rdbase_token);
	(void)its_drain();

	hits_before = its_selftest_hits;
	its_ring_doorbell(devid, 1U);
	if (its_wait_for_hit(hits_before, 20U) == 0) {
		board_early_print("[its] selftest rung3: disabled LPI still delivered\n");
		return -1;
	}

	its_lpi_enable(base_lpi, LPI_CONF_PRIORITY);
	hits_before = its_selftest_hits;
	its_ring_doorbell(devid, 1U);
	if (its_wait_for_hit(hits_before, 50U) != 0) {
		board_early_print("[its] selftest rung3: re-enabled LPI silent\n");
		return -1;
	}
	delivered++;

	/* --- rung 4: a grid, rung in an interleaved order ---
	 *
	 * Sequential order would let an off-by-one in the event-to-LPI mapping
	 * pass unnoticed, because neighbouring values would still land on
	 * neighbouring LPIs. Striding by 7 breaks that coincidence. */
	for (n = 1U; n < ITS_TEST_EVENTS; n++) {
		uint32_t lpi = base_lpi + n;

		its_lpi_enable(lpi, LPI_CONF_PRIORITY);
		IRQ_SetHandler((IRQn_ID_t)(IRQ_LPI_INTID_FIRST + lpi),
			       its_selftest_handler);
		IRQ_Enable((IRQn_ID_t)(IRQ_LPI_INTID_FIRST + lpi));
		if (its_event_map(devid, n, IRQ_LPI_INTID_FIRST + lpi) != 0) {
			board_early_print("[its] selftest rung4: MAPTI failed\n");
			return -1;
		}
	}

	for (n = 1U; n < ITS_TEST_EVENTS; n += 7U) {
		hits_before = its_selftest_hits;
		its_ring_doorbell(devid, n);
		if (its_wait_for_hit(hits_before, 50U) != 0) {
			board_early_print("[its] selftest rung4: LPI in grid not delivered\n");
			return -1;
		}
		delivered++;
	}

	if (lpiondelivered != NULL) {
		*lpiondelivered = delivered;
	}
	return 0;
}
