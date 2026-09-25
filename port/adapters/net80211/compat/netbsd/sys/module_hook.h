/*
 * @file
 * @brief module_hook(9) shell: dev_verbose.h routes the usb vendor and
 * product string lookups through two hooks. No module registers on
 * this carrier: the hooks stay tentative .bss objects (hooked = false)
 * and MODULE_HOOK_CALL falls to the caller's default expression.
 */

#ifndef _SYS_MODULE_HOOK_H
#define _SYS_MODULE_HOOK_H

#include <sys/cdefs.h>
#include <stdbool.h>

struct localcount;

void module_hook_init(void);
void module_hook_set(bool *, struct localcount *);
void module_hook_unset(bool *, struct localcount *);
bool module_hook_tryenter(bool *, struct localcount *);
void module_hook_exit(struct localcount *);

#define MODULE_HOOK(hook, type, args) \
struct hook ## _t { \
	struct localcount *	lc; \
	type			(*f)args; \
	bool			hooked; \
} hook

#define MODULE_HOOK_CALL(hook, args, default, ret) \
	((hook).hooked ? ((ret) = (hook).f args) : ((ret) = (default)))

#endif /* _SYS_MODULE_HOOK_H */
