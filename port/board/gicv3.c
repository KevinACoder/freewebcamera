/*
 * @file   gicv3.c
 * @brief  GIC-600 (GICv3) backend for the CMSIS irq_ctrl.h `IRQ_*` API.
 *
 * The interface is CMSIS's, which is interrupt-controller-version independent
 * by design; only this file knows the part is a GICv3. That is the point of
 * using the standard header: an implementation swap does not reach callers.
 *
 * Single core. Only the boot core's redistributor is brought up.
 *
 * ---------------------------------------------------------------------------
 * Board-specific behaviour that this driver has to respect. All of it was
 * established by experiment on this hardware, and getting any one of them
 * wrong produces silence rather than an error.
 *
 *  1. GICD_CTLR is only ever read-modify-written.
 *     Firmware hands over with DS=1 (single security state) and ARE set
 *     (measured 0x12). A plain store of the enable bit clears ARE, and once
 *     ARE is clear the system-register CPU interface stops working: every
 *     ICC_IAR1_EL1 read returns the spurious INTID 1023 while the interrupt
 *     lines keep asserting, and the system wedges in an interrupt storm.
 *
 *  2. The redistributor needs the WAKER handshake.
 *     Clear GICR_WAKER.ProcessorSleep, then wait for ChildrenAsleep to clear.
 *     Skipping it leaves the redistributor asleep and SPIs/PPIs never arrive.
 *
 *  3. GICD_CTLR.EnableGrp1NS is the only enable bit written here.
 *     Group 1 is already enabled by firmware. Enabling it again through
 *     GICD_CTLR is required and sufficient.
 *
 *  4. ICC_IGRPEN1_EL1 is NOT written.
 *     A non-secure write to it resets this board. Firmware has already set
 *     it, and it stays set as long as ARE survives (see point 1).
 *
 *  5. GICR_CTLR is NOT written here.
 *     EnableLPIs belongs to the LPI/ITS setup. An earlier version wrote
 *     GICR_CTLR here with a GICD-derived mask and wrongly set EnableLPIs and
 *     CES together.
 *
 *  6. Enable/disable decisions are verified by behaviour, not readback.
 *     Group status registers read as zero on this silicon, so
 *     IRQ_GetEnableState cannot answer from hardware. It reports the state
 *     this driver set, and callers are expected to confirm with a handler
 *     counter (the SPI soft-trigger acceptance does exactly that).
 * ---------------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "regs.h"
#include "irq_ctrl.h"

/* --- distributor ---------------------------------------------------------- */
#define GICD_CTLR		0x0000
#define GICD_TYPER		0x0004
#define GICD_IGROUPR(n)		(0x0080 + 4 * (n))
#define GICD_ISENABLER(n)	(0x0100 + 4 * (n))
#define GICD_ICENABLER(n)	(0x0180 + 4 * (n))
#define GICD_ISPENDR(n)		(0x0200 + 4 * (n))
#define GICD_ICPENDR(n)		(0x0280 + 4 * (n))
#define GICD_ISACTIVER(n)	(0x0300 + 4 * (n))
#define GICD_IPRIORITYR(n)	(0x0400 + 4 * (n))
#define GICD_ICFGR(n)		(0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)	(0x0d00 + 4 * (n))
#define GICD_IROUTER(n)		(0x6000 + 8 * (n))

#define GICD_CTLR_RWP		(1u << 31)
#define GICD_CTLR_ENABLEGRP1NS	(1u << 1)
#define GICD_CTLR_ARE_NS	(1u << 4)

/* --- redistributor (this core's frame; SGI/PPI frame at +0x10000) --------- */
#define GICR_CTLR		0x0000
#define GICR_TYPER		0x0008
#define GICR_WAKER		0x0014
#define GICR_SGI_OFFSET		0x10000
#define GICR_IGROUPR0		(GICR_SGI_OFFSET + 0x0080)
#define GICR_ISENABLER0		(GICR_SGI_OFFSET + 0x0100)
#define GICR_ICENABLER0		(GICR_SGI_OFFSET + 0x0180)
#define GICR_ISPENDR0		(GICR_SGI_OFFSET + 0x0200)
#define GICR_ICPENDR0		(GICR_SGI_OFFSET + 0x0280)
#define GICR_IPRIORITYR0	(GICR_SGI_OFFSET + 0x0400)
#define GICR_ICFGR0		(GICR_SGI_OFFSET + 0x0c00)
#define GICR_ICFGR1		(GICR_SGI_OFFSET + 0x0c04)
#define GICR_IGRPMODR0		(GICR_SGI_OFFSET + 0x0d00)

