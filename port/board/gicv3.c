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
/* Per-interrupt priority is ONE BYTE per INTID, at offset 0x400 + id - there
 * is no 4-byte stride here. The register is byte-granular so that a 32-bit
 * access would cover four interrupts; every accessor below uses reg_wr8 /
 * reg_rd8 accordingly.
 *
 * Getting this wrong is not a cosmetic error. With a 4x stride, the loop that
 * seeds default priorities for the SPI range lands on 0x480 upwards instead of
 * 0x420 upwards, and by INTID ~256 it is writing into the reserved region at
 * 0x800 and beyond. On this SoC a write to that hole does not fault cleanly -
 * the system resets, which is what made bring-up look like a GIC hang. */
#define GICD_IPRIORITYR(id)	(0x0400 + (id))
#define GICD_ICFGR(n)		(0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)	(0x0d00 + 4 * (n))
#define GICD_IROUTER(n)		(0x6000 + 8 * (n))

#define GICD_CTLR_RWP		(1u << 31)
#define GICD_CTLR_ENABLEGRP1NS	(1u << 1)
#define GICD_CTLR_ARE_NS	(1u << 4)

/* --- redistributor (this core's frame) ------------------------------------
 *
 * A redistributor is two 64KiB frames: the RD_base frame at offset 0 (CTLR,
 * TYPER, WAKER, ...) and the SGI_base frame at +0x10000 (the per-INTID
 * Group/Enable/Pending/Priority/Config registers for INTID 0..31).
 *
 * ALL offsets below are relative to `gicr_base`, which is the RD_base frame -
 * including the ones that carry GICR_SGI_OFFSET. That is the whole convention:
 * every access is `gicr_base + OFFSET`, with no second addition anywhere.
 *
 * An earlier revision kept a second pointer that already had GICR_SGI_OFFSET
 * folded in and then added these offsets to it, so every SGI/PPI register
 * access landed one frame too high (RD + 0x20100 instead of RD + 0x10100).
 * The consequence was invisible in exactly the way that matters: the timer
 * raised its interrupt and set ISPENDR0, but the enable bit was written to
 * unused space, so INTID 27 was never enabled and no interrupt ever reached
 * the CPU. The scheduler started, ran the first task, and then every delay
 * blocked forever. */
#define GICR_CTLR		0x0000
#define GICR_TYPER		0x0008
#define GICR_WAKER		0x0014
#define GICR_SGI_OFFSET		0x10000	/* start of the SGI_base frame */
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
static volatile uint32_t *gicr_base;
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

	/* The redistributor's own CTLR carries its RWP bit; it lives in the
	 * RD_base frame, so it is reached as gicr_base + GICR_CTLR. */
	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr_base + GICR_CTLR) &
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
	uint32_t waker;

	/* Wake the redistributor: clear ProcessorSleep, wait for ChildrenAsleep
	 * to clear. Without this SPIs and PPIs never arrive.
	 *
	 * NOTHING ELSE IS DONE HERE, and that is deliberate. An earlier version
	 * continued with blanket writes over this core's SGI/PPI frame:
	 *
	 *     GICR_ICENABLER0  = 0xffffffff
	 *     GICR_ICPENDR0    = 0xffffffff
	 *     GICR_IGROUPR0    = 0xffffffff
	 *     GICR_IGRPMODR0   = 0x00000000
	 *     GICR_IPRIORITYR0[0..31] = 0xa0
	 *
	 * Those registers are not ours alone. They cover all 32 SGIs and PPIs,
	 * and firmware owns some of them: the EL1 *physical* timer (PPI 29) is
	 * routed as a Group 0 interrupt by OP-TEE, which is exactly why the
	 * physical timer is unusable from non-secure code here. Reassigning the
	 * whole bank to Group 1 non-secure, and clearing pend/enable for every
	 * line including the secure ones, is a non-secure write to secure state.
	 * On this board that does not fault politely - the system resets,
	 * reproducibly, partway through bring-up, with the console going silent
	 * immediately after the redistributor marker.
	 *
	 * Per-interrupt configuration belongs to the IRQ_* API anyway: callers
	 * set the handler, priority and enable for the lines they own through
	 * IRQ_SetHandler / IRQ_SetPriority / IRQ_Enable, and those paths touch
	 * one line at a time. That is the CMSIS model this file implements, and
	 * it keeps us off firmware's lines by construction. */
	waker = reg_rd32((uintptr_t)gicr_base + GICR_WAKER);
	reg_wr32((uintptr_t)gicr_base + GICR_WAKER,
			  waker & ~GICR_WAKER_PROCESSORSLEEP);

	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr_base +
				      GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) == 0u) {
			break;
		}
	}
	if (i == GIC_WAIT_LIMIT) {
		board_early_print("[gicv3] redistributor never woke\n");
	}

	gicr_wait_rwp();
}

static void gic_cpu_interface_sre_enable(void)
{
	uint32_t sre;

	/* System register access must be enabled or the ICC_* registers are
	 * inaccessible (and with a GICv3 that is fatal, not a fallback).
	 * Runs before anything else touches the GIC. */
	sre = 0;
	__asm__ __volatile__("mrs %0, s3_0_c12_c12_5" : "=r"(sre));
	if ((sre & 0x1u) != 0u) {
		return;
	}

	sre |= 0x7u;	/* SRE | DFB | DIB */
	__asm__ __volatile__("msr s3_0_c12_c12_5, %0" ::"r"(sre));
	__asm__ __volatile__("isb" ::: "memory");

	sre = 0;
	__asm__ __volatile__("mrs %0, s3_0_c12_c12_5" : "=r"(sre));
	if ((sre & 0x1u) == 0u) {
		board_early_print("[gicv3] ICC_SRE will not set\n");
	}
}

