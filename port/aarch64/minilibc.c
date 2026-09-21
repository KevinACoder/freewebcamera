/*
 * @file   minilibc.c
 * @brief  String and formatting functions the shell needs but libc would supply.
 *
 * memops.c covers what the kernel itself calls. This file covers what the shell
 * calls: CherrySH uses strnlen, memchr and a printf-family formatter, and its
 * builtin `shsize` uses atoi; CherryUSB's builtin `lsusb` parses its option
 * arguments with strtol. The build is -nostdlib, so without these the shell
 * fails to link - and a shell that cannot format cannot print a prompt, a
 * version string or an error, which makes it useless for bring-up.
 *
 * The formatter is deliberately a subset, not a general printf. It supports
 * exactly what CherrySH and this project's commands use:
 *
 *   %s %c %d %i %u %x %X %p %%     with '-' and '0' flags, a width, and an
 *                                  optional 'l'/'z' length modifier
 *
 * Anything else is emitted literally rather than guessed at. That matters: a
 * formatter that silently renders an unsupported specifier as something
 * plausible hides bugs in the caller, whereas an unhandled `%q` appearing
 * verbatim in the output is unmistakable.
 *
 * Truncation follows C99: the result is always NUL-terminated when size > 0,
 * and the RETURN value is the length the full output would have had, so callers
 * can detect that they were cut off.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The prototypes these implement are declared by the vendored headers that use
 * them (cherrysh includes <string.h>), so the definitions here are matched to
the standard signatures rather than to a local header. */

/* --- small helpers -------------------------------------------------------- */

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;

	if (s == NULL) {
		return 0U;
	}
	while (n < max && s[n] != '\0') {
		n++;
	}
	return n;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char want = (unsigned char)c;
	size_t i;

	if (p == NULL) {
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (p[i] == want) {
			return (void *)(uintptr_t)&p[i];
		}
	}
	return NULL;
}

int strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
		}
		if (a[i] == '\0') {
			return 0;
		}
	}
	return 0;
}

char *strncpy(char *dest, const char *src, size_t n)
{
	size_t i = 0;

	while (i < n && src[i] != '\0') {
		dest[i] = src[i];
		i++;
	}
	/* strncpy pads with NULs up to n; callers that use it as "copy at most n"
	 * rely on that, and the shell does. */
	while (i < n) {
		dest[i] = '\0';
		i++;
	}
	return dest;
}

/* strchr returns the FIRST occurrence. Note the comparison must happen before
 * the advance, or strchr(s, 0) misses the terminator. */
char *strchr(const char *s, int c)
{
	char want = (char)c;

	for (;; s++) {
		if (*s == want) {
			return (char *)(uintptr_t)s;
		}
		if (*s == '\0') {
			return NULL;
		}
	}
}

int atoi(const char *s)
{
	int value = 0;
	int negative = 0;

	if (s == NULL) {
		return 0;
	}
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (*s == '-') {
		negative = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		value = value * 10 + (*s - '0');
		s++;
	}
	return negative ? -value : value;
}

/* Long form with radix selection, for vendored code that parses option
 * arguments (CherryUSB's lsusb): base 0 auto-detects 0x/0 prefixes, base 16
 * takes an optional 0x, base 8/10 are literal. No overflow clamping - the
 * callers here parse short device IDs, and saturating them silently would
 * hide malformed input rather than surface it. */