#define GICR_CTLR_RWP		(1u << 3)
#define GICR_WAKER_PROCESSORSLEEP (1u << 1)
#define GICR_WAKER_CHILDRENASLEEP (1u << 2)

/* --- INTID ranges --------------------------------------------------------- */
#define IRQ_INTID_SGI_PPI_MAX	31
#define IRQ_INTID_SPI_FIRST	32
#define IRQ_INTID_SPI_MAX	1019
#define IRQ_INTID_LPI_FIRST	8192
#define IRQ_INTID_LPI_MAX	(IRQ_INTID_LPI_FIRST + 255)	/* 208..463 window */
#define IRQ_INTID_SPURIOUS	1023

/* Table spans SGI/PPI + SPI; LPIs get their own window (see gicv3_its.c). */
#define IRQ_TABLE_SIZE		(IRQ_INTID_SPI_MAX + 1)
#define IRQ_LPI_TABLE_SIZE	256

/* Bounded waits. A stuck RWP must surface as a visible failure, not silence:
 * without a bound the driver simply hangs and the board looks dead. */
#define GIC_WAIT_LIMIT		50000000u

static volatile uint32_t *gicd;
static volatile uint32_t *gicr_sgi;
static uint32_t spi_line_count;
static bool gic_ready;

static IRQHandler_t irq_handlers[IRQ_TABLE_SIZE];
static IRQHandler_t lpi_handlers[IRQ_LPI_TABLE_SIZE];
static uint32_t irq_modes[IRQ_TABLE_SIZE];
static volatile uint32_t irq_enabled_bits[(IRQ_TABLE_SIZE + 31u) / 32u];

static void gicd_wait_rwp(void)
{
	uint32_t i;

	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicd + GICD_CTLR) &
		     GICD_CTLR_RWP) == 0u) {
			return;
		}
	}
	board_early_print("[gicv3] distributor RWP stuck\n");
}

static void gicr_wait_rwp(void)
{
	uint32_t i;

	/* The redistributor's own CTLR carries its RWP bit; the frame base sits
	 * one SGI-frame offset below gicr_sgi. */
	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr_sgi -
				      GICR_SGI_OFFSET + GICR_CTLR) &
		     GICR_CTLR_RWP) == 0u) {
			return;
		}
	}
	board_early_print("[gicv3] redistributor RWP stuck\n");
}

static void set_enabled_bit(uint32_t intid)
{
	irq_enabled_bits[intid / 32u] |= (1u << (intid % 32u));
}

static void clear_enabled_bit(uint32_t intid)
{
	irq_enabled_bits[intid / 32u] &= ~(1u << (intid % 32u));
}

static bool get_enabled_bit(uint32_t intid)
{
	return (irq_enabled_bits[intid / 32u] & (1u << (intid % 32u))) != 0u;
}

