/*
 * @file
 * @brief The freestanding libc surface the wlan stack and the
 * supplicant expect.
 *
 * The image is -nostdlib with the minilibc surface (vsnprintf,
 * str/mem functions); the net80211 library's host-world units (the
 * lwIP presentation) and the hostap tree call plain printf/malloc
 * family functions. This unit provides them: console output over the
 * CMSIS USART driver (same channel and ready-gate as the lwIP
 * diagnostics), memory over the kernel heap (pvPortMalloc/vPortFree
 * exist on both kernel lines). The FILE* arguments are accepted and
 * ignored - the image has no real FILE objects, nothing re-enters
 * newlib's stdio internals.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"

/* this unit is the host-world libc surface: the BSD-world malloc(9)
 * macro family must not intercept the definitions below */
#undef malloc
#undef free

/* output goes live only after the console driver is initialized;
 * before that the bytes would vanish into an unarmed peripheral */
static int console_ready;

void wlan_console_ready(void) {
	console_ready = 1;
}

static void console_emit(const char *s) {
	if (!console_ready) {
		return;
	}
	/* board_console_write: same polled UART as board_early_print under
	 * the same print lock - urtwn attach prints used to shred concurrent
	 * fault dumps because the CMSIS USART driver sat on no lock at all */
	board_console_write(s);
}

/* one shared line buffer: wlan prints are shell-paced, never reentered */
static char printf_buf[512];

int printf(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(printf_buf, sizeof(printf_buf), fmt, ap);
	va_end(ap);
	console_emit(printf_buf);
	return 0;
}

int vprintf(const char *fmt, va_list ap) {
	(void) vsnprintf(printf_buf, sizeof(printf_buf), fmt, ap);
	console_emit(printf_buf);
	return 0;
}

int fprintf(FILE *stream, const char *fmt, ...) {
	va_list ap;

	/* stdout and stderr land on the same console */
	(void) stream;
	va_start(ap, fmt);
	(void) vsnprintf(printf_buf, sizeof(printf_buf), fmt, ap);
	va_end(ap);
	console_emit(printf_buf);
	return 0;
}

int fflush(FILE *stream) {
	(void) stream;
	return 0;
}

int puts(const char *s) {
	console_emit(s);
	console_emit("\r\n");
	return 0;
}

int fputs(const char *s, FILE *stream) {
	(void) stream;
	console_emit(s);
	return 0;
}

/* ------------------------------------------------------------------ */
/* heap: the kernel heap under the libc names (hostap's os.h maps
 * os_malloc onto these; the cherryusb class hook allocates the
 * interface shell with malloc). A size header sits in front of each
 * block so realloc can copy without poking heap_4 internals. Only
 * allocations made through these names pass here; the net80211 OSAL
 * binds its hooks straight to pvPortMalloc/vPortFree and never mixes
 * the two worlds. */

extern void *pvPortMalloc(size_t length);
extern void vPortFree(void *ptr);

struct wlan_alloc_hdr {
	size_t size;
	uint32_t magic;
	uint32_t pad;
};

#define WLAN_ALLOC_MAGIC 0x574c414eu /* 'WLAN' */

void *malloc(size_t size) {
	struct wlan_alloc_hdr *h;

	if (size == 0) {
		size = 1;
	}
	h = pvPortMalloc(sizeof(*h) + size);
	if (h == NULL) {
		return NULL;
	}
	h->size = size;
	h->magic = WLAN_ALLOC_MAGIC;
	h->pad = 0;
	return (void *) (h + 1);
}

void free(void *p) {
	struct wlan_alloc_hdr *h = p;

	if (h == NULL) {
		return;
	}
	if (h->magic != WLAN_ALLOC_MAGIC) {
		/* a raw pvPortMalloc pointer freed through the libc name
		 * would be a bug; keep the heap intact either way */
		return;
	}
	h->magic = 0;
	vPortFree(h);
}

void *calloc(size_t n, size_t size) {
	void *p = malloc(n * size);

	if (p != NULL) {
		memset(p, 0, n * size);
	}
	return p;
}

void *realloc(void *p, size_t size) {
	struct wlan_alloc_hdr *h = p;
	void *n;

	if (p == NULL) {
		return malloc(size);
	}
	if (h->magic != WLAN_ALLOC_MAGIC) {
		return NULL;
	}
	n = malloc(size);
	if (n != NULL) {
		memcpy(n, p, h->size < size ? h->size : size);
		free(p);
	}
	return n;
}

