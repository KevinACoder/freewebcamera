/*
 * @file   gdb_arch.c
 * @brief  AArch64 (EL1) debug-state side of the serial GDB stub.
 *
 * Register-file layout and the debug-register sequences follow FreeBSD
 * sys/arm64/arm64/gdb_machdep.c and debug_monitor.c (BSD-2, FreeBSD
 * Foundation / Semihalf); the ESR decode and trapframe contract follow the
 * lab's fgdb port (fgdb_arch.c, fgdb_exception.S). All debug resources are
 * the hardware kind: Z0 breakpoints arm DBGBCR/BVR slots, software
 * patching is never used.
 *
 * FP/SIMD context does not exist in this image (-mgeneral-regs-only): the
 * target description served to the host (gdb_main.c's target_xml) declares
 * only the 34 core registers, so the FP bank is never named at all.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "gdb.h"
#include "gdb_arch.h"
#include "gdb_int.h"

/* --- debug register access (fixed immediates; aarch64 has no indexed
 *     debug-register addressing, so the slot number is a switch case) --- */

#define MAX_BP		8U

static unsigned long bp_addr[MAX_BP];
static unsigned int bp_enabled;
static unsigned int bp_count = MAX_BP;
static int arch_ready;

/* The trapframe being debugged; gdb_trap_loop's register commands read and
 * write through it. Owned by gdb_debug_interrupt below. */
static struct gdb_trapframe *frame_current;

static unsigned long mdscr_read(void)
{
	unsigned long v;

	__asm__ __volatile__("mrs %0, mdscr_el1" : "=r"(v));
	return v;
}

static void mdscr_write(unsigned long v)
{
	__asm__ __volatile__("msr mdscr_el1, %0" :: "r"(v));
}

static unsigned long dfr_read(void)
{
	unsigned long v;

	__asm__ __volatile__("mrs %0, id_aa64dfr0_el1" : "=r"(v));
	return v;
}

static void dbgbvr_write(unsigned int n, unsigned long v)
{
	switch (n) {
	case 0: __asm__ __volatile__("msr dbgbvr0_el1, %0" :: "r"(v)); break;
	case 1: __asm__ __volatile__("msr dbgbvr1_el1, %0" :: "r"(v)); break;
	case 2: __asm__ __volatile__("msr dbgbvr2_el1, %0" :: "r"(v)); break;
	case 3: __asm__ __volatile__("msr dbgbvr3_el1, %0" :: "r"(v)); break;
	case 4: __asm__ __volatile__("msr dbgbvr4_el1, %0" :: "r"(v)); break;
	case 5: __asm__ __volatile__("msr dbgbvr5_el1, %0" :: "r"(v)); break;
	case 6: __asm__ __volatile__("msr dbgbvr6_el1, %0" :: "r"(v)); break;
	case 7: __asm__ __volatile__("msr dbgbvr7_el1, %0" :: "r"(v)); break;
	default: break;
	}
}

static void dbgbcr_write(unsigned int n, unsigned long v)
{
	switch (n) {
	case 0: __asm__ __volatile__("msr dbgbcr0_el1, %0" :: "r"(v)); break;
	case 1: __asm__ __volatile__("msr dbgbcr1_el1, %0" :: "r"(v)); break;
	case 2: __asm__ __volatile__("msr dbgbcr2_el1, %0" :: "r"(v)); break;
	case 3: __asm__ __volatile__("msr dbgbcr3_el1, %0" :: "r"(v)); break;
	case 4: __asm__ __volatile__("msr dbgbcr4_el1, %0" :: "r"(v)); break;
	case 5: __asm__ __volatile__("msr dbgbcr5_el1, %0" :: "r"(v)); break;
	case 6: __asm__ __volatile__("msr dbgbcr6_el1, %0" :: "r"(v)); break;
	case 7: __asm__ __volatile__("msr dbgbcr7_el1, %0" :: "r"(v)); break;
	default: break;
	}
}