static void gic_distributor_init(void)
{
	uint32_t typer;
	uint32_t lines;
	uint32_t i;

	/* Step 1: clear only the Group 1 enable, read-modify-write. ARE and DS
	 * survive because nothing else is touched.
	 *
	 * A plain store of 0 here is the classic way to brick this board: it
	 * clears ARE (firmware boots with DS=1 and ARE set, measured 0x12),
	 * after which the system-register CPU interface stops working and the
	 * board hangs with every IAR1 read returning spurious INTID 1023. */
	reg_wr32_masked((uintptr_t)gicd + GICD_CTLR, GICD_CTLR_ENABLEGRP1NS,
			0u);
	gicd_wait_rwp();

	typer = reg_rd32((uintptr_t)gicd + GICD_TYPER);
	lines = ((typer & 0x1fu) + 1u) * 32u;
	if (lines > (IRQ_INTID_SPI_MAX + 1u)) {
		lines = IRQ_INTID_SPI_MAX + 1u;
	}
	spi_line_count = lines;

	/* Every SPI: Group 1 non-secure, masked, level-triggered, and routed
	 * to this core. Routing must be explicit - with ARE set, an SPI whose
	 * GICD_IROUTER is left at reset is never delivered. */
	for (i = IRQ_INTID_SPI_FIRST; i < lines; i++) {
		uint32_t word = i / 32u;
		uint32_t bit = i % 32u;

		reg_wr32((uintptr_t)gicd + GICD_IGROUPR(word),
				  reg_rd32((uintptr_t)gicd +
						   GICD_IGROUPR(word)) | (1u << bit));
		reg_wr32((uintptr_t)gicd + GICD_IGRPMODR(word),
				  reg_rd32((uintptr_t)gicd +
						   GICD_IGRPMODR(word)) & ~(1u << bit));
		reg_wr32((uintptr_t)gicd + GICD_ICENABLER(word), 1u << bit);
		reg_wr32((uintptr_t)gicd + GICD_ICPENDR(word), 1u << bit);

		/* Level-triggered: ICFGR bit for an SPI is 2 bits wide, and 0
		 * means level. Cleared for completeness. */
		reg_wr32((uintptr_t)gicd + GICD_ICFGR(i / 16u),
				  reg_rd32((uintptr_t)gicd +
						   GICD_ICFGR(i / 16u)) &
				  ~(3u << (2u * (i % 16u))));

		/* Route to CPU 0 (all affinity fields zero, IRM=0). */
		reg_wr64((uintptr_t)gicd + GICD_IROUTER(i), 0u);
	}
	gicd_wait_rwp();

	/* Mid-priority default (0xa0). Callers override per interrupt: the
	 * tick must be lowest, and anything calling a FromISR API must use the
	 * kernel's API-call priority. */
	for (i = IRQ_INTID_SPI_FIRST; i < lines; i++) {
		reg_wr8((uintptr_t)gicd + GICD_IPRIORITYR(i), 0xa0u);
	}
	gicd_wait_rwp();

	/* Step N: enable Group 1 non-secure. Read-modify-write, so ARE and DS
	 * are preserved - this is the single most important line in the file. */
	reg_wr32_masked((uintptr_t)gicd + GICD_CTLR,
				 GICD_CTLR_ENABLEGRP1NS | GICD_CTLR_ARE_NS,
				 GICD_CTLR_ENABLEGRP1NS | GICD_CTLR_ARE_NS);
	gicd_wait_rwp();
}

static void gic_redistributor_init(void)
{
	uint32_t i;

	/* Wake the redistributor: clear ProcessorSleep, wait for
	 * ChildrenAsleep to clear. Without this SPIs and PPIs never arrive. */
	reg_wr32((uintptr_t)gicr_sgi - GICR_SGI_OFFSET + GICR_WAKER,
			  reg_rd32((uintptr_t)gicr_sgi - GICR_SGI_OFFSET +
					   GICR_WAKER) &
			  ~GICR_WAKER_PROCESSORSLEEP);

	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr_sgi - GICR_SGI_OFFSET +
				      GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) == 0u) {
			break;
		}
	}
	if (i == GIC_WAIT_LIMIT) {
		board_early_print("[gicv3] redistributor never woke\n");
	}

	gicr_wait_rwp();

	/* SGI and PPI: Group 1, masked, level for PPIs / edge for SGIs.
	 * GICR_CTLR is deliberately NOT written - EnableLPIs is owned by the
	 * LPI setup path. */
	reg_wr32((uintptr_t)gicr_sgi + GICR_ICENABLER0, 0xffffffffu);
	reg_wr32((uintptr_t)gicr_sgi + GICR_ICPENDR0, 0xffffffffu);
	reg_wr32((uintptr_t)gicr_sgi + GICR_IGROUPR0, 0xffffffffu);
	reg_wr32((uintptr_t)gicr_sgi + GICR_IGRPMODR0, 0x00000000u);

	for (i = 0; i < 32u; i++) {
		reg_wr8((uintptr_t)gicr_sgi + GICR_IPRIORITYR0 + i, 0xa0u);
	}

	/* SGIs edge-triggered, PPIs level-triggered. */
	reg_wr32((uintptr_t)gicr_sgi + GICR_ICFGR0, 0xaaaaaaaa);
	reg_wr32((uintptr_t)gicr_sgi + GICR_ICFGR1, 0x00000000);

	gicr_wait_rwp();
}

