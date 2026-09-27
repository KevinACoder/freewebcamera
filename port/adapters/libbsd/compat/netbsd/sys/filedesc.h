/*
 * @file
 * @brief file descriptor table shell: the fd_allocfile/fd_clone pair the
 * audio(4) open path fabricates its struct file with.
 *
 * Native audioopen() finishes by handing the file it built to the calling
 * process's descriptor table - fd_allocfile() reserves the number and
 * fd_clone() fills the file in and affixes it, returning EMOVEFD (the
 * syscall layer then replaces the freshly opened vnode with this file).
 * There is no process here: bsd_file.c keeps a small static file pool and
 * a flat descriptor table, fd_clone() stores the file there, and the port
 * consumer reads it back with fd_getfile().
 */

#ifndef _COMPAT_SYS_FILEDESC_H_
#define _COMPAT_SYS_FILEDESC_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/file.h>

typedef struct file file_t;

int	fd_allocfile(file_t **, int *);
void	fd_abort(struct proc *, file_t *, unsigned);
void	fd_affix(struct proc *, file_t *, unsigned);
int	fd_clone(file_t *, unsigned, int, const struct fileops *, void *);
void	fd_putfile(unsigned);

#endif /* _COMPAT_SYS_FILEDESC_H_ */