/* Push the shadow slots into the hardware and manage MDSCR.MDE. */
static void dbg_sync(void)
{
	unsigned long mdscr = mdscr_read();
	unsigned int i;

	for (i = 0U; i < bp_count; i++) {
		dbgbvr_write(i, bp_addr[i]);
		if (i < bp_enabled) {
			/* E=1, PMC=any(0b11), BAS=full word (0b1111),
			 * unlinked address match. 4-byte breakpoints. */
			dbgbcr_write(i, 0x1UL | (0x3UL << 1) |
					     (0xfUL << 5));
		} else {
			dbgbcr_write(i, 0UL);
		}
	}
	if (bp_enabled != 0U) {
		mdscr |= (1UL << 15);	/* MDE: enable hw bp/watch */
	} else {
		mdscr &= ~(1UL << 15);
	}
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

void gdb_arch_setup(void)
{
	unsigned long dfr;
	unsigned long probe = 0xdeadbeefUL;

	/* Clear the OS lock (a bootloader/EL3 may leave it set; a set lock
	 * makes every debug register access trap). */
	__asm__ __volatile__("msr oslar_el1, %0" :: "r"(0UL));

	/* One line of boot evidence: can EL1 HOLD debug-register state on
	 * this platform? On RK3568 with the stock BL31 the DBGBCR/BVR/MDSCR
	 * writes silently do nothing - which is exactly why breakpoints are
	 * software-patched BRKs here and not DBGBCR slots (see
	 * gdb_cpu_set_hwbp). */
	__asm__ __volatile__("msr dbgbvr0_el1, %0" :: "r"(probe));
	__asm__ __volatile__("mrs %0, dbgbvr0_el1" : "=r"(probe));
	board_log("gdb: dbgbvr0 readback=%lx (expect deadbeef)", probe);

	/* Breakpoint count: BRPs field is bits [15:12], +1, clamped. */
	dfr = dfr_read();
	bp_count = (unsigned int)((dfr >> 12) & 0xfUL) + 1U;
	if (bp_count > MAX_BP) {
		bp_count = MAX_BP;
	}
	bp_enabled = 0U;
	arch_ready = 1;
}

/* --- exception classification --------------------------------------------- */

int gdb_cpu_signal(const struct gdb_trapframe *tf)
{
	unsigned long ec = (tf->esr >> 26) & 0x3fUL;

	switch (ec) {
	/* breakpoint (lower/current EL), catch, software step,
	 * watchpoint, BRK */
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
	case 0x3c:
		return 5;	/* SIGTRAP */
	/* instruction abort, data abort */
	case 0x20: case 0x21: case 0x24: case 0x25:
		return 11;	/* SIGSEGV */
	/* PC alignment, SP alignment, SError */
	case 0x22: case 0x26: case 0x2f:
		return 7;	/* SIGBUS */
	default:
		return 4;	/* SIGILL - unknown/unallocated EC among them */
	}
}

static const char *stop_reason(const struct gdb_trapframe *tf)
{
	unsigned long ec = (tf->esr >> 26) & 0x3fUL;

	/* Breakpoints are the patched-BRK kind here (see the breakpoint
	 * section), so a hit arrives as EC=0x3c with the stub's immediate. */
	if (ec == 0x3cUL) {
		return "swbreak;";
	}
	return 0;
}

/* The UP carrier's single trap entry: EVERY synchronous exception comes
 * here - debug exceptions become plain stops, real faults become stops
 * too (signal SEGV/BUS/ILL by EC class), which replaces the fatal park:
 * the host inspects the live scene instead of reading a one-shot dump.
 * Continue on a fault re-executes the faulting instruction (the frame's
 * elr is untouched), so the fault simply comes back into the stub - under
 * the host's control, which is the point. */
void gdb_debug_interrupt(struct gdb_trapframe *tf)
{
	unsigned long ec = (tf->esr >> 26) & 0x3fUL;
	unsigned long iss = tf->esr & 0x1ffffffUL;
	int signal;

	if (!gdb_dbgport_ready()) {
		/* No transport registered yet (fault before tx_gdb_init):
		 * nothing to talk to - report through the polled UART and
		 * park, exactly like the old fatal path. */
		board_early_print_raw("fatal: sync exception before gdb dbgport ready\n");
		for (;;) {
			__asm__ __volatile__("wfe");
		}
	}

	if (arch_ready == 0) {
		gdb_arch_setup();
	}

	/* One line per trap on the polled UART: the serial ring becomes a
	 * stop log (ELR/EC per stop) without touching the TCP-side RSP
	 * framing - the host's packet reader skips leading non-'$' bytes. */
	{
		char line[80];

		snprintf(line, sizeof(line),
			 "gdb: trap EC=%02lx ELR=%lx\n", ec, tf->elr);
		board_early_print_raw(line);
	}

	/* Our own BRK #GDB_BRK_IMM: the exception is taken at the BRK's own
	 * address - resume past it, like the host expects of a software
	 * breakpoint. */
	if (ec == 0x3cUL && iss == (unsigned long)GDB_BRK_IMM) {
		tf->elr += 4UL;
	}

	signal = gdb_cpu_signal(tf);
	if (signal != 5) {
		char line[128];

		snprintf(line, sizeof(line),
			 "gdb: sync fault FAR=%lx - stub owns it\n", tf->far);
		board_early_print_raw(line);
	}

	frame_current = tf;
	gdb_session_active = 1;
	gdb_dbgport_intr_ctrl(1);

	gdb_trap_loop(tf, signal, stop_reason(tf));

	gdb_cpu_singlestep_clear();
	dbg_sync();
	gdb_dbgport_intr_ctrl(0);
	gdb_session_active = 0;

	/* The locked console sinks dropped output while the session held the
	 * wire; flush the accounting with the gate already open. */
	board_console_gate_report();
}

/* --- register file --------------------------------------------------------- */

static unsigned long tf_reg(const struct gdb_trapframe *tf, int regnum)
{
	if (regnum <= GDB_REG_LR) {
		if (regnum == GDB_REG_LR) {
			return tf->lr;
		}
		return tf->x[regnum];
	}
	switch (regnum) {
	case GDB_REG_SP:
		return tf->sp;
	case GDB_REG_PC:
		return tf->elr;
	case GDB_REG_CPSR:
		return tf->spsr & 0xffffffffUL;
	default:
		return 0UL;
	}
}

static int tf_set_reg(struct gdb_trapframe *tf, int regnum, unsigned long v)
{
	if (regnum <= GDB_REG_LR) {
		if (regnum == GDB_REG_LR) {
			tf->lr = v;
		} else {
			tf->x[regnum] = v;
		}
		return 0;
	}
	switch (regnum) {
	case GDB_REG_SP:
		tf->sp = v;
		return 0;
	case GDB_REG_PC:
		tf->elr = v;
		return 0;
	case GDB_REG_CPSR:
		tf->spsr = (tf->spsr & ~0xffffffffUL) | (v & 0xffffffffUL);
		return 0;
	default:
		return -1;
	}
}

unsigned long gdb_cpu_regval(int regnum)
{
	if (regnum < 0 || regnum >= GDB_NREGS) {
		return 0UL;
	}
	/* gdb_debug_interrupt passes the trapframe through gdb_trap_loop's
	 * caller; the frame pointer rides in the arch wrapper (see below). */
	return tf_reg(frame_current, regnum);
}

int gdb_cpu_setregval(int regnum, unsigned long v)
{
	if (regnum < 0 || regnum >= GDB_NREGS) {
		return -1;
	}
	return tf_set_reg(frame_current, regnum, v);
}

unsigned int gdb_cpu_getregs(char *buf, unsigned int bufsz)
{
	unsigned int o = 0U;
	unsigned char raw[8];
	int r;

	if (bufsz < 128U) {
		return 0U;
	}
	for (r = 0; r < GDB_NREGS; r++) {
		unsigned long v = tf_reg(frame_current, r);
		unsigned int width = (r == GDB_REG_CPSR) ? 4U : 8U;

		raw[0] = (unsigned char)(v);
		raw[1] = (unsigned char)(v >> 8);
		raw[2] = (unsigned char)(v >> 16);
		raw[3] = (unsigned char)(v >> 24);
		raw[4] = (unsigned char)(v >> 32);
		raw[5] = (unsigned char)(v >> 40);
		raw[6] = (unsigned char)(v >> 48);
		raw[7] = (unsigned char)(v >> 56);
		mem2hex(raw, &buf[o], width);
		o += width * 2U;
	}
	buf[o] = '\0';
	return o;
}

unsigned int gdb_cpu_setregs(const char *hex)
{
	unsigned char raw[8];
	int r;

	for (r = 0; r < 34; r++) {
		unsigned int width = (r == GDB_REG_CPSR) ? 4U : 8U;
		unsigned long v;

		if (hex2mem(hex, raw, width) != width) {
			return 1U;
		}
		v = (unsigned long)raw[0] |
		    ((unsigned long)raw[1] << 8) |
		    ((unsigned long)raw[2] << 16) |
		    ((unsigned long)raw[3] << 24) |
		    ((unsigned long)raw[4] << 32) |
		    ((unsigned long)raw[5] << 40) |
		    ((unsigned long)raw[6] << 48) |
		    ((unsigned long)raw[7] << 56);
		(void)tf_set_reg(frame_current, r, v);
		hex += width * 2U;
	}
	return 0U;
}

/* --- breakpoints and single step ------------------------------------------- *
 *
 * Z0/Z1 are SOFTWARE breakpoints here: the classic DBGBCR/BVR slot path
 * exists (dbg_sync) but this platform does not HOLD debug-register state
 * written from EL1 - the dbgbvr0 readback at setup proves it every boot -
 * so hardware breakpoints never fire and software step is dead with them.
 * Patching the reserved BRK #GDB_BRK_IMM into RAM text works everywhere,
 * needs only the cache maintenance the memory-write path already performs
 * (board_sync_written), and reports as "swbreak", which the host opted
 * into via qSupported. The image is fully RAM-resident, so there is no
 * read-only-text corner. */

#define BRK_INSN(imm16)	(0xD4200000UL | ((unsigned long)(imm16) << 5))

/* The ARM ARM's self-modifying-code recipe, exactly: DC CVAU, DSB, IC
 * IVAU, DSB, ISB. The earlier attempt delegated to the generic
 * flush-invalidate + "ic iallu" pair and the fetch kept serving the old
 * word - the patch landed in memory (readback proved it) but never on the
 * fetch path, so the barrier structure, not the ops, was the bug. */
static void swbp_sync(unsigned long addr)
{
	__asm__ __volatile__("dc cvau, %0" :: "r"(addr) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
	__asm__ __volatile__("ic ivau, %0" :: "r"(addr) : "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
	__asm__ __volatile__("isb" ::: "memory");
}

static unsigned int bp_orig[MAX_BP];

int gdb_cpu_set_hwbp(unsigned long addr)
{
	unsigned int i;

	if (bp_enabled >= bp_count) {
		return -1;
	}
	/* Replace if already present. */
	for (i = 0U; i < bp_enabled; i++) {
		if (bp_addr[i] == addr) {
			return 0;
		}
	}
	bp_orig[bp_enabled] = *(volatile unsigned int *)addr;
	*(volatile unsigned int *)addr = (unsigned int)BRK_INSN(GDB_BRK_IMM);
	swbp_sync(addr);
	bp_addr[bp_enabled++] = addr;
	return 0;
}

int gdb_cpu_clr_hwbp(unsigned long addr)
{
	unsigned int i, j;

	for (i = 0U; i < bp_enabled; i++) {
		if (bp_addr[i] == addr) {
			*(volatile unsigned int *)addr = bp_orig[i];
			swbp_sync(addr);
			for (j = i; j + 1U < bp_enabled; j++) {
				bp_addr[j] = bp_addr[j + 1U];
				bp_orig[j] = bp_orig[j + 1U];
			}
			bp_enabled--;
			return 0;
		}
	}
	return -1;
}

/* Disarm every breakpoint. Detach must leave the target running exactly
 * as it was before the session: a patched BRK survives detach and would
 * trap the next fetch through the old address with no host listening -
 * the world freezes in a silent trap loop. */
void gdb_cpu_breakpoints_disarm(void)
{
	unsigned int i;

	for (i = 0U; i < bp_enabled; i++) {
		*(volatile unsigned int *)bp_addr[i] = bp_orig[i];
		swbp_sync(bp_addr[i]);
	}
	bp_enabled = 0U;
}

void gdb_cpu_singlestep_set(struct gdb_trapframe *tf)
{
	unsigned long mdscr = mdscr_read();

	/* PSTATE.SS in the SPSR we will restore: the instruction after the
	 * ERET completes, then a SoftwareStep exception comes back. Masking
	 * A/I/F keeps the step quiet; D is left alone so the step exception
	 * can reach us. */
	tf->spsr |= (1UL << 21);
	tf->spsr |= 0x3c0UL;		/* mask A|I|F for the stepped instr */
	mdscr |= 1UL;			/* SS */
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

void gdb_cpu_singlestep_clear(void)
{
	unsigned long mdscr = mdscr_read();

	mdscr &= ~1UL;			/* SS */
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

/* Host-written memory needs the I-cache line dropped before the CPU will
 * fetch it (software-injected instructions, patched data read by the
 * debugger's own path). */
void board_sync_written(unsigned long addr, unsigned long len)
{
	board_dcache_flush_invalidate(addr, len);
	__asm__ __volatile__(
		"ic	iallu\n"
		"dsb	nsh\n"
		"isb\n" ::: "memory");
}