static void gic_cpu_interface_init(void)
{
	uint32_t sre;

	/* System register access must be enabled or the ICC_* registers are
	 * inaccessible (and with a GICv3 that is fatal, not a fallback). */
	sre = 0;
	__asm__ __volatile__("mrs %0, s3_0_c12_c12_5" : "=r"(sre));
	if ((sre & 0x1u) == 0u) {
		sre |= 0x7u;	/* SRE | DFB | DIB */
		__asm__ __volatile__("msr s3_0_c12_c12_5, %0" ::"r"(sre));
		__asm__ __volatile__("isb" ::: "memory");
		sre = 0;
		__asm__ __volatile__("mrs %0, s3_0_c12_c12_5" : "=r"(sre));
		if ((sre & 0x1u) == 0u) {
			board_early_print("[gicv3] ICC_SRE will not set\n");
			return;
		}
	}

	/* Accept every priority. The FreeRTOS port narrows this with
	 * ICC_PMR_EL1 for its own critical sections; leaving it wide open here
	 * means an interrupt raised during boot is delivered rather than
	 * silently masked. */
	__asm__ __volatile__("msr s3_0_c4_c6_0, %0" ::"r"(0xffu));

	/* ICC_IGRPEN1_EL1 is deliberately not written: a non-secure write
	 * resets this board. Firmware has already enabled Group 1. */
}

void board_gicv3_init(void)
{
	uint32_t i;

	if (gic_ready) {
		return;
	}

	gicd = (volatile uint32_t *)(uintptr_t)BOARD_GICD_BASE;
	gicr_sgi = (volatile uint32_t *)(uintptr_t)(BOARD_GICR_BASE +
						    GICR_SGI_OFFSET);

	for (i = 0; i < IRQ_TABLE_SIZE; i++) {
		irq_handlers[i] = NULL;
		irq_modes[i] = IRQ_MODE_TRIG_LEVEL;
	}
	for (i = 0; i < IRQ_LPI_TABLE_SIZE; i++) {
		lpi_handlers[i] = NULL;
	}
	for (i = 0; i < (IRQ_TABLE_SIZE + 31u) / 32u; i++) {
		irq_enabled_bits[i] = 0u;
	}

	/* Order matters: distributor, then redistributor, then CPU
	 * interface. Bringing the CPU interface up before the redistributor
	 * is awake leaves PPIs undeliverable. */
	/* Order matters: distributor, then redistributor, then CPU interface.
	 * Bringing the CPU interface up before the redistributor is awake
	 * leaves PPIs undeliverable. The markers exist because a hang inside
	 * any of these is otherwise indistinguishable from dead hardware. */
	board_early_print("[gicv3] dist\n");
	gic_distributor_init();

	board_early_print("[gicv3] redist\n");
	gic_redistributor_init();

	board_early_print("[gicv3] cpuiface\n");
	gic_cpu_interface_init();

	board_early_print("[gicv3] ready\n");
	gic_ready = true;
}

/* --- CMSIS irq_ctrl.h API ------------------------------------------------- */

/* The framework's interrupt entry point (same name and role as IRQ_Handler in
 * CMSIS's own irq_ctrl_gic.c): acknowledge, dispatch, end. Drivers that own
 * their own vector entry can call this.
 *
 * The FreeRTOS port does NOT use it - it reads ICC_IAR1_EL1 itself and calls
 * board_gicv3_dispatch with the INTID - but keeping the framework's entry
 * point costs nothing and means a non-FreeRTOS consumer still works. */
