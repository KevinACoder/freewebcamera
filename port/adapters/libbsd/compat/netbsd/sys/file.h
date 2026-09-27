/*
 * @file
 * @brief file(9) shell: the struct file / fileops shapes the imported
 * sources reach, without the vnode/fd machinery the native kernel puts
 * under them.
 *
 * The audio(4) middle layer does not go through a vnode: audioopen()
 * fabricates a `struct file' of its own (fd_allocfile + fd_clone, see
 * sys/filedesc.h), and every later operation enters through that file's
 * fileops table (audioread/audioioctl/audioclose).  The port's fd layer
 * (bsd_file.c) emulates the path with a small static file pool, so the
 * shape here follows upstream field for field where the compiled set
 * reaches them: f_flag (the FREAD/FWRITE/… bits fd_clone keeps), f_cred,
 * f_ops and the file_data union with its fd_audioctx member.
 *
 * This header shadows the toolchain's one-line <sys/file.h> (which just
 * includes <sys/fcntl.h>): uvideo.c compiled against that stub because it
 * never dereferences a struct file, and would have failed silently only
 * once something did.
 */

#ifndef _COMPAT_SYS_FILE_H_
#define _COMPAT_SYS_FILE_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/mutex.h>
#include <sys/kauth.h>

struct flock;
struct iovec;
struct knote;
struct lwp;
struct proc;
struct stat;
struct uio;
struct uvm_object;
struct vnode;
struct socket;
struct pipe;
struct kqueue;
struct audio_file;
struct file;		/* declared before fileops: a struct file only
			 * mentioned inside a parameter list is scoped to that
			 * prototype and would be a different type here */

struct fileops {
	const char *fo_name;
	int	(*fo_read)	(struct file *, off_t *, struct uio *,
				    kauth_cred_t, int);
	int	(*fo_write)	(struct file *, off_t *, struct uio *,
				    kauth_cred_t, int);
	int	(*fo_ioctl)	(struct file *, u_long, void *);
	int	(*fo_fcntl)	(struct file *, u_int, void *);
	int	(*fo_poll)	(struct file *, int);
	int	(*fo_stat)	(struct file *, struct stat *);
	int	(*fo_close)	(struct file *);
	int	(*fo_kqfilter)	(struct file *, struct knote *);
	void	(*fo_restart)	(struct file *);
	int	(*fo_mmap)	(struct file *, off_t *, size_t, int, int *,
				 int *, struct uvm_object **, int *);
};

/* upstream's union file_data, narrowed to the members the compiled set
 * names (audio(4) is the only file-backed consumer on this carrier) */
union file_data {
	struct vnode *fd_vp;		/* DTYPE_VNODE */
	struct socket *fd_so;		/* DTYPE_SOCKET */
	struct pipe *fd_pipe;		/* DTYPE_PIPE */
	struct kqueue *fd_kq;		/* DTYPE_KQUEUE */
	void *fd_data;			/* DTYPE_MISC */
	struct audio_file *fd_audioctx;	/* DTYPE_MISC (audio) */
};

#define f_vnode		f_undata.fd_vp
#define f_socket	f_undata.fd_so
#define f_pipe		f_undata.fd_pipe
#define f_kqueue	f_undata.fd_kq
#define f_data		f_undata.fd_data
#define f_audioctx	f_undata.fd_audioctx

struct file {
	off_t		f_offset;	/* first, is 64-bit */
	kauth_cred_t	f_cred;		/* creds associated with descriptor */
	const struct fileops *f_ops;
	union file_data f_undata;	/* descriptor data */
	kmutex_t	f_lock;		/* lock on structure */
	int		f_flag;		/* see fcntl.h */
	u_int		f_type;		/* descriptor type */
	u_int		f_count;	/* reference count */
};

#define	DTYPE_VNODE	1
#define	DTYPE_SOCKET	2
#define	DTYPE_PIPE	3
#define	DTYPE_KQUEUE	4
#define	DTYPE_MISC	5

/* The F* bits fd_clone() keeps in f_flag (upstream FMASK).  The port's
 * fcntl shell gives FNONBLOCK and O_NONBLOCK different values: the video
 * line spells the nonblocking request O_NONBLOCK (the cdev entry points
 * test it directly), while the audio line's audioread tests f_flag
 * against O_NONBLOCK after fd_clone masked it - so both spellings have to
 * survive the mask, or a nonblocking audio consumer would block in the
 * record cv with nothing to wake it. */
#ifndef FMASK
#define	FMASK		(FREAD | FWRITE | FNONBLOCK | O_NONBLOCK | FFLAGS_DEFAULT)
#endif

/* Commonly used fileops (bsd_file.c) */
int	fnullop_fcntl(struct file *, u_int, void *);
int	fnullop_poll(struct file *, int);
int	fnullop_kqfilter(struct file *, struct knote *);
void	fnullop_restart(struct file *);

/* the fd layer's lookup (bsd_file.c); upstream's fd_getfile returns the
 * file for a descriptor number */
struct file *fd_getfile(unsigned fd);

#endif /* _COMPAT_SYS_FILE_H_ */
