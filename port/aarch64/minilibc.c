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
 * The formatting engine is mpaland/printf (vendored at third-party/printf,
 * MIT, see that directory's PROVENANCE.md). It is a full C99 integer
 * formatter: '-'/'0'/'+'/' '/'#' flags, width AND precision (including the
 * '*' forms), h/hh/l/ll/z/j/t length modifiers, %d %i %u %x %X %o %b %c %s
 * %p %%. Float/exponential rendering is compiled out at the build rule - the
 * image is -mgeneral-regs-only, there is no FP state - and degrade like any
 * unknown specifier: the bare specifier character is emitted.
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
 * the standard signatures rather than to a local header. */

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

/* --- the formatter -------------------------------------------------------- *
 *
 * mpaland/printf (third-party/printf) is the engine. minilibc keeps the
 * str/mem surface above and owns the symbol contract: the standard-named
 * vsnprintf/snprintf/printf symbols stay here, so the freestanding
 * declarations across the tree (gicv3_its.c, lwip_diag.c, cherrysh, ...)
 * keep linking against the same names as before the swap.
 *
 * printf.h's standard-name #define aliases are unconditional, so this file
 * deliberately does NOT include it - the underscore realizations are
 * declared directly instead.
 */

extern int vsnprintf_(char *buf, size_t count, const char *fmt, va_list va);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	return vsnprintf_(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf_(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

extern void board_console_write(const char *message);

/* _putchar backs printf_/vprintf_/sprintf_ in printf.c. No call site on this
 * trunk uses them, but printf.o links them in, so the sink must exist. It
 * buffers exactly like weak printf below and drains through the same locked
 * console sink; the static buffer makes it shell-context-only, which is all
 * the alias-name paths ever get. */
void _putchar(char character)
{
	static char buf[256];
	static size_t len = 0U;

	buf[len++] = character;
	if (character == '\n' || len == sizeof(buf) - 1U) {
		buf[len] = '\0';
		board_console_write(buf);
		len = 0U;
	}
}

/* printf: the only bare call sites in the tree are tlsf's parameter-error
 * paths (all first-party reporting goes through snprintf/board_log). Routed
 * through the locked console sink - declared here ad hoc like the other
 * board seams, so this file keeps freestanding includes only. */
__attribute__((weak)) int printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf_(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	board_console_write(buf);
	return n;
}
