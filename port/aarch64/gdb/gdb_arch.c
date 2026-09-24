/*
 * @file   gdb_arch.c
 * @brief  AArch64 (EL1) debug-state side of the serial GDB stub.
 *
 * Register-file layout and the debug-register sequences follow FreeBSD
 * sys/arm64/arm64/gdb_machdep.c and debug_monitor.c (BSD-2, FreeBSD
 * Foundation / Semihalf); the ESR decode and trapframe contract follow the
 * lab's standalone GDB port. Z0 patches the reserved BRK #GDB_BRK_IMM into
 * RAM text; Z1 arms DBGBCR/BVR slots and Z2-4 arm DBGWVR/WCR slots.
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
#define MAX_WP		8U

/* FreeBSD debug_monitor.c's breakpoint control word (non-VHE): BAS=0b1111
 * (four bytes), PMC=0b01 (EL1 and above), E=1; HMC/SSC/LBN/BT all zero -
 * plain unlinked address match. */
#define DBG_BCR_ENABLE_WORD	(((unsigned long)0xf << 5) | \
				 ((unsigned long)0x1 << 1) | 0x1UL)

/* MDSCR_EL1 bits (FreeBSD armreg.h): software step, kernel debug enable,
 * monitor debug enable. Breakpoint/watchpoint events are gated by MDE,
 * software step additionally needs KDE - both originals set them together. */
#define MDSCR_SS	(1UL << 0)
#define MDSCR_KDE	(1UL << 13)
#define MDSCR_MDE	(1UL << 15)

static unsigned long bp_addr[MAX_BP];
static unsigned int bp_kind[MAX_BP];	/* 0 = BRK patch, 1 = DBGBCR slot */
static unsigned int bp_enabled;
static unsigned int bp_count = MAX_BP;
static unsigned long wp_addr[MAX_WP];
static unsigned long wp_ctrl[MAX_WP];	/* LSC differs per watchpoint */
static unsigned int wp_enabled;
static unsigned int wp_count = MAX_WP;
static int arch_ready;
static int step_armed;
static unsigned long step_saved_daif;

extern unsigned long gdb_boot_entry_el;
extern unsigned long gdb_boot_mdcr_el2;

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

static unsigned long dbgbvr_read(unsigned int n)
{
	unsigned long v = 0UL;

	switch (n) {
	case 0: __asm__ __volatile__("mrs %0, dbgbvr0_el1" : "=r"(v)); break;
	case 1: __asm__ __volatile__("mrs %0, dbgbvr1_el1" : "=r"(v)); break;
	case 2: __asm__ __volatile__("mrs %0, dbgbvr2_el1" : "=r"(v)); break;
	case 3: __asm__ __volatile__("mrs %0, dbgbvr3_el1" : "=r"(v)); break;
	case 4: __asm__ __volatile__("mrs %0, dbgbvr4_el1" : "=r"(v)); break;
	case 5: __asm__ __volatile__("mrs %0, dbgbvr5_el1" : "=r"(v)); break;
	case 6: __asm__ __volatile__("mrs %0, dbgbvr6_el1" : "=r"(v)); break;
	case 7: __asm__ __volatile__("mrs %0, dbgbvr7_el1" : "=r"(v)); break;
	default: break;
	}
	return v;
}

static unsigned long dbgbcr_read(unsigned int n)
{
	unsigned long v = 0UL;

	switch (n) {
	case 0: __asm__ __volatile__("mrs %0, dbgbcr0_el1" : "=r"(v)); break;
	case 1: __asm__ __volatile__("mrs %0, dbgbcr1_el1" : "=r"(v)); break;
	case 2: __asm__ __volatile__("mrs %0, dbgbcr2_el1" : "=r"(v)); break;
	case 3: __asm__ __volatile__("mrs %0, dbgbcr3_el1" : "=r"(v)); break;
	case 4: __asm__ __volatile__("mrs %0, dbgbcr4_el1" : "=r"(v)); break;
	case 5: __asm__ __volatile__("mrs %0, dbgbcr5_el1" : "=r"(v)); break;
	case 6: __asm__ __volatile__("mrs %0, dbgbcr6_el1" : "=r"(v)); break;
	case 7: __asm__ __volatile__("mrs %0, dbgbcr7_el1" : "=r"(v)); break;
	default: break;
	}
	return v;
}