void IRQ_Handler(void)
{
	IRQn_ID_t irqn = (IRQn_ID_t)IRQ_GetActiveIRQ();

	if (irqn >= 0 && irqn != (IRQn_ID_t)IRQ_INTID_SPURIOUS) {
		board_gicv3_dispatch((uint32_t)irqn);
		IRQ_EndOfInterrupt(irqn);
	}
}

int32_t IRQ_Initialize(void)
{
	board_gicv3_init();
	return 0;
}

int32_t IRQ_SetHandler(IRQn_ID_t irqn, IRQHandler_t handler)
{
	if (irqn < 0) {
		return -1;
	}

	if (irqn >= IRQ_INTID_LPI_FIRST &&
	    irqn < IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE) {
		lpi_handlers[irqn - IRQ_INTID_LPI_FIRST] = handler;
		return 0;
	}
	if (irqn < IRQ_TABLE_SIZE) {
		irq_handlers[irqn] = handler;
		return 0;
	}
	return -1;
}

IRQHandler_t IRQ_GetHandler(IRQn_ID_t irqn)
{
	if (irqn < 0) {
		return NULL;
	}
	if (irqn >= IRQ_INTID_LPI_FIRST &&
	    irqn < IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE) {
		return lpi_handlers[irqn - IRQ_INTID_LPI_FIRST];
	}
	if (irqn < IRQ_TABLE_SIZE) {
		return irq_handlers[irqn];
	}
	return NULL;
}

int32_t IRQ_Enable(IRQn_ID_t irqn)
{
	uint32_t word;
	uint32_t bit;

	if (irqn < 0) {
		return -1;
	}

	if (irqn >= IRQ_INTID_LPI_FIRST) {
		/* LPIs are enabled through the ITS, in gicv3_its.c. */
		return -1;
	}
	if (irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}

	word = (uint32_t)irqn / 32u;
	bit = (uint32_t)irqn % 32u;

	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr_sgi + GICR_ISENABLER0,
				  1u << bit);
	} else {
		reg_wr32((uintptr_t)gicd + GICD_ISENABLER(word),
				  1u << bit);
	}
	reg_dsb();
	set_enabled_bit((uint32_t)irqn);
	return 0;
}

int32_t IRQ_Disable(IRQn_ID_t irqn)
{
	uint32_t word;
	uint32_t bit;

	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}

	word = (uint32_t)irqn / 32u;
	bit = (uint32_t)irqn % 32u;

	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr_sgi + GICR_ICENABLER0,
				  1u << bit);
	} else {
		reg_wr32((uintptr_t)gicd + GICD_ICENABLER(word),
				  1u << bit);
	}
	reg_dsb();
	clear_enabled_bit((uint32_t)irqn);
	return 0;
}

uint32_t IRQ_GetEnableState(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return 0u;
	}
	/* Reported from driver state, not read back: the group status
	 * registers read as zero on this silicon. See the file header. */
	return get_enabled_bit((uint32_t)irqn) ? 1u : 0u;
}

int32_t IRQ_SetMode(IRQn_ID_t irqn, uint32_t mode)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if ((mode & IRQ_MODE_MODEL_Msk) == IRQ_MODE_MODEL_1N) {
		/* 1-N model is a GICv1/v2 concept; not expressible here. */
		return -1;
	}
	irq_modes[irqn] = mode;
	return 0;
}

uint32_t IRQ_GetMode(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return IRQ_MODE_ERROR;
	}
	return irq_modes[irqn];
}

int32_t IRQ_GetActiveIRQ(void)
{
	uint32_t intid;

	__asm__ __volatile__("mrs %0, s3_0_c12_c12_0" : "=r"(intid));
	return (int32_t)intid;
}

int32_t IRQ_GetActiveFIQ(void)
{
	uint32_t intid;

	__asm__ __volatile__("mrs %0, s3_0_c12_c12_4" : "=r"(intid));
	return (int32_t)intid;
}

int32_t IRQ_EndOfInterrupt(IRQn_ID_t irqn)
{
	if (irqn < 0) {
		return -1;
	}
	__asm__ __volatile__("msr s3_0_c12_c12_1, %0" ::"r"((uint32_t)irqn));
	reg_dsb();
	return 0;
}