char *strdup(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p != NULL) {
		memcpy(p, s, n);
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* The remaining libc surface the compiled set references: hostap's
 * utils and the net80211 glue use the ctype macros (newlib reads the
 * _ctype_ table), a handful of string functions the minilibc does not
 * carry, qsort for the bss list, and abort()/setvbuf() on paths that
 * are fatal or no-ops here. */

extern const char _ctype_[];
const char _ctype_[257] = {
	/* [0] is EOF */
	040,
	/* 1..31: control */
	040, 040, 040, 040, 040, 040, 040, 040, 010, 010, 010, 010, 010,
	040, 040, 040, 040, 040, 040, 040, 040, 040, 040, 040, 040, 040,
	040, 040, 040, 040, 040, 040,
	/* 32: space */
	0100,
	/* 33..47: punctuation */
	020, 020, 020, 020, 020, 020, 020, 020, 020, 020, 020, 020, 020,
	020, 020,
	/* 48..57: digits (0-9 also hex) */
	04 + 0100, 04 + 0100, 04 + 0100, 04 + 0100, 04 + 0100,
	04 + 0100, 04 + 0100, 04 + 0100, 04 + 0100, 04 + 0100,
	/* 58..64: punctuation */
	020, 020, 020, 020, 020, 020, 020,
	/* 65..90: uppercase (A-F also hex) */
	01 + 0100, 01 + 0100, 01 + 0100, 01 + 0100, 01 + 0100, 01 + 0100,
	01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01,
	01, 01, 01, 01,
	/* 91..96: punctuation */
	020, 020, 020, 020, 020, 020,
	/* 97..122: lowercase (a-f also hex) */
	02 + 0100, 02 + 0100, 02 + 0100, 02 + 0100, 02 + 0100, 02 + 0100,
	02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02,
	02, 02, 02, 02,
	/* 123..127: punctuation + DEL */
	020, 020, 020, 020, 040,
	/* 128..255: undefined in this table */
	0
};

void abort(void) {
	console_emit("abort() called\n");
	for (;;) {
	}
}

int abs(int n) {
	return n < 0 ? -n : n;
}

int strcasecmp(const char *a, const char *b) {
	while (*a != '\0' && *b != '\0') {
		int ca = *a;
		int cb = *b;
		int d;

		if (ca >= 'A' && ca <= 'Z') {
			ca += 'a' - 'A';
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb += 'a' - 'A';
		}
		d = ca - cb;
		if (d != 0) {
			return d;
		}
		a++;
		b++;
	}
	return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
	while (n-- != 0U && *a != '\0' && *b != '\0') {
		int ca = *a;
		int cb = *b;
		int d;

		if (ca >= 'A' && ca <= 'Z') {
			ca += 'a' - 'A';
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb += 'a' - 'A';
		}
		d = ca - cb;
		if (d != 0) {
			return d;
		}
		a++;
		b++;
	}
	return (n == (size_t) -1) ? 0 :
	       ((int) (unsigned char) *a - (int) (unsigned char) *b);
}

char *strstr(const char *haystack, const char *needle) {
	size_t n;

	if (*needle == '\0') {
		return (char *) haystack;
	}
	n = strlen(needle);
	for (; *haystack != '\0'; haystack++) {
		if (strncmp(haystack, needle, n) == 0) {
			return (char *) haystack;
		}
	}
	return NULL;
}

char *strrchr(const char *s, int c) {
	const char *last = NULL;

	for (; *s != '\0'; s++) {
		if (*s == (char) c) {
			last = s;
		}
	}
	return (char *) last;
}

static void wl_sift_down(char *base, size_t root, size_t n,
	size_t width, int (*cmp)(const void *, const void *)) {
	for (;;) {
		size_t child = 2 * root;
		char *c;
		char *r;

		if (child > n) {
			return;
		}
		if (child < n && cmp(base + child * width,
			base + (child - 1) * width) < 0) {
			child++;
		}
		r = base + (root - 1) * width;
		c = base + (child - 1) * width;
		if (cmp(r, c) >= 0) {
			return;
		}
		for (size_t k = 0; k < width; k++) {
			char t = r[k];

			r[k] = c[k];
			c[k] = t;
		}
		root = child;
	}
}

void qsort(void *base, size_t nmemb, size_t width,
	int (*cmp)(const void *, const void *)) {
	char *b = base;
	size_t n = nmemb;
	size_t i;

	if (n < 2 || width == 0) {
		return;
	}
	/* heap sort: worst-case bounds, no recursion, no allocation */
	for (i = n / 2; i > 0; i--) {
		wl_sift_down(b, i, n, width, cmp);
	}
	for (i = n; i > 1; i--) {
		size_t k;

		for (k = 0; k < width; k++) {
			char t = b[k];

			b[k] = b[(i - 1) * width + k];
			b[(i - 1) * width + k] = t;
		}
		wl_sift_down(b, 1, i - 1, width, cmp);
	}
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
	/* the console is unbuffered; nothing to configure */
	(void) stream;
	(void) buf;
	(void) mode;
	(void) size;
	return 0;
}

/* newlib spells stdout/stderr as _impure_ptr->_stdout/_stderr. The
 * reent structure stays all-zero (stdout == NULL): the printf family
 * above ignores the stream argument, so nothing ever dereferences the
 * FILE pointers - this only satisfies the reference. */
#include <sys/reent.h>

static struct _reent wlan_reent;
struct _reent *_impure_ptr = &wlan_reent;

/* A scanf subset: the compiled hostap files only reach it through
 * ctrl-iface paths this image does not run, but the symbols must
 * resolve. Handles %d/%i/%u/%x/%s plus literals, spaces and %%;
 * anything else stops the parse. */
static int wl_hexval(int c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int sscanf(const char *str, const char *fmt, ...) {
	va_list ap;
	int assigned = 0;
	const char *s = str;

	va_start(ap, fmt);
	for (; *fmt != '\0'; fmt++) {
		if (*fmt != '%') {
			if (*fmt == ' ' || *fmt == '\t') {
				while (*s == ' ' || *s == '\t') {
					s++;
				}
				continue;
			}
			if (*s == *fmt) {
				s++;
			} else {
				break;
			}
			continue;
		}
		fmt++;
		if (*fmt == '%') {
			if (*s == '%') {
				s++;
			} else {
				break;
			}
			continue;
		}
		int suppress = 0;
		if (*fmt == '*') {
			suppress = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			fmt++;
		}
		if (*fmt == 'l' || *fmt == 'h') {
			fmt++;
		}
		switch (*fmt) {
		case 'd':
		case 'i': {
			long v = 0;
			int neg = 0;

			if (*s == '-') {
				neg = 1;
				s++;
			} else if (*s == '+') {
				s++;
			}
			if (*s < '0' || *s > '9') {
				goto out;
			}
			while (*s >= '0' && *s <= '9') {
				v = v * 10 + (*s++ - '0');
			}
			if (neg) {
				v = -v;
			}
			if (!suppress) {
				*va_arg(ap, int *) = (int) v;
				assigned++;
			}
			break;
		}
		case 'u': {
			unsigned long v = 0;

			if (*s < '0' || *s > '9') {
				goto out;
			}
			while (*s >= '0' && *s <= '9') {
				v = v * 10 + (unsigned) (*s++ - '0');
			}
			if (!suppress) {
				*va_arg(ap, unsigned *) = (unsigned) v;
				assigned++;
			}
			break;
		}
		case 'x': {
			unsigned long v = 0;
			int d = wl_hexval(*s);

			if (d < 0) {
				goto out;
			}
			while ((d = wl_hexval(*s)) >= 0) {
				v = v * 16 + (unsigned) d;
				s++;
			}
			if (!suppress) {
				*va_arg(ap, unsigned *) = (unsigned) v;
				assigned++;
			}
			break;
		}
		case 's': {
			char *out = NULL;

			if (!suppress) {
				out = va_arg(ap, char *);
			}
			while (*s == ' ' || *s == '\t') {
				s++;
			}
			while (*s != '\0' && *s != ' ' && *s != '\t') {
				if (out != NULL) {
					*out++ = *s;
				}
				s++;
			}
			if (out != NULL) {
				*out = '\0';
				assigned++;
			}
			break;
		}
		default:
			goto out;
		}
	}
out:
	va_end(ap);
	return assigned;
}
