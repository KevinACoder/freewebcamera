/*
 * @file   board_conf.h
 * @brief  RK3568 board calibration: every register base, interrupt number
 *         and hardware-behaviour constant in one place.
 *
 * This header is selected by the build (Makefile puts port/board/<board> on
 * the include path ahead of this file's consumers) and is included by
 * port/board/board.h. It carries DATA ONLY - no declarations, no inline
 * functions - so that assembly files can include it too (.S files run the
 * preprocessor, but cannot digest function bodies).
 *
 * Every value here was established by experiment on this board unless a
 * comment says otherwise; where a wrong value fails silently rather than
 * loudly, the failure mode is recorded next to the value.
 */

#ifndef FREEWEBCAMERA_RK3568_BOARD_CONF_H
#define FREEWEBCAMERA_RK3568_BOARD_CONF_H

/* --- register bases ------------------------------------------------------- */

#define BOARD_UART2_BASE	0xfe660000UL	/* console UART2 */
#define BOARD_GICD_BASE		0xfd400000UL
#define BOARD_GICR_BASE		0xfd460000UL	/* core 0 redistributor */
#define BOARD_ITS_BASE		0xfd440000UL
#define BOARD_ITS_TRANSLATER	0xfd450040UL

/* Console UART calibration (DW-APB 16550-compatible: 32-bit access, register
 * byte-offset stride 4 - byte accesses read and write garbage). */
#define BOARD_UART_BASE			BOARD_UART2_BASE
#define BOARD_UART_CLOCK_HZ		24000000UL
#define BOARD_UART_BAUD			115200UL

/* --- interrupts ------------------------------------------------------------ */

/* The GIC INTID of the console UART's interrupt.
 *
 * 150, from three independent sources that agree: the device trees (uart2's
 * node carries GIC_SPI 118, and uart1..uart9 map to SPI 117..125, so the
 * sequence is self-consistent), and the vendor SoC header for this board,
 * whose FUART2_IRQ_NUM is 150 against the same base address 0xfe660000.
 * SPI N is INTID N+32, hence 150.
 *
 * The lab's FreeBSD logs report `irq 66` for the same base address; that is
 * FreeBSD's own interrupt-rack numbering, not the GIC INTID. A brief earlier
 * attempt at 66 delivered nothing, and an interrupt storm seen with 150 was
 * traced to the driver's own handler not clearing the source - not to the
 * number. */
#define BOARD_CONSOLE_INTID	150U

/* Tick source: the EL1 virtual timer (CNTV), INTID 27.
 *
 * Not the physical timer, despite that being the obvious choice. On this
 * board OP-TEE claims the physical timer as a Group 0 interrupt that
 * non-secure code cannot enable, and the EL2 physical timer is unreachable
 * because the `go` boot path leaves CNTHCTL_EL2.EL1PCEN=0. The virtual timer
 * is the one that works, measured: writing CNTV_TVAL raises GICR_ISPENDR0
 * bit 27 and INTID 27 arrives. */
#define BOARD_TICK_INTID	27U

/* GIC-600 redistributor frame stride. A redistributor is a pair of 64KiB
 * frames (RD_base + the paired vLPI frame), so consecutive cores' RD_base
 * frames are 2 * 64KiB apart. Walking the frames and matching GICR_TYPER's
 * affinity value against MPIDR is the only sound way to find a core's own
 * redistributor; the boot core's frame happens to sit at BOARD_GICR_BASE. */
#define BOARD_GICR_STRIDE	0x20000UL

/* --- SMP: core numbering and PSCI ------------------------------------------ */

/* HOW A CORE'S LOGICAL NUMBER IS EXTRACTED FROM MPIDR_EL1.
 *
 * MEASURED (SMP bring-up round 6): a released secondary reads MPIDR_EL1 =
 * 0x8100_0N00 with N = 0..3 - the logical core number lives in AFFINITY 1,
 * and Aff0 is 0 for every core. (Bits 31 and 24 are the usual RES1/U.)
 * Aff0-based extraction made every secondary believe it was core 0, which is
 * what rounds 2-5 thrashed on: core-0's redistributor frame and boot stack
 * shared by four cores. */
#define BOARD_MPIDR_CORE_SHIFT	8

/* PSCI conduit instruction, executed at non-secure EL1. BL31/OP-TEE are
 * resident on this board (they own PPI 29 as a Group 0 interrupt), so the
 * SMC conduit is the board-proven path - the same one mainline Linux uses
 * here (enable-method = "psci"). */
#define BOARD_PSCI_INSN		"smc #0"

/* PSCI CPU_ON target encoding. Measured: the LINEAR core index works -
 * OP-TEE maps the low byte to its own core table and releases the core
 * whose hardware identity is Aff1 = index (MPIDRs here are 0x8100_0N00).
 * Passing an MPIDR-shaped value instead does NOT reach the right core. */
#define BOARD_PSCI_CPU_ON_TARGET(n)	((uint64_t)(n))

/* --- MMU layout (consumed by common/mmu.c) --------------------------------- */

/* Image RAM base: below this is firmware and must stay Device (never cached,
 * never speculated, never executed). */
#define BOARD_MMU_IMAGE_RAM_BASE	0x0a000000ULL

/* Normal-cacheable window in the top 1GiB. The ITS LPI property/pending
 * tables were once placed here; they now live in the image's .bss, but the
 * window is kept - removing it would change the boot-core mapping for no
 * measured benefit. */
#define BOARD_MMU_LPI_WINDOW_BASE	0xc0000000ULL
#define BOARD_MMU_LPI_WINDOW_END	0xc2000000ULL

/* DesignWare PCIe register files (DBI), one per controller, both in the same
 * 1GiB at 15GiB. Defining BOARD_MMU_PCIE_DBI0_BASE also switches on the L2
 * table that maps them (and nothing else in that 1GiB). */
#define BOARD_MMU_PCIE_DBI0_BASE	0x3c0000000ULL
#define BOARD_MMU_PCIE_DBI0_END		(BOARD_MMU_PCIE_DBI0_BASE + 0x400000ULL)
#define BOARD_MMU_PCIE_DBI1_BASE	0x3c0800000ULL
#define BOARD_MMU_PCIE_DBI1_END		(BOARD_MMU_PCIE_DBI1_BASE + 0x400000ULL)

/* TCR_EL1 physical address size: PS=0b010 selects 40-bit PA, which is what
 * this SoC implements (a larger IPS than the hardware supports is
 * UNPREDICTABLE). */
#define BOARD_MMU_TCR_PS		(2ULL << 32)

#endif /* FREEWEBCAMERA_RK3568_BOARD_CONF_H */