int32_t IRQ_SetPending(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr_sgi + GICR_ISPENDR0,
				  1u << ((uint32_t)irqn % 32u));
	} else {
		reg_wr32((uintptr_t)gicd + GICD_ISPENDR((uint32_t)irqn / 32u),
				  1u << ((uint32_t)irqn % 32u));
	}
	reg_dsb();
	return 0;
}

uint32_t IRQ_GetPending(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return 0u;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		return (reg_rd32((uintptr_t)gicr_sgi + GICR_ISPENDR0) &
			(1u << ((uint32_t)irqn % 32u))) ? 1u : 0u;
	}
	return (reg_rd32((uintptr_t)gicd +
				 GICD_ISPENDR((uint32_t)irqn / 32u)) &
		(1u << ((uint32_t)irqn % 32u))) ? 1u : 0u;
}

int32_t IRQ_ClearPending(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr_sgi + GICR_ICPENDR0,
				  1u << ((uint32_t)irqn % 32u));
	} else {
		reg_wr32((uintptr_t)gicd + GICD_ICPENDR((uint32_t)irqn / 32u),
				  1u << ((uint32_t)irqn % 32u));
	}
	reg_dsb();
	return 0;
}

int32_t IRQ_SetPriority(IRQn_ID_t irqn, uint32_t priority)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr8((uintptr_t)gicr_sgi + GICR_IPRIORITYR0 +
				 (uint32_t)irqn, (uint8_t)(priority & 0xffu));
	} else {
		reg_wr8((uintptr_t)gicd + GICD_IPRIORITYR((uint32_t)irqn),
				 (uint8_t)(priority & 0xffu));
	}
	reg_dsb();
	return 0;
}

uint32_t IRQ_GetPriority(IRQn_ID_t irqn)
{
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return IRQ_PRIORITY_ERROR;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		return reg_rd8((uintptr_t)gicr_sgi + GICR_IPRIORITYR0 +
				       (uint32_t)irqn);
	}
	return reg_rd8((uintptr_t)gicd + GICD_IPRIORITYR((uint32_t)irqn));
}

int32_t IRQ_SetPriorityMask(uint32_t priority)
{
	__asm__ __volatile__("msr s3_0_c4_c6_0, %0" ::"r"(priority & 0xffu));
	reg_dsb();
	return 0;
}

uint32_t IRQ_GetPriorityMask(void)
{
	uint32_t value;

	__asm__ __volatile__("mrs %0, s3_0_c4_c6_0" : "=r"(value));
	return value & 0xffu;
}

int32_t IRQ_SetPriorityGroupBits(uint32_t bits)
{
	/* Binary point is fixed by the kernel port's priority model
	 * (configUNIQUE_INTERRUPT_PRIORITIES == 16 implies 4 bits of
	 * preemption). Changing it would invalidate the port's assertions, so
	 * only the value it expects is accepted. */
	if (bits == IRQ_PRIORITY_Msk || bits == 0u) {
		return 0;
	}
	return -1;
}

uint32_t IRQ_GetPriorityGroupBits(void)
{
	return 0u;
}

/* --- dispatch ------------------------------------------------------------- */

/* Called by the kernel port's vApplicationIRQHandler with the INTID already
 * acknowledged (the port reads ICC_IAR1_EL1 and performs EOI itself). Runs in
 * interrupt context: handlers must not block. */
void board_gicv3_dispatch(uint32_t intid)
{
	IRQHandler_t handler;

	if (intid == IRQ_INTID_SPURIOUS || intid > IRQ_INTID_SPI_MAX) {
		if (intid >= IRQ_INTID_LPI_FIRST &&
		    intid < IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE) {
			handler = lpi_handlers[intid - IRQ_INTID_LPI_FIRST];
			if (handler != NULL) {
				handler();
			}
		}
		/* Spurious or unhandled: the port still does the EOI, so
		 * dropping it here is correct. */
		return;
	}

	handler = irq_handlers[intid];
	if (handler != NULL) {
		handler();
	}
}
