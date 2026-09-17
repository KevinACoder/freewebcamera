/*
 * @file   smp_secondary.S
 * @brief  Secondary-core entry, reached through PSCI CPU_ON.
 *
 * BL31 drops a released core here with interrupts masked and the MMU and
 * caches off, most likely at EL2 (the PSCI entry state is the highest
 * implemented non-secure exception level). The descent to EL1 mirrors
 * startup.S exactly - same EL2 sequence, same SCTLR discipline, same
 * CNTHCTL handling - because the two paths face identical firmware state and
 * the reasons for every step are documented there, not repeated here.
 *
 * Two things differ from the boot path:
 *
 *  - The boot stack is SLICED. The link script reserves one 64 KiB region
 *    under __stack_top; core 0 keeps the top slice and each logical core
 *    takes the 16 KiB slice below it (sp = __stack_top - core * 0x4000,
 *    core = MPIDR Aff1 - this SoC numbers its cores in Aff1). The boot
 *    stack is transient - after the scheduler starts, every task runs on
 *    its own stack - so a transient 16 KiB budget per core is comfortably
 *    above what the descent path uses.
 *
 *  - BSS is NOT cleared. Core 0 owns that; this core only consumes memory
 *    core 0 already initialised. A secondary that ever ran the boot path
 *    would zero live kernel state - startup.S parks any Aff0 != 0 core for
 *    exactly that reason.
 *
 * Hand-off after the MMU is on goes to the kernel port's uxPortSecondaryMain
 * (GIC bring-up, report-in, wait for the scheduler, first context restore).
 * It lives on the kernel side of the seam because what it does is enter the
 * scheduler - the mirror image of startup.S calling board_main for the boot
 * core. Until that call, everything here is board bring-up identical in kind
 * to the boot path.
 */

	.arch	armv8-a
	.text

	.globl	freertos_secondary_entry
	.type	freertos_secondary_entry, %function
freertos_secondary_entry:
	/* Mask everything until EL1 is set up. */
	msr	daifset, #0xf

	mrs	x0, CurrentEL
	and	x0, x0, #0xc
	cmp	x0, #0xc			/* EL3 */
	b.eq	sec_from_el3
	cmp	x0, #0x8			/* EL2 */
	b.eq	sec_from_el2
	b	sec_el1_entry			/* already EL1 */

sec_from_el3:
	mov	x0, #0
	msr	sctlr_el2, x0
	msr	hcr_el2, xzr
	mov	x0, #0x30			/* SCR_EL3: NS | IRQ | FIQ */
	msr	scr_el3, x0
	adr	x1, sec_from_el2
	msr	elr_el3, x1
	mov	x0, #0x3c9			/* SPSR_EL3: EL2h, DAIF masked */
	msr	spsr_el3, x0
	isb
	eret

sec_from_el2:
	ldr	x0, =__stack_top
	mov	sp, x0

	dsb	sy
	msr	sctlr_el2, xzr
	isb

	mov	x0, #0x80000000			/* HCR_EL2.RW = 1 */
	msr	hcr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3			/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x0
	msr	cptr_el2, xzr			/* no FP/SIMD trapping */
	msr	hstr_el2, xzr
	msr	vttbr_el2, xzr

	/* Deliberately NO ICC_SRE_EL2 write here, although a bare-metal port
	 * might expect one (the boot core's firmware, U-Boot, sets it before
	 * `go`). Measured on this board: writing ICC_SRE_EL2 from a
	 * PSCI-released core wedges it silently - with ICC_SRE_EL3.Enable=0
	 * the access traps into EL3 and OP-TEE's unexpected-trap path eats
	 * the core without a word. The firmware that releases the core also
	 * configures the system-register interface for it, which is why the
	 * secondaries sail through their EL1 GIC bring-up once released. */

	adr	x1, sec_el1_entry
	msr	elr_el2, x1
	mov	x0, #0x3c5			/* SPSR_EL2: EL1h, DAIF masked */
	msr	spsr_el2, x0
	isb
	eret

sec_el1_entry:
	msr	spsel, #1
	msr	sctlr_el1, xzr
	ic	iallu
	dsb	sy
	isb

	mrs	x0, cpacr_el1
	orr	x0, x0, #(3 << 20)
	msr	cpacr_el1, x0

	/* Pre-scheduler fault vectors: same table as the boot core, whose
	 * entries print the exception kind and park. */
	adr	x0, boot_vectors
	msr	vbar_el1, x0
	isb

	/* This core's slice of the boot stack (see the file comment). The
	 * slice index is the LOGICAL core number: on this SoC that is
	 * MPIDR Aff1 (Aff0 is 0 for every core - measured). */
	mrs	x1, mpidr_el1
	lsr	x1, x1, #8
	and	x1, x1, #0xff
	ldr	x0, =__stack_top
	mov	x2, #0x4000			/* 64 KiB / BOARD_SMP_CORES */
	mul	x2, x1, x2
	sub	sp, x0, x2

	/* MMU on, bound to the SAME page tables the boot core built. Must be
	 * the secondary variant: rebuilding the tables here would pull valid
	 * descriptors out from under the running boot core. */
	bl	board_mmu_enable_secondary

	/* Stage marker: MMU and stacks are live, C is next. MUST sit after
	 * the SP setup - a marker call before SP is configured pushes to
	 * whatever SP the firmware handoff left behind (round-12 lesson). */
	bl	uart_early_puts_marker_a

	/* Kernel port: own-GIC bring-up, report in, wait, first context
	 * restore. Does not return.
	 *
	 * NOTE: the whole secondary path up to the scheduler is SILENT on
	 * purpose. Console output here overlaps OP-TEE's own I/TC release
	 * messages on the same UART, and that concurrency wedged the
	 * transmitter mid-line during bring-up (rounds 6-10: output stopped
	 * half-sentence, entire system mute). A secondary's proof of life is
	 * its report-in flag, not a print. */
	bl	uxPortSecondaryMain

1:	wfe
	b	1b
	.size	freertos_secondary_entry, . - freertos_secondary_entry

	.global uart_early_puts_marker_a
uart_early_puts_marker_a:
	adr	x0, mark_a
	b	uart_early_puts

	.section .rodata, "a"
mark_a:	.asciz	"[descent-a]\r\n"