long strtol(const char *s, char **endptr, int base)
{
	const char *begin = s;
	unsigned long value = 0;
	int negative = 0;
	int any = 0;

	if (s == NULL) {
		if (endptr != NULL) {
			*endptr = NULL;
		}
		return 0;
	}
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (*s == '-') {
		negative = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	if ((base == 0 || base == 16) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = (s[0] == '0') ? 8 : 10;
	}

	for (;; s++) {
		int digit;

		if (*s >= '0' && *s <= '9') {
			digit = *s - '0';
		} else if (*s >= 'a' && *s <= 'z') {
			digit = *s - 'a' + 10;
		} else if (*s >= 'A' && *s <= 'Z') {
			digit = *s - 'A' + 10;
		} else {
			break;
		}
		if (digit >= base) {
			break;
		}
		value = value * (unsigned long)base + (unsigned long)digit;
		any = 1;
	}

	if (endptr != NULL) {
		*endptr = (char *)(uintptr_t)(any ? s : begin);
	}
	return negative ? -(long)value : (long)value;
}

/* --- output sink ---------------------------------------------------------- */

typedef struct {
	char  *buf;
	size_t size;	/* capacity, including the terminating NUL */
	size_t pos;	/* characters the full output would have produced */
} out_t;

static void out_char(out_t *o, char c)
{
	/* Only the first size-1 characters are stored; pos keeps counting so
	 * the return value reports the intended length. */
	if (o->size > 0U && o->pos < (o->size - 1U)) {
		o->buf[o->pos] = c;
	}
	o->pos++;
}

static void out_str(out_t *o, const char *s, size_t maxlen)
{
	size_t i;

	if (s == NULL) {
		/* Rendering NULL as "(null)" is the common convention and is
		 * more useful than crashing or printing nothing. */
		s = "(null)";
		maxlen = 6U;
	}
	for (i = 0; i < maxlen && s[i] != '\0'; i++) {
		out_char(o, s[i]);
	}
}

/* Emit `digits` characters of `value` in `base`, respecting pad/zero flags.
 * `upper` selects uppercase hex digits. */
static void out_uint(out_t *o, unsigned long long value, unsigned base,
		     int width, int left_align, int zero_pad, int upper,
		     int prefix)
{
	char tmp[24];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;
	int i;

	if (value == 0ULL) {
		tmp[n++] = '0';
	} else {
		while (value != 0ULL && n < (int)sizeof(tmp)) {
			tmp[n++] = digits[value % (unsigned long long)base];
			value /= (unsigned long long)base;
		}
	}

	/* A prefix counts toward the field width, as printf does. */
	i = n + prefix;
	if (left_align) {
		if (prefix == 2) {
			out_char(o, '0');
			out_char(o, 'x');
		}
		while (n > 0) {
			out_char(o, tmp[--n]);
		}
		for (; i < width; i++) {
			out_char(o, ' ');
		}
		return;
	}

	if (zero_pad) {
		if (prefix == 2) {
			out_char(o, '0');
			out_char(o, 'x');
		}
		for (; i < width; i++) {
			out_char(o, '0');
		}
	} else {
		for (; i < width; i++) {
			out_char(o, ' ');
		}
		if (prefix == 2) {
			out_char(o, '0');
			out_char(o, 'x');
		}
	}
	while (n > 0) {
		out_char(o, tmp[--n]);
	}
}

static void out_int(out_t *o, long long value, int width, int left_align,
		    int zero_pad)
{
	unsigned long long magnitude;
	int negative = 0;
	char tmp[24];
	int n = 0;
	int i;

	if (value < 0) {
		negative = 1;
		/* Negate in unsigned space so LLONG_MIN cannot overflow. */
		magnitude = (unsigned long long)(-(value + 1)) + 1ULL;
	} else {
		magnitude = (unsigned long long)value;
	}

	if (magnitude == 0ULL) {
		tmp[n++] = '0';
	} else {
		while (magnitude != 0ULL && n < (int)sizeof(tmp)) {
			tmp[n++] = (char)('0' + (int)(magnitude % 10ULL));
			magnitude /= 10ULL;
		}
	}

	i = n + (negative ? 1 : 0);
	if (left_align) {
		if (negative) {
			out_char(o, '-');
		}
		while (n > 0) {
			out_char(o, tmp[--n]);
		}
		for (; i < width; i++) {
			out_char(o, ' ');
		}
		return;
	}

	if (zero_pad) {
		if (negative) {
			out_char(o, '-');
		}
		for (; i < width; i++) {
			out_char(o, '0');
		}
	} else {
		for (; i < width; i++) {
			out_char(o, ' ');
		}
		if (negative) {
			out_char(o, '-');
		}
	}
	while (n > 0) {
		out_char(o, tmp[--n]);
	}
}

/* --- the formatter -------------------------------------------------------- */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	out_t o;
	int left_align;
	int zero_pad;
	int width;
	int is_long;
	int is_short;

	o.buf = buf;
	o.size = size;
	o.pos = 0U;

	if (fmt == NULL) {
		if (size > 0U) {
			buf[0] = '\0';
		}
		return 0;
	}

	while (*fmt != '\0') {
		if (*fmt != '%') {
			out_char(&o, *fmt++);
			continue;
		}
		fmt++;			/* consume '%' */

		if (*fmt == '%') {
			out_char(&o, '%');
			fmt++;
			continue;
		}

		/* flags */
		left_align = 0;
		zero_pad = 0;
		for (;;) {
			if (*fmt == '-') {
				left_align = 1;
				fmt++;
			} else if (*fmt == '0') {
				zero_pad = 1;
				fmt++;
			} else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') {
				/* Accepted and ignored: none of this project's
				 * output depends on them, and ignoring them is
				 * better than printing them literally. */
				fmt++;
			} else {
				break;
			}
		}

		/* width (a literal '*' is handled as "unspecified" rather than
		 * consuming an argument, since nothing here uses it) */
		width = 0;
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}

		/* length modifier */
		is_long = 0;
		is_short = 0;
		if (*fmt == 'l' || *fmt == 'z') {
			is_long = 1;
			fmt++;
			if (*fmt == 'l') {
				fmt++;	/* 'll': same as long here */
			}
		} else if (*fmt == 'h') {
			is_short = 1;
			fmt++;
			if (*fmt == 'h') {
				fmt++;
			}
		}

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			size_t len = (s != NULL) ? strnlen(s, (size_t)-1) : 0U;
			int pad = (width > 0) ? width - (int)len : 0;

			if (!left_align) {
				while (pad-- > 0) {
					out_char(&o, ' ');
				}
			}
			out_str(&o, s, len);
			if (left_align) {
				while (pad-- > 0) {
					out_char(&o, ' ');
				}
			}
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			int pad = (width > 0) ? width - 1 : 0;

			if (!left_align) {
				while (pad-- > 0) {
					out_char(&o, ' ');
				}
			}
			out_char(&o, c);
			if (left_align) {
				while (pad-- > 0) {
					out_char(&o, ' ');
				}
			}
			break;
		}
		case 'd':
		case 'i': {
			long long v;

			if (is_long) {
				v = va_arg(ap, long long);
			} else {
				v = (long long)va_arg(ap, int);
				if (is_short) {
					v = (long long)(short)v;
				}
			}
			out_int(&o, v, width, left_align, zero_pad);
			break;
		}
		case 'u': {
			unsigned long long v = is_long
				? va_arg(ap, unsigned long long)
				: (unsigned long long)va_arg(ap, unsigned int);

			out_uint(&o, v, 10U, width, left_align, zero_pad, 0, 0);
			break;
		}
		case 'x':
		case 'X': {
			unsigned long long v = is_long
				? va_arg(ap, unsigned long long)
				: (unsigned long long)va_arg(ap, unsigned int);
			int upper = (*fmt == 'X');

			out_uint(&o, v, 16U, width, left_align, zero_pad, upper, 0);
			break;
		}
		case 'p': {
			void *p = va_arg(ap, void *);

			out_str(&o, "0x", 2U);
			out_uint(&o, (unsigned long long)(uintptr_t)p, 16U,
				 0, 0, 1, 0, 0);
			break;
		}
		case '\0':
			/* A trailing '%' at end of string: print it rather than
			 * reading past the terminator. */
			out_char(&o, '%');
			break;
		default:
			/* Unknown specifier: show it verbatim so the caller's
			 * mistake is visible instead of silently swallowed. */
			out_char(&o, '%');
			out_char(&o, *fmt);
			break;
		}

		if (*fmt != '\0') {
			fmt++;
		}
	}

	if (size > 0U) {
		size_t end = (o.pos < (size - 1U)) ? o.pos : (size - 1U);

		buf[end] = '\0';
	}
	return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
