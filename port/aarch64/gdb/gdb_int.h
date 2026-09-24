/*
 * @file   gdb_int.h
 * @brief  Internal contract between the stub's packet layer and command
 *         layer (the gdb_int.h role from the FreeBSD original).
 */

#ifndef GDB_INT_H
#define GDB_INT_H

#include <stdint.h>

/* packet layer */
void gdb_send_packet(const char *data, unsigned int len);
void gdb_reply_str(const char *s);
void gdb_reply_ok(void);
void gdb_reply_err(unsigned char err);
int gdb_get_packet(char *buf, unsigned int bufsz);
int hexval(char c);
unsigned int hex2mem(const char *hex, unsigned char *mem, unsigned int nbytes);
char *mem2hex(const unsigned char *mem, char *hex, unsigned int nbytes);

/* arch layer (gdb_arch.c) */
struct gdb_trapframe;
unsigned int gdb_cpu_getregs(char *buf, unsigned int bufsz);
unsigned int gdb_cpu_setregs(const char *hex);
int gdb_cpu_signal(const struct gdb_trapframe *tf);
void gdb_cpu_singlestep_set(struct gdb_trapframe *tf);
void gdb_cpu_singlestep_clear(void);
int gdb_cpu_set_hwbp(unsigned long addr);
int gdb_cpu_clr_hwbp(unsigned long addr);
void gdb_cpu_breakpoints_disarm(void);
unsigned long gdb_cpu_regval(int regnum);
int gdb_cpu_setregval(int regnum, unsigned long val);
void board_sync_written(unsigned long addr, unsigned long len);

/* command layer entry (gdb_main.c) */
void gdb_trap_loop(struct gdb_trapframe *tf, int signal, const char *reason);

#endif /* GDB_INT_H */
