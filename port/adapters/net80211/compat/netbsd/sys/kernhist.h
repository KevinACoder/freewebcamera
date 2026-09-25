/*
 * @file
 * @brief kernhist(9) history buffers: compiled to no-ops, the shape
 * NetBSD's own sys/kernhist.h takes when KERNHIST is undefined.
 * usbhist.h (the USBHIST_* debug facade) needs the vocabulary.
 */

#ifndef _SYS_KERNHIST_H_
#define _SYS_KERNHIST_H_

struct kern_history;

typedef struct kern_history_ent {
	void *unused;
} kernhist_entry_t;

#define KERNHIST_DECL(name, h)
#define KERNHIST_DEFINE(name, size)
#define KERNHIST_LINK_STATIC(h)
#define KERNHIST_INIT(h)
#define KERNHIST_FUNC(h)
#define KERNHIST_CALLED(h)
#define KERNHIST_LOG(h, f, a, b, c, d)
#define KERNHIST_CALLARGS(h, f, a, b, c, d)
#define KERNHIST_PRINT(h, f)
#define KERNHIST_DUMP(h)
#define kernhist_print(h, f)

#endif /* _SYS_KERNHIST_H_ */