static void dbgwvr_write(unsigned int n, unsigned long v)
{
	switch (n) {
	case 0: __asm__ __volatile__("msr dbgwvr0_el1, %0" :: "r"(v)); break;
	case 1: __asm__ __volatile__("msr dbgwvr1_el1, %0" :: "r"(v)); break;
	case 2: __asm__ __volatile__("msr dbgwvr2_el1, %0" :: "r"(v)); break;
	case 3: __asm__ __volatile__("msr dbgwvr3_el1, %0" :: "r"(v)); break;
	case 4: __asm__ __volatile__("msr dbgwvr4_el1, %0" :: "r"(v)); break;
	case 5: __asm__ __volatile__("msr dbgwvr5_el1, %0" :: "r"(v)); break;
	case 6: __asm__ __volatile__("msr dbgwvr6_el1, %0" :: "r"(v)); break;
	case 7: __asm__ __volatile__("msr dbgwvr7_el1, %0" :: "r"(v)); break;
	default: break;
	}
}

static void dbgwcr_write(unsigned int n, unsigned long v)
{
	switch (n) {
	case 0: __asm__ __volatile__("msr dbgwcr0_el1, %0" :: "r"(v)); break;
	case 1: __asm__ __volatile__("msr dbgwcr1_el1, %0" :: "r"(v)); break;
	case 2: __asm__ __volatile__("msr dbgwcr2_el1, %0" :: "r"(v)); break;
	case 3: __asm__ __volatile__("msr dbgwcr3_el1, %0" :: "r"(v)); break;
	case 4: __asm__ __volatile__("msr dbgwcr4_el1, %0" :: "r"(v)); break;
	case 5: __asm__ __volatile__("msr dbgwcr5_el1, %0" :: "r"(v)); break;
	case 6: __asm__ __volatile__("msr dbgwcr6_el1, %0" :: "r"(v)); break;
	case 7: __asm__ __volatile__("msr dbgwcr7_el1, %0" :: "r"(v)); break;
	default: break;
	}
}

/* Push the shadow slots into the hardware, FreeBSD dbg_register_sync
 * order: per slot BCR then BVR (watchpoints: WCR then WVR), every write
 * followed by isb; MDSCR last, then one more isb. MDE gates breakpoint and
 * watchpoint events; KDE rides along as both originals set the pair. */
