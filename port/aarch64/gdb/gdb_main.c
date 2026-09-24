/*
 * @file   gdb_main.c
 * @brief  RSP command loop.
 *
 * Command behaviour derived from FreeBSD sys/gdb/gdb_main.c (BSD-2, 2004
 * Marcel Moolenaar): the trap reply, register/memory access, hardware
 * breakpoints (Z0 aliased onto Z1 - debug resources are the hardware kind
 * on this target) and continue/single-step/detach semantics.
 *
 * Not implemented and NOT advertised: vCont (the host falls back to c/s),
 * X/qSearch, watchpoints (Z2-4). Of the qXfer family only
 * qXfer:features:read:target.xml is implemented - it declares exactly the
 * 34 core registers the g packet reports, without which GDB assumes the
 * full AArch64 set including FP and rejects the short g packet (see
 * target_xml below). An unsupported command answers with an empty packet,
 * which GDB treats as unsupported rather than fatal.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gdb.h"
#include "gdb_arch.h"
#include "gdb_int.h"

volatile int gdb_session_active;

/* Local hex parse: strtoul does not exist in the freestanding libc and
 * whatever the link pulled in for it is not trusted. */
static unsigned long gdb_hex_ul(const char *s, char **end)
{
	unsigned long v = 0;

	while (1) {
		int c = *s;

		if (c >= '0' && c <= '9') {
			c -= '0';
		} else if (c >= 'a' && c <= 'f') {
			c -= 'a' - 10;
		} else if (c >= 'A' && c <= 'F') {
			c -= 'A' - 10;
		} else {
			break;
		}
		v = v * 16UL + (unsigned long)c;
		s++;
	}
	*end = (char *)s;
	return v;
}

/* freestanding: no libc str* - the project's minilibc does not carry
 * strcpy and the stub needs nothing else from it. */
static char *gdb_strcpy(char *dst, const char *src)
{
	char *d = dst;

	while (*src != '\0') {
		*d++ = *src++;
	}
	*d = '\0';
	return dst;
}

/* Stop reply buffer: "T05" + reason + thread terminator. */
static char stop_reply[64];

/* Target description served via qXfer:features:read:target.xml. It lists
 * exactly the registers gdb_cpu_getregs() reports - x0..x30, sp, pc, cpsr.
 * The order here defines the host's register numbering, so it must match
 * gdb_arch.h's GDB_REG_* constants. Without this description GDB assumes
 * the full AArch64 register set (34 core + 32 FP + fpsr/fpcr), fails to
 * parse the short g packet ("Truncated register 35") and silently falls
 * back to reading "memory" from the ELF file - values that look plausible
 * but never came from the target. The image is built -mgeneral-regs-only:
 * the FP bank holds no state, so it is not declared at all. */
static const char target_xml[] =
	"<?xml version=\"1.0\"?>"
	"<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">"
	"<target>"
	"<feature name=\"org.gnu.gdb.aarch64.core\">"
	"<reg name=\"x0\" bitsize=\"64\"/>"
	"<reg name=\"x1\" bitsize=\"64\"/>"
	"<reg name=\"x2\" bitsize=\"64\"/>"
	"<reg name=\"x3\" bitsize=\"64\"/>"
	"<reg name=\"x4\" bitsize=\"64\"/>"
	"<reg name=\"x5\" bitsize=\"64\"/>"
	"<reg name=\"x6\" bitsize=\"64\"/>"
	"<reg name=\"x7\" bitsize=\"64\"/>"
	"<reg name=\"x8\" bitsize=\"64\"/>"
	"<reg name=\"x9\" bitsize=\"64\"/>"
	"<reg name=\"x10\" bitsize=\"64\"/>"
	"<reg name=\"x11\" bitsize=\"64\"/>"
	"<reg name=\"x12\" bitsize=\"64\"/>"
	"<reg name=\"x13\" bitsize=\"64\"/>"
	"<reg name=\"x14\" bitsize=\"64\"/>"
	"<reg name=\"x15\" bitsize=\"64\"/>"
	"<reg name=\"x16\" bitsize=\"64\"/>"
	"<reg name=\"x17\" bitsize=\"64\"/>"
	"<reg name=\"x18\" bitsize=\"64\"/>"
	"<reg name=\"x19\" bitsize=\"64\"/>"
	"<reg name=\"x20\" bitsize=\"64\"/>"
	"<reg name=\"x21\" bitsize=\"64\"/>"
	"<reg name=\"x22\" bitsize=\"64\"/>"
	"<reg name=\"x23\" bitsize=\"64\"/>"
	"<reg name=\"x24\" bitsize=\"64\"/>"
	"<reg name=\"x25\" bitsize=\"64\"/>"
	"<reg name=\"x26\" bitsize=\"64\"/>"
	"<reg name=\"x27\" bitsize=\"64\"/>"
	"<reg name=\"x28\" bitsize=\"64\"/>"
	"<reg name=\"x29\" bitsize=\"64\"/>"
	"<reg name=\"x30\" bitsize=\"64\"/>"
	"<reg name=\"sp\" bitsize=\"64\"/>"
	"<reg name=\"pc\" bitsize=\"64\"/>"
	"<reg name=\"cpsr\" bitsize=\"32\"/>"
	"</feature>"
	"</target>";

