/*
 * @file
 * @brief The parts of subr_prf.c the compiled NetBSD set reaches.
 *
 * snprintb(3) (xhci.c formats its HCCPARAMS dumps with it).  Both
 * bitmask styles are decoded: the old "\20" style (octal radix byte,
 * then bit + NUL-terminated name pairs) and the new "\177\020" style
 * ('b' bit entries and 'f' field entries), which is the one xhci.c
 * actually ships.  The output is diagnostics-grade: "0x<val>" followed
 * by a <NAME> per set named bit, <bN> for unnamed set bits and
 * <NAME=0xv> for fields.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include <sys/systm.h>

struct snb_out {
	char *buf;
	size_t size;
	size_t pos;
};

/* printf-append that always leaves the buffer NUL-terminated and the
 * position clamped inside the buffer */
static void snb_printf(struct snb_out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->pos + 1U >= o->size) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(o->buf + o->pos, o->size - o->pos, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	o->pos += (size_t) n;
	if (o->pos >= o->size) {
		o->pos = o->size - 1U;
	}
}

/* one field: <NAME=0xvalue> */
static void snb_field(struct snb_out *o, const char *name, uint64_t v,
    unsigned int pos, unsigned int len)
{
	uint64_t mask = (len >= 64U) ? ~0ULL : ((1ULL << len) - 1ULL);

	if (name[0] == '\0') {
		return;
	}
	snb_printf(o, "<%s=0x%llx>", name,
	    (unsigned long long) ((v >> pos) & mask));
}

/* find first set bit, 1-based, 0 if none (xhci.c decodes PAGESIZE
 * with it) */
int
ffs(int x)
{
	unsigned int u = (unsigned int) x;
	int i;

	for (i = 0; i < 32; i++) {
		if ((u & (1U << i)) != 0U) {
			return i + 1;
		}
	}
	return 0;
}

int
snprintb(char *buf, size_t buflen, const char *bitfmt, uint64_t val)
{
	struct snb_out o;
	const char *p;

	if (buflen == 0U) {
		return 0;
	}
	o.buf = buf;
	o.size = buflen;
	o.pos = 0U;

	snb_printf(&o, "0x%llx", (unsigned long long) val);

	p = bitfmt;
	if (*p == '\177') {
		/* new style: \177 + radix byte, then b/f entries */
		p++;
		if (*p != '\0') {
			p++; /* radix */
		}
		while (*p != '\0') {
			unsigned int pos, len;

			if (*p == 'b') {
				p++;
				pos = 0U;
				while (*p >= '0' && *p <= '7') {
					pos = pos * 8U + (unsigned) (*p - '0');
					p++;
				}
				if ((val & (1ULL << pos)) != 0ULL) {
					snb_printf(&o, "<%s>", p);
				}
			} else if (*p == 'f') {
				p++;
				pos = 0U;
				while (*p >= '0' && *p <= '7') {
					pos = pos * 8U + (unsigned) (*p - '0');
					p++;
				}
				len = 0U;
				while (*p >= '0' && *p <= '7') {
					len = len * 8U + (unsigned) (*p - '0');
					p++;
				}
				snb_field(&o, p, val, pos, len);
			} else {
				/* unknown entry kind: skip to the next NUL */
			}
			while (*p != '\0') {
				p++;
			}
			p++;
		}
	} else {
		/* old style: radix byte, then octal bit + name pairs */
		if (*p != '\0') {
			p++;
		}
		while (*p != '\0') {
			unsigned int pos = 0U;

			while (*p >= '0' && *p <= '7') {
				pos = pos * 8U + (unsigned) (*p - '0');
				p++;
			}
			if (*p == '\0') {
				break;
			}
			if ((val & (1ULL << pos)) != 0ULL) {
				snb_printf(&o, "<%s>", p);
			}
			while (*p != '\0') {
				p++;
			}
			p++;
		}
	}

	if (o.pos < o.size) {
		o.buf[o.pos] = '\0';
	}
	return (int) o.pos;
}
