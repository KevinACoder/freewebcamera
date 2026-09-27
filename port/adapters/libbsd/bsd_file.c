/*
 * @file
 * @brief The file(9) layer the audio(4) middle layer needs: a small
 * struct file pool, the fd_allocfile/fd_clone pair audioopen() finishes
 * with, and the single-privilege credential / passive-reference /
 * pserialize services the middle layer's locking shape names.
 *
 * audio(4) is the first imported consumer that is not a cdev-open device:
 * its open path fabricates a `struct file' of its own and hands it to the
 * descriptor table (there is no vnode here), and every later operation
 * enters through that file's fileops table.  The pool below stands in for
 * the process descriptor table: fd_allocfile() reserves a slot,
 * fd_clone() fills the file in and returns EMOVEFD (what the native
 * syscall layer expects to see at that point), and the port consumer
 * reads the file back with fd_getfile().
 *
 * The rest of the file is glue for the services audio.c names without the
 * kernel underneath them: one root credential, no-op passive references
 * and pserialize sections (single execution context), failing uvm stubs
 * (the mmap path is unreachable here - the consumer reads), the log(9)
 * sink, and the single-process table's proc_find().
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <sys/types.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/psref.h>
#include <sys/pserialize.h>
#include <sys/proc.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>

/* ------------------------------------------------------------------
 * the file pool
 */

#define FWC_FILE_MAX	8

static struct file fwc_files[FWC_FILE_MAX];

static void
fwc_file_reset(struct file *fp)
{
	memset(fp, 0, sizeof(*fp));
}

int
fd_allocfile(file_t **resultfp, int *resultfd)
{
	int i;

	for (i = 0; i < FWC_FILE_MAX; i++) {
		if (fwc_files[i].f_count == 0) {
			fwc_file_reset(&fwc_files[i]);
			fwc_files[i].f_cred = kauth_cred_get();
			fwc_files[i].f_count = 1;
			*resultfp = &fwc_files[i];
			*resultfd = i;
			return 0;
		}
	}
	return ENFILE;
}

int
fd_clone(file_t *fp, unsigned fd, int flag, const struct fileops *fops,
	void *data)
{
	(void) fd;
	/* upstream: f_flag = flag & FMASK, type/ops/data set, affix to the
	 * descriptor, then EMOVEFD tells the caller "moved".  FMASK keeps
	 * FREAD/FWRITE and the nonblocking spellings so audio's
	 * `fp->f_flag & O_NONBLOCK` tests survive */
	fp->f_flag = flag & FMASK;
	fp->f_type = DTYPE_MISC;
	fp->f_ops = fops;
	fp->f_data = data;
	return EMOVEFD;
}

void
fd_affix(struct proc *p, file_t *fp, unsigned fd)
{
	(void) p; (void) fp; (void) fd;
}

/* the audio(4) open path's failure tail: it reserved a descriptor slot
 * (fd_allocfile) before the fabrication failed, and upstream returns the
 * slot with fd_abort.  nothing was affixed here, so releasing the file
 * that never got handed out is the whole job. */
void
fd_abort(struct proc *p, file_t *fp, unsigned fd)
{
	(void) p;
	(void) fd;
	if (fp != NULL && fp->f_count != 0) {
		fp->f_count = 0;
		fwc_file_reset(fp);
	}
}

/* sysctl(9): the compat tree compiles out (sysctl_createv always fails),
 * so teardown has no log to walk */
void
sysctl_teardown(struct sysctllog **logp)
{
	(void) logp;
}

struct file *
fd_getfile(unsigned fd)
{
	if (fd >= FWC_FILE_MAX) {
		return NULL;
	}
	if (fwc_files[fd].f_count == 0) {
		return NULL;
	}
	return &fwc_files[fd];
}

/* The consumer side of the fabrication: the open path stores its file
 * with whatever fileops the middle layer registered, and the port shim
 * picks it back out by that table (there is no descriptor namespace to
 * key on - the "descriptor" is whatever fd_allocfile handed out). */
struct file *
fwc_file_byops(const struct fileops *fops)
{
	int i;

	for (i = 0; i < FWC_FILE_MAX; i++) {
		if (fwc_files[i].f_count != 0 &&
		    fwc_files[i].f_ops == fops) {
			return &fwc_files[i];
		}
	}
	return NULL;
}

unsigned
fwc_file_index(const struct file *fp)
{
	unsigned i;

	for (i = 0; i < FWC_FILE_MAX; i++) {
		if (&fwc_files[i] == fp) {
			return i;
		}
	}
	return (unsigned) -1;
}

void
fd_putfile(unsigned fd)
{
	if (fd < FWC_FILE_MAX && fwc_files[fd].f_count != 0) {
		fwc_files[fd].f_count--;
		if (fwc_files[fd].f_count == 0) {
			fwc_file_reset(&fwc_files[fd]);
		}
	}
}

/* ------------------------------------------------------------------
 * commonly used fileops (upstream kern_descrip.c shapes)
 */

int
fnullop_fcntl(struct file *fp, u_int cmd, void *data)
{
	(void) fp; (void) data;
	if (cmd == F_SETFL) {
		return 0;
	}
	return EOPNOTSUPP;
}

int
fnullop_poll(struct file *fp, int which)
{
	(void) fp; (void) which;
	return 0;
}

