/*
 * @file
 * @brief kernhist(9), really implemented: one static ring per history.
 *
 * The first pass compiled these macros to no-ops, which left the whole
 * imported USB world silent: ehci.c alone logs 174 state transitions
 * (the port-reset PORTSC samples, every qTD halt, the usbdi/hub trail)
 * through USBHIST_LOG/DPRINTF.  Observing an enumeration needs that
 * trail, so this is the upstream macro surface over a slimmed record:
 * no bintime/fmtlen, and the backing store is the caller's static
 * buffer, so KERNHIST_INIT has nothing to allocate.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#ifndef _SYS_KERNHIST_H_
#define _SYS_KERNHIST_H_

#include <sys/cdefs.h>

struct kern_history_ent {
	unsigned long long bt_us;	/* CNTVCT stamp, microseconds */
	const char *fn;			/* enclosing function (KERNHIST_FUNC) */
	const char *fmt;		/* the caller's format, verbatim */
	unsigned int call;		/* per-function call serial */
	unsigned long long v[4];	/* the four logged values */
};

struct kern_history {
	const char *name;
	unsigned int n;			/* slots in the ring */
	unsigned int f;			/* next free slot */
	unsigned int total;		/* records written since the start */
	unsigned int pad;
	struct kern_history_ent *e;
};

unsigned long long wlan_kernhist_now_us(void);
void wlan_kernhist_dump(struct kern_history *h, unsigned int max);

#define KERNHIST_DECL(NAME)	extern struct kern_history NAME
#define KERNHIST_DEFINE(NAME)	struct kern_history NAME

/*
 * The buffer is static, so the two halves just point at each other.
 * Called only from the definitions in the imported sources (usb.c).
 */
#define KERNHIST_INITIALIZER(NAME, BUF) {				\
	.name = __STRING(NAME),						\
	.n = sizeof(BUF) / sizeof(struct kern_history_ent),		\
	.f = 0,								\
	.total = 0,							\
	.e = (struct kern_history_ent *) (BUF),				\
}
#define KERNHIST_INIT(NAME, N)
#define KERNHIST_LINK_STATIC(NAME)

/*
 * One record: reserve the slot first (so a preempting writer takes the
 * next one), then fill it.  Single core, so the index arithmetic needs
 * no atomics; a reader can at worst see the newest record half-written.
 */
#define KERNHIST_LOG(NAME, FMT, A, B, C, D) do {			\
	struct kern_history *_kh_ = &(NAME);				\
	unsigned int _ki_ = _kh_->f;					\
	struct kern_history_ent *_ke_ = &_kh_->e[_ki_];			\
	if (_ki_ + 1 < _kh_->n) {					\
		_kh_->f = _ki_ + 1;					\
	} else {							\
		_kh_->f = 0;						\
	}								\
	_kh_->total++;							\
	_ke_->bt_us = wlan_kernhist_now_us();				\
	_ke_->fn = _kernhist_name;					\
	_ke_->call = _kernhist_call;					\
	_ke_->fmt = (FMT);						\
	_ke_->v[0] = (unsigned long long) (A);				\
	_ke_->v[1] = (unsigned long long) (B);				\
	_ke_->v[2] = (unsigned long long) (C);				\
	_ke_->v[3] = (unsigned long long) (D);				\
} while (0)

#define KERNHIST_CALLED(NAME) do {					\
	_kernhist_call = ++_kernhist_cnt;				\
	KERNHIST_LOG(NAME, "called!", 0, 0, 0, 0);			\
} while (0)

#define KERNHIST_CALLARGS(NAME, FMT, A, B, C, D) do {			\
	_kernhist_call = ++_kernhist_cnt;				\
	KERNHIST_LOG(NAME, "called: " FMT, (A), (B), (C), (D));		\
} while (0)

/* declares the per-function name/serial the two macros above stamp in */
#define KERNHIST_FUNC(FNAME)						\
	static unsigned int _kernhist_cnt = 0;				\
	static const char *const _kernhist_name = FNAME;		\
	unsigned int _kernhist_call = 0

/* dumping goes through wlan_kernhist_dump() from the shell, not DDB */
#define KERNHIST_DUMP(NAME)
#define KERNHIST_PRINT(NAME, FMT)
#define kernhist_print(NAME, FMT)

#endif /* _SYS_KERNHIST_H_ */