/*
 * @file   gicv3.c
 * @brief  GIC-600 (GICv3) backend for the CMSIS irq_ctrl.h `IRQ_*` API.
 *
 * The interface is CMSIS's, which is interrupt-controller-version independent
 * by design; only this file knows the part is a GICv3. That is the point of
 * using the standard header: an implementation swap does not reach callers.
 *
 * SMP: the distributor is global and brought up once, by the boot core; the
 * redistributor and CPU interface are per-core and each core brings up its
 * own (board_gicv3_secondary_init). Frame ownership comes from a
 * GICR_TYPER affinity walk, not from a hard-coded base.
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
#include "gicv3_its.h"
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

/* --- redistributor (the calling core's frame) ------------------------------
 *
 * A redistributor is two 64KiB frames: the RD_base frame at offset 0 (CTLR,
 * TYPER, WAKER, ...) and the SGI_base frame at +0x10000 (the per-INTID
 * Group/Enable/Pending/Priority/Config registers for INTID 0..31).
 *
 * ALL offsets below are relative to the frame base returned by
 * gicr_core_base(), which is the RD_base frame of the CALLING core -
 * including the ones that carry GICR_SGI_OFFSET. That is the whole
 * convention: every access is `gicr + OFFSET`, with no second addition
 * anywhere.
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
/* Dispatch window 8192..8447 (IRQ_LPI_TABLE_SIZE entries below); the ITS
 * itself hands out only ITS_LPI_QUANTITY slots above IRQ_LPI_INTID_FIRST. */
#define IRQ_INTID_LPI_MAX	(IRQ_INTID_LPI_FIRST + 255)
#define IRQ_INTID_SPURIOUS	1023

/* Table spans SGI/PPI + SPI; LPIs get their own window (see gicv3_its.c). */
#define IRQ_TABLE_SIZE		(IRQ_INTID_SPI_MAX + 1)
#define IRQ_LPI_TABLE_SIZE	256

/* Bounded waits. A stuck RWP must surface as a visible failure, not silence:
 * without a bound the driver simply hangs and the board looks dead. */
#define GIC_WAIT_LIMIT		50000000u

static volatile uint32_t *gicd;
/* One RD_base frame per logical core, resolved by the TYPER walk in
 * gicr_probe_frames(). All GICR accesses go through gicr_core_base(), which
 * returns the CALLING core's frame - the per-core registers (SGI/PPI bank,
 * WAKER) must touch the frame of whoever executes the access. */
static volatile uint32_t *gicr_frames[BOARD_SMP_CORES];
static uint32_t spi_line_count;
static bool gic_ready;

#define GICR_TYPER_LAST		(1ull << 8)

/* This core's redistributor frame. The fallback exists for the one window
 * where an IRQ_* call can precede board_gicv3_init() on the boot core (see
 * irq_ensure_ready): before the probe, "core 0's frame" is the same constant
 * the file has always used. */
static volatile uint32_t *gicr_core_base(void)
{
	uint32_t core = board_smp_core_id();

	if (core < BOARD_SMP_CORES && gicr_frames[core] != NULL) {
		return gicr_frames[core];
	}
	return (volatile uint32_t *)(uintptr_t)BOARD_GICR_BASE;
}

/* Resolve the CALLING core's own redistributor frame.
 *
 * MEASURED ON THIS BOARD (bring-up round 3, boot-time sweep): the four
 * frames at BOARD_GICR_BASE + n*BOARD_GICR_STRIDE read
 *
 *   frame0 typer 00000000_00000021
 *   frame1 typer 00000100_00000121
 *   frame2 typer 00000200_00000221
 *   frame3 typer 00000300_00000331   (bit8 = Last)
 *
 * i.e. GICR_TYPER carries the affinity as the 8-bit Aff0 value at bits
 * [39:32] (the original GICv3.0 layout) - NOT the 32-bit MPIDR-shaped value
 * at [63:32] that newer GIC parts and Linux's GIC-700 path use. Matching on
 * [39:32] is therefore the primary, architectural resolution; the frame
 * order (frame n = core n) agrees with every frame seen and serves as the
 * fallback. Prints stay: if a firmware update changes the layout, the log
 * shows it. */