int
fnullop_kqfilter(struct file *fp, struct knote *kn)
{
	(void) fp; (void) kn;
	return EOPNOTSUPP;
}

void
fnullop_restart(struct file *fp)
{
	(void) fp;
}

/* ------------------------------------------------------------------
 * kauth(9): one root credential
 */

static struct kauth_cred fwc_root_cred = {
	.cr_refcnt = 1,
	.cr_uid = 0,
	.cr_euid = 0,
	.cr_svuid = 0,
	.cr_gid = 0,
	.cr_egid = 0,
	.cr_svgid = 0,
	.cr_ngroups = 1,
};

kauth_cred_t
kauth_cred_get(void)
{
	return &fwc_root_cred;
}

kauth_cred_t
kauth_cred_hold(kauth_cred_t cred)
{
	if (cred != NULL && cred != NOCRED && cred != FSCRED) {
		cred->cr_refcnt++;
	}
	return cred;
}

void
kauth_cred_free(kauth_cred_t cred)
{
	if (cred != NULL && cred != NOCRED && cred != FSCRED) {
		cred->cr_refcnt--;
	}
}

uid_t
kauth_cred_geteuid(kauth_cred_t cred)
{
	return cred != NULL ? cred->cr_euid : 0;
}

gid_t
kauth_cred_getegid(kauth_cred_t cred)
{
	return cred != NULL ? cred->cr_egid : 0;
}

/* ------------------------------------------------------------------
 * psref(9): passive references record and drop; teardown is never
 * concurrent here, so there is nothing to drain
 */

struct psref_class {
	const char *pc_name;
	int pc_ipl;
};

struct psref_class *
psref_class_create(const char *name, int ipl)
{
	struct psref_class *pc = kmem_alloc(sizeof(*pc), KM_SLEEP);

	if (pc == NULL) {
		return NULL;
	}
	pc->pc_name = name;
	pc->pc_ipl = ipl;
	return pc;
}

void
psref_class_destroy(struct psref_class *pc)
{
	kmem_free(pc, sizeof(*pc));
}

void
psref_target_init(struct psref_target *prt, struct psref_class *pc)
{
	if (prt != NULL) {
		prt->prt_class = pc;
		prt->prt_draining = false;
	}
}

void
psref_target_destroy(struct psref_target *prt, struct psref_class *pc)
{
	(void) prt; (void) pc;
}

void
psref_acquire(struct psref *ref, const struct psref_target *prt,
	struct psref_class *pc)
{
	(void) pc;
	ref->psref_target = prt;
	ref->psref_lwp = NULL;
	ref->psref_cpu = NULL;
	ref->psref_debug = NULL;
}

void
psref_release(struct psref *ref, const struct psref_target *prt,
	struct psref_class *pc)
{
	(void) prt; (void) pc;
	ref->psref_target = NULL;
}

void
psref_copy(struct psref *dst, const struct psref *src,
	struct psref_class *pc)
{
	(void) pc;
	*dst = *src;
}

bool
psref_held(const struct psref_target *prt, struct psref_class *pc)
{
	(void) prt; (void) pc;
	return true;
}

void
psref_init(void)
{
}

/* ------------------------------------------------------------------
 * pserialize(9): read sections are free, perform() has nothing to wait
 */

pserialize_t
pserialize_create(void)
{
	return (pserialize_t) (uintptr_t) 1;
}

void
pserialize_destroy(pserialize_t psz)
{
	(void) psz;
}

pserialize_read_critical_t
pserialize_read_enter(void)
{
	return 0;
}

void
pserialize_read_exit(pserialize_read_critical_t s)
{
	(void) s;
}

void
pserialize_perform(pserialize_t psz)
{
	(void) psz;
}

/* ------------------------------------------------------------------
 * uvm(9): the mmap path is unreachable on this carrier; a mapping
 * attempt fails instead of pretending to succeed
 */

struct vm_map *kernel_map = NULL;

int
uvm_map(struct vm_map *map, vaddr_t *startp, vsize_t size,
	struct uvm_object *uobj, voff_t offset, vsize_t align,
	uvm_flag_t flags)
{
	(void) map; (void) startp; (void) size; (void) uobj;
	(void) offset; (void) align; (void) flags;
	return ENOMEM;
}

int
uvm_unmap(struct vm_map *map, vaddr_t start, vaddr_t end)
{
	(void) map; (void) start; (void) end;
	return 0;
}

int
uvm_map_pageable(struct vm_map *map, vaddr_t start, vaddr_t end,
	bool pageable, int flags)
{
	(void) map; (void) start; (void) end; (void) pageable; (void) flags;
	return 0;
}

struct uvm_object *
uao_create(voff_t size, int flags)
{
	(void) size; (void) flags;
	return NULL;
}

void
uao_detach(struct uvm_object *uobj)
{
	(void) uobj;
}

void
uao_reference(struct uvm_object *uobj)
{
	(void) uobj;
}

/* ------------------------------------------------------------------
 * syslog(9) sink: the BSD console
 */

void
log(int level, const char *fmt, ...)
{
	va_list ap;

	(void) level;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------
 * the one process: the shell
 */

struct proc proc0_holder = {
	.p_vmspace = NULL,
	.p_pid = 1,
};

struct proc *
proc_find(pid_t pid)
{
	if (pid == proc0_holder.p_pid) {
		return &proc0_holder;
	}
	return NULL;
}
