/*
 * @file
 * @brief uvm(9) shell: the mapping vocabulary audio(4)'s mmap path names.
 *
 * audio_mmap() is compiled but unreachable on this carrier (no mmap(2),
 * the consumer reads through the fileops read method).  The macros carry
 * upstream's values so the expressions mean the same thing, and the
 * functions are stubs in bsd_file.c that fail - a mapping attempt would
 * be a bug here, not a silently wrong map.
 */

#ifndef _COMPAT_UVM_UVM_EXTERN_H_
#define _COMPAT_UVM_UVM_EXTERN_H_

#include <sys/cdefs.h>
#include <sys/types.h>

typedef uint64_t voff_t;
typedef int uvm_flag_t;
typedef int vm_prot_t;

struct vm_map;
struct uvm_object;
struct lwp;

extern struct vm_map *kernel_map;

/* protections */
#define UVM_PROT_MASK	0x07
#define UVM_PROT_NONE	0x00
#define UVM_PROT_READ	0x01
#define UVM_PROT_WRITE	0x02
#define UVM_PROT_EXEC	0x04
#define UVM_PROT_RW	0x03
#define UVM_PROT_RWX	0x07

/* inherit codes */
#define UVM_INH_MASK	0x30
#define UVM_INH_SHARE	0x00
#define UVM_INH_COPY	0x10
#define UVM_INH_NONE	0x20

/* advice */
#define UVM_ADV_NORMAL		0x0
#define UVM_ADV_RANDOM		0x1
#define UVM_ADV_SEQUENTIAL	0x2
#define UVM_ADV_WILLNEED	0x3
#define UVM_ADV_DONTNEED	0x4

#define UVM_MAPFLAG(PROT, MAXPROT, INH, ADVICE, FLAGS) \
	(((MAXPROT) << 8) | (PROT) | (INH) | ((ADVICE) << 12) | (FLAGS))

int	uvm_map(struct vm_map *, vaddr_t *, vsize_t, struct uvm_object *,
	    voff_t, vsize_t, uvm_flag_t);
int	uvm_unmap(struct vm_map *, vaddr_t, vaddr_t);
int	uvm_map_pageable(struct vm_map *, vaddr_t, vaddr_t, bool, int);

struct uvm_object *uao_create(voff_t, int);
void	uao_detach(struct uvm_object *);
void	uao_reference(struct uvm_object *);

#endif /* _COMPAT_UVM_UVM_EXTERN_H_ */