static void gic_cpu_interface_init(void)
{
	/* Accept every priority. The FreeRTOS port narrows this with
	 * ICC_PMR_EL1 for its own critical sections; leaving it wide open here
	 * means an interrupt raised during boot is delivered rather than
	 * silently masked. */
	__asm__ __volatile__("msr s3_0_c4_c6_0, %0" ::"r"(0xffu));

	/* Enable Group 1 signalling at the CPU interface: ICC_IGRPEN1_EL1,
	 * S3_0_C12_C12_7.
	 *
	 * An earlier revision deliberately skipped this on the belief that a
	 * non-secure write to it resets the board. That belief was wrong - the
	 * board-proven GICv3 driver for this SoC writes it - and skipping it
	 * was the reason no Group 1 interrupt was ever delivered: the
	 * scheduler started, the first task ran, and then every osDelay()
	 * blocked forever because the tick never arrived. Distributor and
	 * redistributor configuration all read back correct, which is exactly
	 * what makes this failure mode quiet. */
	{
		uint32_t grpen = 0;

		__asm__ __volatile__("mrs %0, s3_0_c12_c12_7" : "=r"(grpen));
		if ((grpen & 0x1u) == 0u) {
			grpen |= 0x1u;
			__asm__ __volatile__("msr s3_0_c12_c12_7, %0"
					     ::"r"(grpen));
			__asm__ __volatile__("isb" ::: "memory");
		}
	}
}

void board_gicv3_init(void)
{
	uint32_t i;

	if (gic_ready) {
		return;
	}

	gicd = (volatile uint32_t *)(uintptr_t)BOARD_GICD_BASE;
	gicr_base = (volatile uint32_t *)(uintptr_t)BOARD_GICR_BASE;

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

	/* Order matters: SRE first, then distributor, then redistributor/CPU
	 * interface.
	 *
	 * System register access has to be enabled before any ICC_* access, and
	 * before the CPU interface is programmed at all. The reference sequence
	 * proven on this board does it first; an earlier revision of this file
	 * did it last, after GICD/GICR had already been written, which is a
	 * hazardous order on hardware where the security state of the two views
	 * differs. */
	gic_cpu_interface_sre_enable();
	gic_distributor_init();
	gic_redistributor_init();
	gic_cpu_interface_init();
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
		/* An LPI has no enable bit in a GICR_ISENABLER register: its
		 * enabled state lives in the ITS property table, which the ITS
		 * module owns. Report success only for a range this build can
		 * actually configure, so a caller cannot believe an
		 * out-of-window LPI was enabled. The property table itself is
		 * programmed by the caller through the ITS API, so there is
		 * nothing to do here beyond validating the number. */
		if (irqn < (IRQn_ID_t)(IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE)) {
			return 0;
		}
		return -1;
	}
	if (irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}

	word = (uint32_t)irqn / 32u;
	bit = (uint32_t)irqn % 32u;

	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		/* Route this one line to Group 1 non-secure so a non-secure
		 * handler can receive it.
		 *
		 * Read-modify-write of a SINGLE bit, never a bank write. This
		 * bank also holds firmware's lines (the EL1 physical timer is
		 * PPI 29, claimed as Group 0 by OP-TEE), and rewriting the whole
		 * bank is a non-secure write to secure state that resets this
		 * board. Touching only the requested line means a caller can
		 * only ever disturb an interrupt it explicitly asked to
		 * enable.
		 *
		 * IGRPMODR0 must be cleared as well, not just IGROUPR0 set. The
		 * two bits together select the group: IGROUPR0=1 with
		 * IGRPMODR0=1 is Group 1 *Secure*, which a non-secure handler
		 * never receives. Setting only IGROUPR0 therefore looks correct
		 * in every readback and still delivers nothing. */
		reg_wr32((uintptr_t)gicr_base + GICR_IGROUPR0,
				  reg_rd32((uintptr_t)gicr_base + GICR_IGROUPR0) |
				  (1u << bit));
		reg_wr32((uintptr_t)gicr_base + GICR_IGRPMODR0,
				  reg_rd32((uintptr_t)gicr_base + GICR_IGRPMODR0) &
				  ~(1u << bit));

		reg_wr32((uintptr_t)gicr_base + GICR_ISENABLER0,
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

	if (irqn >= IRQ_INTID_LPI_FIRST) {
		/* Mirror of IRQ_Enable: an LPI's enabled state is the ITS
		 * property table's, not a GICR_ICENABLER bit. */
		if (irqn < (IRQn_ID_t)(IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE)) {
			return 0;
		}
		return -1;
	}
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}

	word = (uint32_t)irqn / 32u;
	bit = (uint32_t)irqn % 32u;

	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr_base + GICR_ICENABLER0,
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
		reg_wr32((uintptr_t)gicr_base + GICR_ISPENDR0,
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
		return (reg_rd32((uintptr_t)gicr_base + GICR_ISPENDR0) &
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
		reg_wr32((uintptr_t)gicr_base + GICR_ICPENDR0,
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
	if (irqn >= IRQ_INTID_LPI_FIRST) {
		/* An LPI's priority lives in the ITS property table, not in a
		 * per-INTID GICR register. It is set through the ITS module when
		 * the LPI is enabled; accepting a value here and dropping it
		 * would be worse than refusing. */
		return -1;
	}
	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr8((uintptr_t)gicr_base + GICR_IPRIORITYR0 +
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
		return reg_rd8((uintptr_t)gicr_base + GICR_IPRIORITYR0 +
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