/* qXfer chunk size: the reply packet must fit the advertised PacketSize
 * (1024 bytes) with framing and checksum; 256 keeps it comfortable on the
 * 115200 line as well. */
#define GDB_XML_CHUNK 256U

static void build_stop_reply(int signal, const char *reason)
{
	unsigned int o = 0U;

	stop_reply[o++] = 'T';
	stop_reply[o++] = "0123456789abcdef"[(unsigned)signal >> 4];
	stop_reply[o++] = "0123456789abcdef"[(unsigned)signal & 0xfU];
	if (reason != 0) {
		while (*reason != '\0') {
			stop_reply[o++] = *reason++;
		}
	}
	(void)gdb_strcpy(&stop_reply[o], "thread:1;");
}

/* Raw memory probe: everything the stub reads must be mapped; the kernel
 * text/heap/peripheral space it debugs is, and a stray address from the
 * host just returns whatever the translation maps there. */

/* Stop-reply announcement state. After the host issued c/s it sits in
 * wait-mode and expects the NEXT stop to be announced - a silent break-in
 * would hang it. So the stop reply is pushed on trap entry iff the last
 * host interaction was a continue/step (the flag is consumed then). Any
 * received host packet proves the host is not waiting for an old stop and
 * clears the flag; D/k detach clears it too - after those, a break-in with
 * nobody listening must stay silent, or a stale T05 would poison the FIFO
 * and mis-align the next real session (board-proven failure mode). */
static int awaiting_stop;

