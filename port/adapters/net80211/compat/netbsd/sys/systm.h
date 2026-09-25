/*
 * @file
 * @brief Kernel services shell: panic, delay, printf.
 */

#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_

#include <sys/cdefs.h>
#include "types.h"
#include <sys/intr.h>
#include <sys/mutex.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


/* NetBSD calls panic with the arguments in a second pair of
 * parentheses: panic(("message %d", x)). */

void delay(unsigned int us);
#define DELAY(x) delay(x)

void get_random_bytes(void *, size_t);

int kprintf(const char *fmt, ...) __printflike(1, 2);
#define printf kprintf
#define uprintf kprintf
#define aprint_naive kprintf
#define aprint_verbose kprintf
#define aprint_debug kprintf

#define NBBY 8
static inline void setbit(volatile unsigned char *p, unsigned int n) {
	p[n / NBBY] |= (unsigned char) (1 << (n % NBBY));
}
static inline void clrbit(volatile unsigned char *p, unsigned int n) {
	p[n / NBBY] &= (unsigned char) ~(1 << (n % NBBY));
}
static inline int isset(const volatile unsigned char *p, unsigned int n) {
	return p[n / NBBY] & (1 << (n % NBBY));
}
static inline int isclr(const volatile unsigned char *p, unsigned int n) {
	return !(p[n / NBBY] & (1 << (n % NBBY)));
}

ipl_t splnet(void);
void splx(ipl_t);

/* Process-context sleeps. timo is in ticks (<= 0 waits forever); the
 * wait honours the timeout, dropping the port serializer around it so
 * the interrupt worker can run while a firmware command is pending. */
#define PCATCH 0x100
int tsleep(void *ident, int pri, const char *wmesg, int timo);
void wakeup(void *ident);
void wakeup_one(void *ident);
/* kpause(9): a timed, identified sleep; the imported drivers never
 * hand a lock in */
int kpause(const char *ident, bool nlocked, int timo, kmutex_t *lock);

int uimin(int a, int b);
int uimax(int a, int b);

/* subr_prf.c: bitmask formatting for the imported drivers' dumps */
int snprintb(char *buf, size_t buflen, const char *bitfmt, uint64_t val);

int copyin(const void *, void *, size_t);
int copyout(const void *, void *, size_t);
int copystr(const void *, void *, size_t, size_t *);


static inline void *explicit_memset(void *b, int c, size_t len) {
    volatile uint8_t *p = b;
    size_t i;
    for (i = 0; i < len; i++) p[i] = (uint8_t)c;
    return b;
}
static inline int consttime_memequal(const void *a, const void *b, size_t len) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < len; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}

/* only the imported crypto self-tests call this; no-op */
static inline void
hexdump(int (*print_fn)(const char *, ...) __attribute__((unused)),
    const char *tag, const void *buf, int len)
{
	(void) tag;
	(void) buf;
	(void) len;
}

/* sleepability assertions: single-context carrier, always true */
#define ASSERT_SLEEPABLE() ((void) 0)

static inline void kpreempt_disable(void) { }
static inline void kpreempt_enable(void) { }

struct timeval;
void microtime(struct timeval *tv);
int ratecheck(struct timeval *last, const struct timeval *min);

extern const char ostype[];
extern const char osrelease[];
extern const char version[];

#endif /* _SYS_SYSTM_H_ */