static void gicr_probe_self(const char *tag)
{
	static const char boot_tag[] = "boot";
	uint32_t me = board_smp_core_id();
	uintptr_t frame = (uintptr_t)BOARD_GICR_BASE;
	uint32_t i;
	uint64_t my_aff = 0ull;

	__asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(my_aff));
	my_aff &= 0xffffffull;	/* Aff2 | Aff1 | Aff0, MPIDR-shaped */

	for (i = 0u; i < BOARD_SMP_CORES * 2u;
	     i++, frame += BOARD_GICR_STRIDE) {
		uint64_t typer = reg_rd64(frame + GICR_TYPER);

		/* TYPER[55:32] holds the owning PE's affinity value in MPIDR
		 * shape: measured 0x000000 / 0x000100 / 0x000200 / 0x000300
		 * for frames 0..3 (this SoC numbers its cores in Aff1). */
		if ((typer >> 32) == my_aff) {
			gicr_frames[me] = (volatile uint32_t *)frame;
			/* Secondaries stay SILENT here: their prints land in
			 * the middle of OP-TEE's release-message window on
			 * the same UART, and that overlap wedged the
			 * transmitter during bring-up (rounds 6-10). The
			 * boot core's sweep above already logged every
			 * frame's TYPER. */
			if (tag == boot_tag) {
				board_log("gicv3: core%u %s frame typer"
					  " %08x%08x waker %x",
					  (unsigned)me, tag,
					  (unsigned)(typer >> 32),
					  (unsigned)typer,
					  (unsigned)reg_rd32(frame +
							     GICR_WAKER));
			}
			return;
		}
		if ((typer & GICR_TYPER_LAST) != 0u) {
			break;
		}
	}

	/* Fallback: frame order. On this SoC the two mappings agree (see the
	 * sweep above); reaching here means TYPER stopped making sense.
	 * INTEGER arithmetic first, pointer cast last: `ptr + n` on a
	 * uint32_t * scales n by 4, which turned frame 2 into frame 8 the
	 * first time this fallback ever ran. */
	gicr_frames[me] = (volatile uint32_t *)(uintptr_t)
			  ((uintptr_t)BOARD_GICR_BASE +
			   (uintptr_t)me * BOARD_GICR_STRIDE);
	if (tag == boot_tag) {
		board_log("gicv3: core%u %s: TYPER no match, using frame"
			  " order",
			  (unsigned)me, tag);
	}
}

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
	board_early_print("gicv3: distributor RWP stuck\n");
}

