/*
 * @file
 * @brief kernhist(9) backend: the clock the records are stamped with and
 *        the dump that turns a history ring into console lines.
 *
 * Recording has to be cheap and reentrant (the USB core logs from the
 * interrupt handler too), so the stamp is two MRS reads off the generic
 * timer and the ring write in the header is a plain store.  Printing is
 * shell-paced only, which is what lets the %j rewrite below live in one
 * shared buffer.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <stddef.h>

#include <sys/types.h>
#include <sys/kernhist.h>

/* the console line printer (wlan_console.c), reached the same way the
 * rest of the BSD world reaches it */
int kprintf(const char *fmt, ...) __printflike(1, 2);

static unsigned long long
kernhist_cntvct(void)
{
	unsigned long long v;

	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static unsigned long long
kernhist_cntfrq(void)
{
	unsigned long long v;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

unsigned long long
wlan_kernhist_now_us(void)
{
	const unsigned long long frq = kernhist_cntfrq();

	if (frq == 0ULL) {
		return kernhist_cntvct();
	}
	/* ~30 min of uptime fits: cntvct is a 24-100 MHz counter */
	return (kernhist_cntvct() * 1000000ULL) / frq;
}

/*
 * Every NetBSD DPRINTF format uses the %j (intmax_t) length modifier and
 * the image's vsnprintf does not know it, so rewrite %j -> %ll on the
 * way out.  Escapes, flags, width, precision and '*' pass through; if
 * the rewritten format does not fit the buffer the original is used
 * (a garbled line is better than a truncated format).
 */
static char kernhist_fmtbuf[192];

static const char *
kernhist_fix_fmt(const char *fmt)
{
	size_t o = 0;
	const char *p = fmt;
	int changed = 0;
	int fits = 1;

#define PUT(c)								\
	do {								\
		if (o + 1 < sizeof(kernhist_fmtbuf)) {			\
			kernhist_fmtbuf[o++] = (char) (c);		\
		} else {						\
			fits = 0;					\
		}							\
	} while (0)

	if (fmt == NULL) {
		return "(no fmt)";
	}

	while (*p != '\0' && fits) {
		if (*p != '%') {
			PUT(*p++);
			continue;
		}
		PUT(*p++);
		if (*p == '%') {	/* literal per cent */
			PUT(*p++);
			continue;
		}
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' ||
		       *p == '0') {
			PUT(*p++);
		}
		while ((*p >= '0' && *p <= '9') || *p == '*') {
			PUT(*p++);
		}
		if (*p == '.') {
			PUT(*p++);
			while ((*p >= '0' && *p <= '9') || *p == '*') {
				PUT(*p++);
			}
		}
		if (*p == 'j') {
			p++;
			PUT('l');
			PUT('l');
			changed = 1;
		}
	}
#undef PUT

	if (!fits || !changed) {
		return fmt;
	}
	kernhist_fmtbuf[o] = '\0';
	return kernhist_fmtbuf;
}

void
wlan_kernhist_dump(struct kern_history *h, unsigned int max)
{
	unsigned int have, show, start, i;

	if (h == NULL || h->e == NULL || h->n == 0) {
		kprintf("hist: no ring\n");
		return;
	}
	have = (h->total < h->n) ? h->total : h->n;
	if (have == 0) {
		kprintf("hist %s: empty\n", h->name);
		return;
	}

	show = have;
	if (max != 0 && max < show) {
		show = max;
	}
	/* the newest `show` records, oldest first */
	start = (h->total < h->n) ? (h->total - show) :
	    ((h->f + h->n - show) % h->n);

	kprintf("hist %s: %u records, ring %u, showing the last %u\n",
	    h->name, h->total, h->n, show);
	for (i = 0; i < show; i++) {
		const struct kern_history_ent *e = &h->e[(start + i) % h->n];
		unsigned long long us = e->bt_us;

		kprintf("%3llu.%06llu %s#%u: ", us / 1000000ULL,
		    us % 1000000ULL, (e->fn != NULL) ? e->fn : "?", e->call);
		kprintf(kernhist_fix_fmt(e->fmt), e->v[0], e->v[1], e->v[2],
		    e->v[3]);
		kprintf("\n");
	}
}