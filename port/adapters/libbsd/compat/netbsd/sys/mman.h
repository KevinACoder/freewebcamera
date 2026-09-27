/*
 * @file
 * @brief mman(9) shell: mapping flags only.
 *
 * audio(4)'s mmap path returns MAP_SHARED through flagsp and (inside a
 * dead #if 0 block) speaks VM_PROT_*.  This carrier has no mmap(2) - the
 * consumer reads - so the flags exist for the compiler, and
 * audio_mmap()'s uvm calls fail in the stub (bsd_file.c) if a mapping is
 * ever attempted.
 */

#ifndef _COMPAT_SYS_MMAN_H_
#define _COMPAT_SYS_MMAN_H_

#include <sys/cdefs.h>

#define PROT_NONE	0x00
#define PROT_READ	0x01
#define PROT_WRITE	0x02
#define PROT_EXEC	0x04

#define MAP_SHARED	0x0001
#define MAP_PRIVATE	0x0002

#define VM_PROT_NONE	PROT_NONE
#define VM_PROT_READ	PROT_READ
#define VM_PROT_WRITE	PROT_WRITE
#define VM_PROT_EXECUTE	PROT_EXEC

#endif /* _COMPAT_SYS_MMAN_H_ */