static void dbg_sync(void)
{
	unsigned long mdscr = mdscr_read();
	unsigned int i;

	for (i = 0U; i < bp_count; i++) {
		if (i < bp_enabled && bp_kind[i] == 1U) {
			dbgbcr_write(i, DBG_BCR_ENABLE_WORD);
		} else {
			dbgbcr_write(i, 0UL);
		}
		__asm__ __volatile__("isb" ::: "memory");
		dbgbvr_write(i, bp_addr[i]);
		__asm__ __volatile__("isb" ::: "memory");
	}
	for (i = 0U; i < wp_count; i++) {
		if (i < wp_enabled) {
			dbgwcr_write(i, wp_ctrl[i]);
		} else {
			dbgwcr_write(i, 0UL);
		}
		__asm__ __volatile__("isb" ::: "memory");
		dbgwvr_write(i, wp_addr[i]);
		__asm__ __volatile__("isb" ::: "memory");
	}
	if (bp_enabled != 0U || wp_enabled != 0U) {
		mdscr |= MDSCR_MDE | MDSCR_KDE;
	} else {
		mdscr &= ~(MDSCR_MDE | MDSCR_KDE);
	}
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

void gdb_arch_setup(void)
{
	unsigned long dfr;
	unsigned long daif;
	unsigned long auth;
	unsigned long oslock;
	unsigned long probe = 0xdeadbeefUL;
	unsigned int i;

	__asm__ __volatile__("mrs %0, daif" : "=r"(daif));
	__asm__ __volatile__("mrs %0, dbgauthstatus_el1" : "=r"(auth));
	__asm__ __volatile__("mrs %0, oslsr_el1" : "=r"(oslock));
	board_log("gdb: boot EL=%lx MDCR_EL2=%lx DAIF=%lx MDSCR=%lx",
		  gdb_boot_entry_el, gdb_boot_mdcr_el2, daif, mdscr_read());
	board_log("gdb: DBGAUTHSTATUS=%lx OSLSR=%lx", auth, oslock);

	/* Clear the OS lock (a bootloader/EL3 may leave it set; a set lock
	 * makes every debug register access trap). */
	__asm__ __volatile__("msr oslar_el1, %0" :: "r"(0UL));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrs %0, oslsr_el1" : "=r"(oslock));
	board_log("gdb: OSLSR after unlock=%lx", oslock);

	/* Boot evidence that EL1 HOLDS debug-register state: DBGBCR/BVR
	 * bits[1:0] are RAZ, so 0xdeadbeef reads back as deadbeec when the
	 * slot retains the write. (The earlier "EL3 swallows debug-register
	 * writes" conclusion was wrong - the readback always held.) */
	__asm__ __volatile__("msr dbgbvr0_el1, %0" :: "r"(probe));
	__asm__ __volatile__("mrs %0, dbgbvr0_el1" : "=r"(probe));
	board_log("gdb: dbgbvr0 readback=%lx (expect deadbeec)", probe);

	/* Resource counts: BRPs field is bits [15:12], WRPs bits [19:16],
	 * both +1 (FreeBSD dbg_monitor_init), clamped to the stub's tables. */
	dfr = dfr_read();
	bp_count = (unsigned int)((dfr >> 12) & 0xfUL) + 1U;
	if (bp_count > MAX_BP) {
		bp_count = MAX_BP;
	}
	wp_count = (unsigned int)((dfr >> 16) & 0xfUL) + 1U;
	if (wp_count > MAX_WP) {
		wp_count = MAX_WP;
	}
	bp_enabled = 0U;
	wp_enabled = 0U;
	board_log("gdb: hw breakpoints=%u watchpoints=%u", bp_count,
		  wp_count);

	/* Reset every slot to disabled and MDSCR debug bits off, so boot
	 * state cannot leak a live resource into the first session. */
	for (i = 0U; i < bp_count; i++) {
		dbgbcr_write(i, 0UL);
		dbgbvr_write(i, 0UL);
	}
	for (i = 0U; i < wp_count; i++) {
		dbgwcr_write(i, 0UL);
		dbgwvr_write(i, 0UL);
	}
	mdscr_write(mdscr_read() & ~(MDSCR_SS | MDSCR_KDE | MDSCR_MDE));
	__asm__ __volatile__("isb" ::: "memory");

	/* Unmask debug exceptions (FreeBSD dbg_enable: msr daifclr, #DAIF_D).
	 * Exception entry sets PSTATE.D itself, so handlers stay quiet while
	 * normal execution keeps breakpoints live. */
	__asm__ __volatile__("msr daifclr, #8" ::: "memory");

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

	/* Same shapes as FreeBSD gdb_cpu_stop_reason: DBGBCR address match
	 * reports "hwbreak", a watchpoint hit reports the faulting data
	 * address, and our own compiled-in BRK reports "swbreak". */
	if (ec == 0x30UL || ec == 0x31UL) {
		return "hwbreak;";
	}
	if (ec == 0x35UL) {
		static char wbuf[40];

		snprintf(wbuf, sizeof(wbuf), "watch:%lx;", tf->far);
		return wbuf;
	}
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
			 "gdb: trap EC=%02lx ELR=%lx SPSR=%lx\n",
			 ec, tf->elr, tf->spsr);
		board_early_print_raw(line);
	}
	if (step_armed) {
		gdb_cpu_singlestep_clear(tf);
	}

	/* The compiled-in gate BRK has no original instruction to execute.
	 * A Z0 patch does: leave its PC at the patched address so GDB can
	 * remove the patch, step the original instruction, then reinsert it. */
	if (ec == 0x3cUL && iss == (unsigned long)GDB_BRK_IMM) {
		unsigned int i;

		for (i = 0U; i < bp_enabled; i++) {
			if (bp_kind[i] == 0U && bp_addr[i] == tf->elr) {
				break;
			}
		}
		if (i == bp_enabled) {
			tf->elr += 4UL;
		}
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

	/* Command handlers own the debug state on exit: 'c'/'D'/'k' clear a
	 * pending step there, and the Z handlers sync the slots when they
	 * change it. There is deliberately NO blanket cleanup here - a step
	 * armed by the host's 's' must survive this ERET (that was the bug
	 * that made stepping dead). */
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

/* --- breakpoints, watchpoints and single step ------------------------------ *
 *
 * Z0 arms a SOFTWARE breakpoint - the reserved BRK #GDB_BRK_IMM patched
 * into RAM text with the FreeBSD DDB cache recipe - and Z1 arms a DBGBCR/
 * BVR slot; Z2-4 arm DBGWVR/WCR slots. Why both kinds: board-proven
 * triage on the RK3568 stock boot chain (BL31 + OP-TEE + U-Boot) shows
 * the debug registers HOLD every value this stub writes (bcr=1e3,
 * bvr=<addr>, mdscr=a000 read back exactly), but self-hosted debug EVENTS
 * other than BRK never deliver - breakpoint (EC 0x31), software step
 * (EC 0x32) and watchpoint (EC 0x35) are all silently not taken, while
 * the BRK instruction (EC 0x3c, not a routed debug event) always traps.
 * The suppression sits above EL1 (MDCR_EL2/SDCR_EL3 territory - this
 * image asserts CurrentEL==EL1 at startup) and cannot be inspected from
 * here, so Z0 takes the BRK road that demonstrably works, Z1/Z2-4 keep
 * the FreeBSD-exact hardware path for chains where the events arrive.
 *
 * Historical note: the earlier rounds blamed the i-cache and then EL3
 * register filtering for hw breakpoints not firing. Both were wrong; the
 * real bug all along was this stub's own Z-packet parser using the
 * freestanding libc's strtol/strtoul, which return 0 for bare hex - every
 * breakpoint was armed on (or patched at) address 0. */

#define BRK_INSN(imm16)	(0xD4200000UL | ((unsigned long)(imm16) << 5))

static unsigned int bp_orig[MAX_BP];	/* patched-away words */

int gdb_cpu_set_hwbp_kind(unsigned long addr, unsigned int kind)
{
	unsigned int i;

	if ((addr & 0x3UL) != 0UL) {
		return -1;	/* instructions are word-aligned here */
	}
	/* Replace if already present. */
	for (i = 0U; i < bp_enabled; i++) {
		if (bp_addr[i] == addr && bp_kind[i] == kind) {
			return 0;
		}
	}
	if (bp_enabled >= bp_count) {
		return -1;
	}
	i = bp_enabled++;
	bp_addr[i] = addr;
	bp_kind[i] = kind;
	if (kind == 0U) {
		/* Software: BRK into text, then the FreeBSD DDB sync. */
		bp_orig[i] = *(volatile unsigned int *)addr;
		*(volatile unsigned int *)addr =
			(unsigned int)BRK_INSN(GDB_BRK_IMM);
		board_sync_written(addr, 4UL);
	} else {
		bp_orig[i] = 0U;
		dbg_sync();
		/* Live evidence per insertion: the slot's BCR/BVR read back
		 * through the session-muted console, splitting "write lost"
		 * from "written but events not delivered" on the wire log. */
		{
			char line[96];

			snprintf(line, sizeof(line),
				 "gdb: bp%u @%lx bcr=%lx bvr=%lx mdscr=%lx\n",
				 i, addr, dbgbcr_read(i), dbgbvr_read(i),
				 mdscr_read());
			board_early_print_raw(line);
		}
	}
	return 0;
}

int gdb_cpu_set_hwbp(unsigned long addr)
{
	return gdb_cpu_set_hwbp_kind(addr, 0U);
}

int gdb_cpu_set_breakpoint_hw(unsigned long addr)
{
	return gdb_cpu_set_hwbp_kind(addr, 1U);
}

static int clr_bp_at(unsigned int i)
{
	unsigned int j;

	if (bp_kind[i] == 0U) {
		*(volatile unsigned int *)bp_addr[i] = bp_orig[i];
		board_sync_written(bp_addr[i], 4UL);
	}
	for (j = i; j + 1U < bp_enabled; j++) {
		bp_addr[j] = bp_addr[j + 1U];
		bp_orig[j] = bp_orig[j + 1U];
		bp_kind[j] = bp_kind[j + 1U];
	}
	bp_enabled--;
	dbg_sync();	/* resyncs the DBGBCR/DBGWCR shadow; BRK patches
			 * were already restored above */
	return 0;
}

int gdb_cpu_clr_hwbp(unsigned long addr)
{
	unsigned int i;

	for (i = 0U; i < bp_enabled; i++) {
		if (bp_addr[i] == addr && bp_kind[i] == 0U) {
			return clr_bp_at(i);
		}
	}
	return -1;
}

int gdb_cpu_clr_breakpoint_hw(unsigned long addr)
{
	unsigned int i;

	for (i = 0U; i < bp_enabled; i++) {
		if (bp_addr[i] == addr && bp_kind[i] == 1U) {
			return clr_bp_at(i);
		}
	}
	return -1;
}

/* lsc: DBGWCR LSC (bits [4:3]) - 1=load, 2=store, 3=both (Z3/Z2/Z4). */
int gdb_cpu_set_watchpoint(unsigned long addr, unsigned long len, int lsc)
{
	unsigned long aligned = addr & ~0x7UL;
	unsigned long off = addr & 0x7UL;
	unsigned long ctrl;
	unsigned int i;

	if (len == 0UL || len > 8UL || off + len > 8UL) {
		return -1;	/* must stay inside one aligned doubleword */
	}
	ctrl = ((((1UL << len) - 1UL) << off) << 5) |	/* BAS */
	       ((unsigned long)lsc << 3) |		/* LSC */
	       ((unsigned long)0x1 << 1) | 0x1UL;	/* PMC EL1 | E */
	for (i = 0U; i < wp_enabled; i++) {
		if (wp_addr[i] == aligned && wp_ctrl[i] == ctrl) {
			return 0;
		}
	}
	if (wp_enabled >= wp_count) {
		return -1;
	}
	wp_addr[wp_enabled] = aligned;
	wp_ctrl[wp_enabled] = ctrl;
	wp_enabled++;
	dbg_sync();
	if (wp_enabled == 1U) {
		unsigned long wcr, wvr;
		char line[96];

		__asm__ __volatile__("mrs %0, dbgwcr0_el1" : "=r"(wcr));
		__asm__ __volatile__("mrs %0, dbgwvr0_el1" : "=r"(wvr));
		snprintf(line, sizeof(line),
			 "gdb: wp0 @%lx wcr=%lx wvr=%lx mdscr=%lx\n",
			 addr, wcr, wvr, mdscr_read());
		board_early_print_raw(line);
	}
	return 0;
}

int gdb_cpu_clr_watchpoint(unsigned long addr, unsigned long len, int lsc)
{
	unsigned long aligned = addr & ~0x7UL;
	unsigned long off = addr & 0x7UL;
	unsigned long ctrl;
	unsigned int i, j;

	if (len == 0UL || len > 8UL || off + len > 8UL) {
		return -1;
	}
	ctrl = ((((1UL << len) - 1UL) << off) << 5) |
	       ((unsigned long)lsc << 3) |
	       ((unsigned long)0x1 << 1) | 0x1UL;
	for (i = 0U; i < wp_enabled; i++) {
		if (wp_addr[i] == aligned && wp_ctrl[i] == ctrl) {
			for (j = i; j + 1U < wp_enabled; j++) {
				wp_addr[j] = wp_addr[j + 1U];
				wp_ctrl[j] = wp_ctrl[j + 1U];
			}
			wp_enabled--;
			dbg_sync();
			return 0;
		}
	}
	return -1;
}

/* Detach must leave the target running exactly as it was before the
 * session: every patched BRK restored, every debug resource disabled, no
 * pending step - a surviving patch would trap the next fetch through the
 * old address with no host listening. */
void gdb_cpu_breakpoints_disarm(void)
{
	unsigned int i;

	for (i = 0U; i < bp_enabled; i++) {
		if (bp_kind[i] == 0U) {
			*(volatile unsigned int *)bp_addr[i] = bp_orig[i];
			board_sync_written(bp_addr[i], 4UL);
		}
	}
	bp_enabled = 0U;
	wp_enabled = 0U;
	dbg_sync();
}

void gdb_cpu_singlestep_set(struct gdb_trapframe *tf)
{
	unsigned long mdscr = mdscr_read();

	/* PSTATE.SS in the SPSR we will restore: the instruction after the
	 * ERET completes, then a SoftwareStep exception comes back. Masking
	 * A/I/F keeps the step quiet; D is left alone so the step exception
	 * can reach us. KDE is required for the software step event itself;
	 * MDE goes OFF for the stepped instruction (FreeBSD
	 * kdb_cpu_set_singlestep) so a watchpoint or breakpoint on it
	 * cannot pin the target in place. */
	step_saved_daif = tf->spsr & 0x3c0UL;
	step_armed = 1;
	tf->spsr |= (1UL << 21);
	tf->spsr &= ~(1UL << 9);	/* D must stay clear for the step trap */
	tf->spsr |= 0x1c0UL;		/* mask A|I|F for the stepped instr */
	mdscr |= MDSCR_SS | MDSCR_KDE;
	mdscr &= ~MDSCR_MDE;
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

void gdb_cpu_singlestep_clear(struct gdb_trapframe *tf)
{
	unsigned long mdscr = mdscr_read();

	if (step_armed) {
		tf->spsr = (tf->spsr & ~0x3c0UL) | step_saved_daif;
		tf->spsr &= ~(1UL << 21);
		step_armed = 0;
	}

	mdscr &= ~(MDSCR_SS | MDSCR_KDE);
	if (bp_enabled != 0U || wp_enabled != 0U) {
		mdscr |= MDSCR_MDE;
	} else {
		mdscr &= ~MDSCR_MDE;
	}
	mdscr_write(mdscr);
	__asm__ __volatile__("isb" ::: "memory");
}

/* Host-written memory needs the I-cache dropped before the CPU will fetch
 * it. Sequence is FreeBSD DDB's db_write_bytes + arm64
 * cpu_icache_sync_range: stores, dsb ish, dc cvau over the range (to the
 * point of unification), ic ialluis, dsb ish, isb. */
void board_sync_written(unsigned long addr, unsigned long len)
{
	unsigned long ctr;
	unsigned long line;
	unsigned long a;
	unsigned long end = addr + len;

	__asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4UL << ((ctr >> 16) & 0xfUL);	/* DminLine */
	__asm__ __volatile__("dsb ish" ::: "memory");
	for (a = addr & ~(line - 1UL); a < end; a += line) {
		__asm__ __volatile__("dc cvau, %0" :: "r"(a) : "memory");
	}
	__asm__ __volatile__("dsb ish" ::: "memory");
	__asm__ __volatile__("ic ialluis" ::: "memory");
	__asm__ __volatile__("dsb ish" ::: "memory");
	__asm__ __volatile__("isb" ::: "memory");
}
