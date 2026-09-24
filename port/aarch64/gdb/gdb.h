/*
 * @file   gdb.h
 * @brief  Serial GDB stub core - transport contract and shared globals.
 *
 * The stub keeps the FreeBSD sys/gdb packet layer's wire behaviour (D56)
 * but is wired through a tiny dbgport ops table instead of FreeBSD's
 * DATA_SET: the adapter (port/adapters/threadx/tx_gdb_glue.c) registers
 * console-UART access at boot, the core never reaches for a driver. The
 * core is kernel-free and lives in port/aarch64/gdb/ (compiled without
 * adapter includes - check-deps enforces it).
 *
 * Derived from FreeBSD sys/gdb (gdb_main.c, gdb_packet.c, BSD-2,
 * Copyright 2004 Marcel Moolenaar) and the lab's fgdb port of it; see
 * docs/imports.md for the full derivation.
 */

#ifndef GDB_H
#define GDB_H

#include <stdint.h>

/* Wire buffer: one RSP packet + framing slack. Advertised to the host as
 * PacketSize=400 - conservative on purpose: 115200 8N1 with no flow control
 * and small frames survive best. */
#define GDB_BUFSZ	1024U

struct gdb_dbgport {
	void	(*putc)(char c);
	int	(*getc)(void);		/* -1 when the FIFO is empty */
	void	(*intr_ctrl)(int on);	/* mask (on) / unmask (off) console INTID */
};

/* Registered once by the adapter before anything can trap. */
void gdb_dbgport_register(const struct gdb_dbgport *ops);

/* 1 once the dbgport is in - the trap entry needs to know whether a fault
 * can be turned into a host stop or has to fall back to a raw park. */
int gdb_dbgport_ready(void);

/* Mask (on=1) / unmask (on=0) the console INTID at the interrupt
 * controller while the stub owns the CPU; a no-op with no dbgport. */
void gdb_dbgport_intr_ctrl(int on);

/* 1 while the stub owns the CPU (between a trap and its continue). The
 * early-print layer gates on this so boot/console output cannot corrupt
 * the RSP stream mid-session. */
extern volatile int gdb_session_active;

/* Compiled-in entry: dsb + BRK #0x401 (the stub's reserved immediate). */
void gdb_break(void);

/* Called from the adapter once, before anything can trap: clears the OS
 * lock, probes the debug architectural maxima, no register side effects
 * beyond OSLAR. */
void gdb_arch_setup(void);

#endif /* GDB_H */