static void gicr_wait_rwp(void)
{
	uint32_t i;
	volatile uint32_t *gicr = gicr_core_base();

	/* The redistributor's own CTLR carries its RWP bit; it lives in the
	 * RD_base frame, so it is reached as gicr + GICR_CTLR. */
	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr + GICR_CTLR) &
		     GICR_CTLR_RWP) == 0u) {
			return;
		}
	}
	board_early_print("gicv3: redistributor RWP stuck\n");
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

		/* Route to CPU 0 (all affinity fields zero, IRM=0). Every SPI
		 * stays boot-core-affine by design: the drivers' ISRs run on
		 * the boot core and waking tasks across cores is the kernel
		 * port's business, not the routing table's. */
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
	volatile uint32_t *gicr = gicr_core_base();

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
	waker = reg_rd32((uintptr_t)gicr + GICR_WAKER);
	reg_wr32((uintptr_t)gicr + GICR_WAKER,
		  waker & ~GICR_WAKER_PROCESSORSLEEP);

	for (i = 0; i < GIC_WAIT_LIMIT; i++) {
		if ((reg_rd32((uintptr_t)gicr +
			      GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) == 0u) {
			break;
		}
	}
	if (i == GIC_WAIT_LIMIT) {
		board_early_print("gicv3: redistributor never woke\n");
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
		board_early_print("gicv3: ICC_SRE will not set\n");
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
	/* Frame ownership is resolved per core, by that core, at its own GIC
	 * bring-up (gicr_probe_self) - see that function for why a boot-time
	 * sweep cannot see parked secondaries' frames. Before this runs,
	 * gicr_core_base()'s fallback constant covers the boot core's
	 * pre-init window (see irq_ensure_ready). The raw sweep below is
	 * bring-up evidence: it shows what each frame reports while the
	 * secondaries are still parked. */
	gicr_probe_self("boot");
	{
		/* Raw sweep of the four candidate frames while the
		 * secondaries are parked - the evidence that pinned the frame
		 * layout (TYPER's affinity half reads zero on this part; see
		 * gicr_probe_self). */
		uint32_t f;

		for (f = 0u; f < BOARD_SMP_CORES; f++) {
			uint64_t typer = reg_rd64(
				(uintptr_t)BOARD_GICR_BASE +
				(uintptr_t)f * BOARD_GICR_STRIDE + GICR_TYPER);

			board_log("gicv3: frame%u typer %08x%08x",
				  (unsigned)f,
				  (unsigned)(typer >> 32), (unsigned)typer);
		}
	}

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

/* Bring the controller up on first use.
 *
 * Why this is here and not left to the caller: the IRQ_* API is used by device
 * drivers, and a driver's own bring-up order is not something the interrupt
 * controller should dictate. Concretely, the console driver enables its RX
 * interrupt from PowerControl(ARM_POWER_FULL), which the boot path calls before
 * IRQ_Initialize() - with a NULL redistributor base, the enable write went to
 * address 0x10100 and the system stopped before printing anything. Nothing
 * about "the GIC must be initialised first" is discoverable from the driver's
 * point of view.
 *
 * Making IRQ_* total removes that ordering trap for every future driver, and
 * costs an already-true branch on the paths that run per interrupt.
 * IRQ_Initialize() remains as the explicit entry point and stays idempotent. */
static void irq_ensure_ready(void)
{
	if (!gic_ready) {
		board_gicv3_init();
	}
}

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
	irq_ensure_ready();
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
	irq_ensure_ready();
	uint32_t word;
	uint32_t bit;
	/* SGI/PPI registers are per-core: the enable lands in the CALLING
	 * core's redistributor frame, which is what "enable this interrupt on
	 * me" means for an SGI or PPI. */
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn < 0) {
		return -1;
	}

	if (irqn >= IRQ_INTID_LPI_FIRST) {
		/* An LPI has no enable bit in a GICR_ISENABLER register: its
		 * enabled state lives in the ITS property table. Same hook
		 * shape as the embox kernel: enabling an LPI-bound INTID
		 * writes the property entry (flushed to memory - the ITS
		 * reads it over a non-coherent port) and INVALIDATEs the
		 * ITS's cached copy. Before the ITS is up this is a no-op
		 * that still reports success for in-window INTIDs, which
		 * matches when a caller can legitimately arm early. */
		if (irqn < (IRQn_ID_t)(IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE)) {
			gic_lpi_set_state((uint32_t)irqn, 1);
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
		reg_wr32((uintptr_t)gicr + GICR_IGROUPR0,
			  reg_rd32((uintptr_t)gicr + GICR_IGROUPR0) |
			  (1u << bit));
		reg_wr32((uintptr_t)gicr + GICR_IGRPMODR0,
			  reg_rd32((uintptr_t)gicr + GICR_IGRPMODR0) &
			  ~(1u << bit));

		reg_wr32((uintptr_t)gicr + GICR_ISENABLER0,
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
	irq_ensure_ready();
	uint32_t word;
	uint32_t bit;
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn >= IRQ_INTID_LPI_FIRST) {
		/* Mirror of IRQ_Enable: an LPI's enabled state is the ITS
		 * property table's, not a GICR_ICENABLER bit. */
		if (irqn < (IRQn_ID_t)(IRQ_INTID_LPI_FIRST + IRQ_LPI_TABLE_SIZE)) {
			gic_lpi_set_state((uint32_t)irqn, 0);
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
		reg_wr32((uintptr_t)gicr + GICR_ICENABLER0,
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
	irq_ensure_ready();
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr + GICR_ISPENDR0,
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
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return 0u;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		return (reg_rd32((uintptr_t)gicr + GICR_ISPENDR0) &
			(1u << ((uint32_t)irqn % 32u))) ? 1u : 0u;
	}
	return (reg_rd32((uintptr_t)gicd +
				 GICD_ISPENDR((uint32_t)irqn / 32u)) &
		(1u << ((uint32_t)irqn % 32u))) ? 1u : 0u;
}

int32_t IRQ_ClearPending(IRQn_ID_t irqn)
{
	irq_ensure_ready();
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return -1;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		reg_wr32((uintptr_t)gicr + GICR_ICPENDR0,
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
	irq_ensure_ready();
	volatile uint32_t *gicr = gicr_core_base();

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
		reg_wr8((uintptr_t)gicr + GICR_IPRIORITYR0 +
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
	volatile uint32_t *gicr = gicr_core_base();

	if (irqn < 0 || irqn > (IRQn_ID_t)IRQ_INTID_SPI_MAX) {
		return IRQ_PRIORITY_ERROR;
	}
	if (irqn <= IRQ_INTID_SGI_PPI_MAX) {
		return reg_rd8((uintptr_t)gicr + GICR_IPRIORITYR0 +
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

/* --- SMP ------------------------------------------------------------------ */

/* Per-core GIC bring-up, running ON the secondary core. Same order and same
 * discipline as the boot core's board_gicv3_init(), minus everything that is
 * global state: the distributor was configured once, while the secondaries
 * were still parked, and is nobody else's to touch.
 *
 * The redistributor step is the WAKER handshake ONLY (see the long warning
 * inside gic_redistributor_init: this bank is shared with firmware's secure
 * lines, so no bank-wide writes, ever). The CPU interface registers
 * (ICC_*) are banked per core by the architecture, so the same sequence the
 * boot core ran is exactly right here.
 *
 * The yield SGI is armed through the ordinary IRQ_* API: IRQ_Enable takes
 * the single-bit read-modify-write path (IGROUPR0 set, IGRPMODR0 cleared,
 * ISENABLER0 set) against THIS core's frame via gicr_core_base(). */
void board_gicv3_secondary_init(void)
{
	/* Resolve THIS core's redistributor frame first: the frame is live
	 * now that the core is out of its park, and everything below keys
	 * off gicr_core_base(). */
	gicr_probe_self("secondary");

	gic_cpu_interface_sre_enable();
	gic_redistributor_init();
	gic_cpu_interface_init();

	IRQ_SetPriority((IRQn_ID_t)BOARD_SMP_YIELD_INTID,
			BOARD_IRQ_PRIORITY_SGI_RAW);
	IRQ_Enable((IRQn_ID_t)BOARD_SMP_YIELD_INTID);
}

/* Raise a software interrupt on the cores in core_mask (bit i = logical
 * core i). This is the kernel port's cross-core yield path.
 *
 * ICC_SGI1R_EL1 (S3_0_C12_C11_5), system-register only - a GICv3 has no
 * MMIO SGI register to write. Field layout per ARM IHI 0069:
 *
 *   TargetList [3:0]   Aff2 [7:4]   RS [15:12]   Aff1 [23:16]   Aff3 [39:32]
 *   IRM [40] (1 = all PEs, 0 = target list)
 *
 * On THIS board the logical core number lives in MPIDR AFFINITY 1, not
 * Aff0 - measured MPIDR_EL1 = 0x8100_0N00 (board_conf.h,
 * BOARD_MPIDR_CORE_SHIFT). An earlier encoding of this function put the
 * core mask into TargetList alone, which addresses Aff0 - and every core
 * here has Aff0 = 0 - so an SGI aimed at cores 1..3 targeted non-existent
 * PEs and the GIC dropped it SILENTLY: no fault, no status bit, the yield
 * just never arrived (the reference SMP line lost days to the same class
 * of bug, in its case Aff1 written into the Aff3 field). The encoding that
 * works: one write per target core, Aff1 = core at bits [23:16],
 * TargetList = 1 (bit 0 = the single core at that Aff1 value). IRM = 0.
 *
 * The DSB is what makes this correct, not decoration: the yield is often
 * sent because this core just made scheduler state (unblocked a task,
 * changed a ready list) that the target core is about to consume - a dsb
 * orders those writes, it does not make them visible, but together with the
 * inner-shareable coherency of the cluster it is the required barrier
 * before the IPI can be taken. */
void board_gicv3_send_sgi(uint32_t intid, uint32_t core_mask)
{
	uint64_t sgi1r = ((uint64_t)(intid & 0x0fu) << 24);

	__asm__ __volatile__("dsb sy" ::: "memory");

	for (uint32_t core = 0u; core < 16u && core_mask != 0u; core++) {
		if ((core_mask & (1u << core)) == 0u) {
			continue;
		}

		/* S3_0_C12_C11_5 is ICC_SGI1R_EL1 per ARM IHI 0069 (the
		 * board-validated standalone reference encodes the same).
		 * An earlier encoding here used _6 - ICC_ASGI1R, a different
		 * register with a different targeting semantic. */
		__asm__ __volatile__("msr s3_0_c12_c11_5, %0"
				     ::"r"(sgi1r | 0x1ull |
					   ((uint64_t)(core & 0xffu) << 16)));
	}

	__asm__ __volatile__("isb" ::: "memory");
}

/* Run-time interrupt-path diagnostics, for the shell's gicdiag command.
 * Read-only, on the CALLING core: PMR is this core's priority mask (a value
 * parked below the console priority starves exactly those lines while tick
 * and SGI keep delivering - the "anchors green, shell deaf" split) and RPR
 * is the running priority (non-idle means the GIC still sees a claimed
 * interrupt on this core).
 *
 * ICC_RPR_EL1 is S3_0_C12_C11_3. NEVER touch S3_0_C12_C8_0 in a diagnostic
 * like this - that is ICC_IAR0_EL1, and reading it at EL1 in this
 * configuration resets through EL3 (the D33 lesson). */
void board_gicv3_diag(uint32_t *pmr, uint32_t *rpr)
{
	uint32_t v;

	__asm__ __volatile__("mrs %0, s3_0_c12_c11_3" : "=r"(v));
	if (rpr != NULL) {
		*rpr = v & 0xffu;
	}
	if (pmr != NULL) {
		*pmr = IRQ_GetPriorityMask();
	}
}
