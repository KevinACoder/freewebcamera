/*
 * @file
 * @brief conf(5) shell: the cdevsw table shape plus the default device
 * methods, so a driver's character device switch compiles even though
 * nothing dispatches through it here.
 */

#ifndef _COMPAT_SYS_CONF_H_
#define _COMPAT_SYS_CONF_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/event.h>
#include <sys/kernhist.h>

typedef int devmajor_t;
typedef int devminor_t;

struct tty;
struct buf;
struct cfdriver;
struct lwp;
struct uio;
struct knote;

#define D_OTHER		0x0000
#define D_TAPE		0x0001
#define D_DISK		0x0002
#define D_TTY		0x0003
#define D_MPSAFE	0x0100

struct cdevsw {
	int		(*d_open)(dev_t, int, int, struct lwp *);
	int		(*d_cancel)(dev_t, int, int, struct lwp *);
	int		(*d_close)(dev_t, int, int, struct lwp *);
	int		(*d_read)(dev_t, struct uio *, int);
	int		(*d_write)(dev_t, struct uio *, int);
	int		(*d_ioctl)(dev_t, u_long, void *, int, struct lwp *);
	void		(*d_stop)(struct tty *, int);
	struct tty *	(*d_tty)(dev_t);
	int		(*d_poll)(dev_t, int, struct lwp *);
	paddr_t		(*d_mmap)(dev_t, off_t, int);
	int		(*d_kqfilter)(dev_t, struct knote *);
	int		(*d_discard)(dev_t, off_t, off_t);
	int		(*d_devtounit)(dev_t);
	struct cfdriver	*d_cfdriver;
	int		d_flag;
};

/* dev_type_*: the declaring typedefs a driver's cdevsw entry points
 * rely on (NetBSD conf.h spells them out the same way) */
#define dev_type_open(n)	int n(dev_t, int, int, struct lwp *)
#define dev_type_cancel(n)	int n(dev_t, int, int, struct lwp *)
#define dev_type_close(n)	int n(dev_t, int, int, struct lwp *)
#define dev_type_read(n)	int n(dev_t, struct uio *, int)
#define dev_type_write(n)	int n(dev_t, struct uio *, int)
#define dev_type_ioctl(n)	int n(dev_t, u_long, void *, int, struct lwp *)
#define dev_type_stop(n)	void n(struct tty *, int)
#define dev_type_poll(n)	int n(dev_t, int, struct lwp *)
#define dev_type_kqfilter(n)	int n(dev_t, struct knote *)
#define dev_type_mmap(n)	paddr_t n(dev_t, off_t, int)
#define dev_type_discard(n)	int n(dev_t, off_t, off_t)
#define dev_type_ioctl_locked(n) int n(dev_t, u_long, void *, int, struct lwp *)
#define dev_type_cancel(n) int n(dev_t, int, int, struct lwp *)

int nowrite(dev_t, struct uio *, int);
int noclose(dev_t, int, int, struct lwp *);
int noread(dev_t, struct uio *, int);
int noioctl(dev_t, u_long, void *, int, struct lwp *);
int nopoll(dev_t, int, struct lwp *);
int nostop(struct tty *, int);
int noioctl(dev_t, u_long, void *, int, struct lwp *);
int nokqfilter(dev_t, struct knote *);
int nodiscard(dev_t, off_t, off_t);
paddr_t nommap(dev_t, off_t, int);
#define minor(dev) ((int) (unsigned) (dev))
#define major(dev) ((int) ((unsigned) (dev) >> 8))

#define notty ((struct tty * (*)(dev_t)) 0)

/* the video(4) detach path asks for this switch's major and tears down
 * its open instances; there is no cdevsw table and no vnode layer here,
 * so the major is a constant tag and vdevgone is a no-op (the one open
 * consumer closes its unit through the port shim) */
devmajor_t cdevsw_lookup_major(const struct cdevsw *);
void vdevgone(devmajor_t, int, int, int);
#define VCHR	4	/* upstream enum vtype order: VNON VREG VDIR VBLK VCHR */

/* audiostat() stamps S_IFCHR into st_mode; upstream takes it from
 * <sys/stat.h>, which this tree does not carry */
#ifndef S_IFCHR
#define S_IFCHR	0020000
#endif

#endif /* _COMPAT_SYS_CONF_H_ */
