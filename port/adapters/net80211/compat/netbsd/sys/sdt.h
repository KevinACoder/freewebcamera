/*
 * @file
 * @brief sdt(9) DTrace probes: compiled to no-ops, the shape NetBSD's
 * own sys/sdt.h takes when KDTRACE_HOOKS is off. usb_sdt.h needs the
 * vocabulary to exist.
 */

#ifndef _COMPAT_SYS_SDT_H_
#define _COMPAT_SYS_SDT_H_

#define SDT_PROVIDER_DEFINE(prov)
#define SDT_PROVIDER_DECLARE(prov)
#define SDT_MODULE_DEFINE(mod, prov)
#define SDT_MODULE_DECLARE(mod, prov)
/* variadic no-ops: the numbered DEFINE macros' arity differs between
 * NetBSD revisions and the imported .c files use them verbatim */
#define SDT_PROBE_DEFINE1(...)
#define SDT_PROBE_DEFINE2(...)
#define SDT_PROBE_DEFINE3(...)
#define SDT_PROBE_DEFINE4(...)
#define SDT_PROBE_DEFINE5(...)
#define SDT_PROBE_DEFINE6(...)
#define SDT_PROBE_DEFINE7(...)
#define SDT_PROBE_DEFINE8(...)
#define SDT_PROBE_DEFINE(...)
#define SDT_PROBE_DEFINE1_XLATE(...)
#define SDT_PROBE_DEFINE2_XLATE(...)
#define SDT_PROBE_DEFINE3_XLATE(...)
#define SDT_PROBE_DEFINE4_XLATE(...)
#define SDT_PROBE_DEFINE5_XLATE(...)
#define SDT_PROBE_ARGTYPE(...)
#define SDT_PROBE(prov, mod, ffn, name, arg0, arg1, arg2, arg3, arg4) \
	do { } while (0)
#define SDT_PROBE1(prov, mod, ffn, name, a0) do { } while (0)
#define SDT_PROBE2(prov, mod, ffn, name, a0, a1) do { } while (0)
#define SDT_PROBE3(prov, mod, ffn, name, a0, a1, a2) do { } while (0)
#define SDT_PROBE4(prov, mod, ffn, name, a0, a1, a2, a3) do { } while (0)
#define SDT_PROBE5(prov, mod, ffn, name, a0, a1, a2, a3, a4) do { } while (0)
#define SDT_PROBE6(prov, mod, ffn, name, a0, a1, a2, a3, a4, a5) \
	do { } while (0)
#define SDT_PROBE7(prov, mod, ffn, name, a0, a1, a2, a3, a4, a5, a6) \
	do { } while (0)
#define SDT_PROBE8(prov, mod, ffn, name, a0, a1, a2, a3, a4, a5, a6, a7) \
	do { } while (0)
#define SDT_PROBE_AVAILABLE(prov, mod, ffn, name) (0)

#endif /* _COMPAT_SYS_SDT_H_ */