void gdb_trap_loop(struct gdb_trapframe *tf, int signal, const char *reason)
{
	static char buf[GDB_BUFSZ];
	static char obuf[GDB_BUFSZ];

	/* The stop reply is sent ONLY in answer to the host's first '?'
	 * (no host has spoken yet), or pushed right here when the host had
	 * continued and is waiting. An unsolicited T05 fired before any host
	 * is listening cannot be ACKed, and with fire-and-forget TX it would
	 * sit in the FIFO and be parsed by the host as the answer to its
	 * first command - the whole session then runs one response out of
	 * phase. */
	build_stop_reply(signal, reason);
	if (awaiting_stop) {
		awaiting_stop = 0;
		gdb_send_packet(stop_reply, strlen(stop_reply));
	}

	for (;;) {
		int len;

		len = gdb_get_packet(buf, sizeof(buf));
		if (len <= 0) {
			continue;
		}
		awaiting_stop = 0;

		switch (buf[0]) {
		case '?':
			gdb_reply_str(stop_reply);
			break;

		case 'g': {
			unsigned int n = gdb_cpu_getregs(obuf, sizeof(obuf));

			gdb_send_packet(obuf, n);
			break;
		}

		case 'G':
			if (gdb_cpu_setregs(&buf[1]) == 0U) {
				gdb_reply_ok();
			} else {
				gdb_reply_err(1);
			}
			break;

		case 'm': {
			char *q;
			unsigned long addr = gdb_hex_ul(&buf[1], &q);
			unsigned long n = 0U;
			unsigned char mem[256];
			unsigned int i;

			if (q != &buf[1] && *q == ',') {
				n = gdb_hex_ul(q + 1, &q);
			}
			if (n == 0UL || n > sizeof(mem)) {
				gdb_reply_err(1);
				break;
			}
			for (i = 0U; i < (unsigned int)n; i++) {
				mem[i] = ((const unsigned char *)
					  (uintptr_t)addr)[i];
			}
			mem2hex(mem, obuf, (unsigned int)n);
			gdb_reply_str(obuf);
			break;
		}

		case 'M': {
			/* "Maddr,length:data" - write one byte per two hex
			 * chars. The length field is part of the format; a
			 * parser that expects "Maddr:data" answers E01 to
			 * every write GDB ever sends (board-proven). */
			char *q;
			unsigned long addr = gdb_hex_ul(&buf[1], &q);
			unsigned long n = 0UL;
			unsigned char mem[256];
			const char *data;

			if (q != &buf[1] && *q == ',') {
				n = gdb_hex_ul(q + 1, &q);
			}
			data = (*q == ':') ? q + 1 : 0;
			if (data == 0 || n == 0UL || n > sizeof(mem) ||
			    (unsigned long)strlen(data) / 2UL < n ||
			    hex2mem(data, mem, (unsigned int)n) !=
				    (unsigned int)n) {
				gdb_reply_err(1);
				break;
			}
			for (unsigned long i = 0; i < n; i++) {
				((unsigned char *)(uintptr_t)addr)[i] = mem[i];
			}
			board_sync_written(addr, n);
			gdb_reply_ok();
			break;
		}

		case 'p': {
			char *q0;
			int regnum = (int)gdb_hex_ul(&buf[1], &q0);
			unsigned char raw[8];
			unsigned long v;
			unsigned int width = 8U;

			if (regnum < 0 || regnum >= GDB_NREGS) {
				gdb_reply_err(1);
				break;
			}
			v = gdb_cpu_regval(regnum);
			raw[0] = (unsigned char)(v);
			raw[1] = (unsigned char)(v >> 8);
			raw[2] = (unsigned char)(v >> 16);
			raw[3] = (unsigned char)(v >> 24);
			raw[4] = (unsigned char)(v >> 32);
			raw[5] = (unsigned char)(v >> 40);
			raw[6] = (unsigned char)(v >> 48);
			raw[7] = (unsigned char)(v >> 56);
			if (regnum == GDB_REG_CPSR) {
				width = 4U;
			}
			mem2hex(raw, obuf, width);
			gdb_reply_str(obuf);
			break;
		}

		case 'P': {
			char *q;
			char *q0;
			long regnum = (long)gdb_hex_ul(&buf[1], &q0);
			unsigned char raw[8];
			unsigned long v;

			if (regnum < 0 || regnum >= GDB_NREGS || *q != '=' ||
			    hex2mem(q + 1, raw, 8U) != 8U) {
				gdb_reply_err(1);
				break;
			}
			v = (unsigned long)raw[0] |
			    ((unsigned long)raw[1] << 8) |
			    ((unsigned long)raw[2] << 16) |
			    ((unsigned long)raw[3] << 24) |
			    ((unsigned long)raw[4] << 32) |
			    ((unsigned long)raw[5] << 40) |
			    ((unsigned long)raw[6] << 48) |
			    ((unsigned long)raw[7] << 56);
			if (gdb_cpu_setregval((int)regnum, v) == 0) {
				gdb_reply_ok();
			} else {
				gdb_reply_err(1);
			}
			break;
		}

		case 'Z':
		case 'z': {
			char *q;
			long type = strtol(&buf[1], &q, 16);
			unsigned long addr = 0UL;
			int r;

			if (*q == ',') {
				addr = strtoul(q + 1, 0, 16);
			}
			if (type != 0 && type != 1) {
				gdb_reply_str("");
				break;
			}
			/* Z0 (software breakpoint) maps onto the hardware
			 * slots: debug state owns the resource here. */
			if (buf[0] == 'Z') {
				r = gdb_cpu_set_hwbp(addr);
			} else {
				r = gdb_cpu_clr_hwbp(addr);
			}
			if (r == 0) {
				gdb_reply_ok();
			} else {
				gdb_reply_err(1);
			}
			break;
		}

		case 'c':
			gdb_cpu_singlestep_clear();
			awaiting_stop = 1;
			return;

		case 's':
			gdb_cpu_singlestep_set(tf);
			awaiting_stop = 1;
			return;

		case 'D':
		case 'k':
			/* Detach/kill: leave the debug state exactly as found -
			 * breakpoints disarmed, no pending step - so the
			 * target resumes clean even if the host never comes
			 * back. */
			gdb_cpu_breakpoints_disarm();
			gdb_cpu_singlestep_clear();
			awaiting_stop = 0;
			gdb_reply_ok();
			return;

		case 'H':
			/* Thread selection: single-threaded stub. */
			gdb_reply_ok();
			break;

		case 'q':
			if (strncmp(buf, "qSupported", 10) == 0) {
				gdb_reply_str(
				    "PacketSize=400;qXfer:features:read+");
			} else if (strncmp(buf, "qC", 2) == 0) {
				gdb_reply_str("QC1");
			} else if (strncmp(buf, "qfThreadInfo", 12) == 0) {
				gdb_reply_str("m1");
			} else if (strncmp(buf, "qsThreadInfo", 12) == 0) {
				gdb_reply_str("l");
			} else if (strncmp(buf, "qOffsets", 8) == 0) {
				gdb_reply_str("Text=0;Data=0;Bss=0");
			} else if (strncmp(buf,
					   "qXfer:features:read:target.xml:",
					   31) == 0) {
				/* Annex is fixed (target.xml); parse the
				 * "offset,length" suffix. 'm' marks more
				 * chunks, 'l' the last one. */
				char *q;
				unsigned long off = gdb_hex_ul(&buf[31], &q);
				unsigned long len = 0U;
				unsigned long total = sizeof(target_xml) - 1UL;
				unsigned int chunk;

				if (*q == ',') {
					len = gdb_hex_ul(q + 1, &q);
				}
				if (off >= total) {
					gdb_reply_err(1);
					break;
				}
				if (len > GDB_XML_CHUNK) {
					len = GDB_XML_CHUNK;
				}
				chunk = (unsigned int)len;
				if (off + chunk > total) {
					chunk = (unsigned int)(total - off);
				}
				obuf[0] =
				    (off + chunk < total) ? 'm' : 'l';
				memcpy(&obuf[1], &target_xml[off], chunk);
				obuf[1 + chunk] = '\0';
				gdb_reply_str(obuf);
			} else {
				gdb_reply_str("");
			}
			break;

		default:
			gdb_reply_str("");
			break;
		}
	}
}
