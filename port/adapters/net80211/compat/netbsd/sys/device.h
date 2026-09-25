/*
 * @file
 * @brief autoconf(9)/device(9) shell matching the netbsd-11 ABI.
 *
 * The imported NetBSD sources (usbdi, usb_subr, uhub, ehci, if_urtwn)
 * expand CFATTACH_DECL_NEW/CFARGS and call the config_* API against
 * this header, so the structures and macro shapes follow sys/sys/device.h
 * of the pinned tree (field-for-field where the drivers reach them).
 * The implementation lives in bsd_autoconf.c: a static cfdata table
 * plays the role of the generated ioconf.c, including the four cfdriver
 * definitions (usb/uhub/uroothub/ehci/urtwn) that NetBSD otherwise
 * generates at kernel-build time.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#ifndef _COMPAT_SYS_DEVICE_H_
#define _COMPAT_SYS_DEVICE_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/callout.h>
#include <sys/queue.h>

struct cfdata;
struct cfdriver;
struct cfattach;
struct device;

typedef struct device *device_t;
typedef struct cfdata *cfdata_t;
typedef struct cfdriver *cfdriver_t;
typedef struct cfattach *cfattach_t;
typedef uintptr_t devhandle_t;

enum devclass {
	DV_DULL,		/* generic, no special class */
	DV_DISK,		/* mass storage */
	DV_NET,			/* network interface */
	DV_NETDDON,		/* network interface, done on detach */
	DV_TAPE,
	DV_TTY,
	DV_AUDIODEV,
	DV_DISKLABEL,
	DV_IFNET,
	DV_CPU
};
typedef enum devclass devclass_t;

enum devact {
	DVACT_ACTIVATE = 1,
	DVACT_DEACTIVATE = 2
};
typedef enum devact devact_t;
#define ACT_ACTIVATE	DVACT_ACTIVATE
#define ACT_DEACTIVATE	DVACT_DEACTIVATE

/* device flags (the ones the compiled set can see) */
#define DVF_PRIV_ALLOC		0x0001
#define DVF_DETACH_SHUTDOWN	0x0002
#define DVF_ACTIVE		0x0004

/* device shells handed out by config_attach; the imported drivers only
 * read xname/private and take the address */
struct device {
	char dv_xname[16];
	device_t dv_parent;
	cfdata_t dv_cfdata;
	void *dv_private;
	int dv_unit;
};

#define device_xname(d)		((d)->dv_xname)
#define device_private(d)	((d)->dv_private)
#define device_self(d)		((d))
#define device_unit(d)		((d)->dv_unit)
#define device_parent(d)	((d)->dv_parent)
#define device_is_active(d)	(true)
#define device_lookup_private(d, u) \
	(((u) < (d)->cd_ndevs) ? (d)->cd_devs[u] != NULL ? \
	 (d)->cd_devs[u]->dv_private : NULL : NULL)

/* ------------------------------------------------------------------
 * configuration data (ioconf.c shapes)
 */

struct cfparent {
	const char *cfp_name;	/* parent name or wildcard */
	int cfp_unit;		/* DVUNIT_ANY for wildcard */
};
#define DVUNIT_ANY	-1

struct cfdata {
	const char *cf_name;	/* driver name */
	const char *cf_atname;	/* attachment name */
	unsigned int cf_unit:24;
	unsigned char cf_fstate;
	int *cf_loc;
	int cf_flags;
	const struct cfparent *cf_pspec;
};
#define FSTATE_NOTFOUND		0
#define FSTATE_FOUND		1
#define FSTATE_STAR		2
#define FSTATE_DSTAR		3
#define FSTATE_DNOTFOUND	4

LIST_HEAD(cfattachlist, cfattach);
LIST_HEAD(cfdriverlist, cfdriver);

struct cfattach {
	const char *ca_name;
	LIST_ENTRY(cfattach) ca_list;
	size_t ca_devsize;
	int ca_flags;
	int (*ca_match)(device_t, cfdata_t, void *);
	void (*ca_attach)(device_t, device_t, void *);
	int (*ca_detach)(device_t, int);
	int (*ca_activate)(device_t, devact_t);
	int (*ca_rescan)(device_t, const char *, const int *);
	void (*ca_childdetached)(device_t, device_t);
};

/* External linkage on purpose: the autoconf glue references
 * <name>_ca/<name>_cd the way the generated ioconf.c does. */
#define	CFATTACH_DECL3_NEW(name, ddsize, matfn, attfn, detfn, actfn, \
	rescanfn, chdetfn, __flags) \
const struct cfattach __CONCAT(name, _ca) = { \
	.ca_name = ___STRING(name), \
	.ca_devsize = (ddsize), \
	.ca_flags = (__flags) | DVF_PRIV_ALLOC, \
	.ca_match = (matfn), \
	.ca_attach = (attfn), \
	.ca_detach = (detfn), \
	.ca_activate = (actfn), \
	.ca_rescan = (rescanfn), \
	.ca_childdetached = (chdetfn), \
}

#define	CFATTACH_DECL2_NEW(name, ddsize, matfn, attfn, detfn, actfn, \
	rescanfn, chdetfn) \
	CFATTACH_DECL3_NEW(name, ddsize, matfn, attfn, detfn, actfn, \
	    rescanfn, chdetfn, 0)

#define	CFATTACH_DECL_NEW(name, ddsize, matfn, attfn, detfn, actfn) \
	CFATTACH_DECL2_NEW(name, ddsize, matfn, attfn, detfn, actfn, \
	    NULL, NULL)

#define DETACH_FORCE	0x01
#define DETACH_QUIET	0x02
#define DETACH_SHUTDOWN	0x04
#define DETACH_POWEROFF	0x08

struct cfdriver {
	LIST_ENTRY(cfdriver) cd_list;
	struct cfattachlist cd_attach;
	device_t *cd_devs;
	const char *cd_name;
	devclass_t cd_class;
	int cd_ndevs;
	const void *cd_attrs;
};

#define	CFDRIVER_DECL(name, class, attrs) \
struct cfdriver __CONCAT(name, _cd) = { \
	.cd_name = ___STRING(name), \
	.cd_class = (class), \
	.cd_attrs = (attrs), \
}

/* ------------------------------------------------------------------
 * config_found/search arguments
 */

typedef int (*cfprint_t)(void *, const char *);
#define QUIET	0
#define UNCONF	1
#define UNSUPP	2

typedef int (*cfsubmatch_t)(device_t, cfdata_t, const int *, void *);
typedef int (*cfsearch_t)(device_t, cfdata_t, const int *, void *);

struct cfargs {
	uintptr_t cfargs_version;
	cfsubmatch_t submatch;
	cfsearch_t search;
	const char *iattr;
	const int *locators;
	devhandle_t devhandle;
};

#define CFARGS_VERSION	1
#define CFARGS_NONE	NULL

#define CFARGS(...) \
	&((const struct cfargs){ \
		.cfargs_version = CFARGS_VERSION, \
		__VA_ARGS__ \
	})

/* ------------------------------------------------------------------
 * config(9) API - implemented in bsd_autoconf.c
 */

void config_init(void);
cfdata_t config_search(device_t, void *, const struct cfargs *);
device_t config_found(device_t, void *, cfprint_t, const struct cfargs *);
device_t config_attach(device_t, cfdata_t, void *, cfprint_t,
	const struct cfargs *);
int config_detach(device_t, int);
int config_stdsubmatch(device_t, cfdata_t, const int *, void *);
void config_defer(device_t, void (*)(device_t));
void config_interrupts(device_t, void (*)(device_t));
void config_mountroot(device_t, void (*)(device_t));
void config_pending_incr(device_t);
void config_pending_decr(device_t);

/* deferred hooks run synchronously: the firmware blobs are embedded
 * and the config worker drains them right after */
void config_deferred_run(void);

/* ------------------------------------------------------------------
 * kernel-lock shells: the ports run without one
 */

#define KERNEL_LOCKED_P() 1
#define KERNEL_LOCK(c, l) ((void) (c))
#define KERNEL_UNLOCK_ONE(l) ((void) (l))
#define KERNEL_UNLOCK_ALL(l, o) ((void) (l))

/* ------------------------------------------------------------------
 * device printing
 */

#define aprint_normal_dev(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define aprint_error_dev(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define aprint_verbose_dev(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define aprint_debug_dev(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define aprint_naive_dev(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define device_printf(dev, fmt, ...) \
	printf("%s: " fmt, device_xname(dev), ##__VA_ARGS__)
#define aprint_normal(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define aprint_error(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define aprint_naive(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define aprint_verbose(fmt, ...) do { } while (0)
#define aprint_debug(fmt, ...) do { } while (0)

/* ------------------------------------------------------------------
 * proplib shells: the usb stack publishes device facts into a
 * dictionary nobody reads on this carrier (devmond territory)
 */

typedef void *prop_dictionary_t;
typedef void *prop_object_t;

static inline prop_dictionary_t device_properties(device_t dev) {
	(void) dev;
	return (prop_dictionary_t) 0;
}

static inline bool prop_dictionary_set_uint8(prop_dictionary_t dict,
	const char *key, uint8_t val) {
	(void) dict; (void) key; (void) val;
	return true;
}
static inline bool prop_dictionary_set_uint16(prop_dictionary_t dict,
	const char *key, uint16_t val) {
	(void) dict; (void) key; (void) val;
	return true;
}
static inline bool prop_dictionary_set_uint32(prop_dictionary_t dict,
	const char *key, uint32_t val) {
	(void) dict; (void) key; (void) val;
	return true;
}
static inline bool prop_dictionary_set_cstring(prop_dictionary_t dict,
	const char *key, const char *str) {
	(void) dict; (void) key; (void) str;
	return true;
}

/* ------------------------------------------------------------------
 * pmf(9) stubs: no power management on this carrier
 */

static inline int pmf_device_register(device_t dev,
	void (*suspend)(void *, int), void (*resume)(void *, int)) {
	(void) dev; (void) suspend; (void) resume;
	return 1;
}

static inline int pmf_device_register1(device_t dev,
	void (*suspend)(void *, int), void (*resume)(void *, int),
	void (*shutdown)(void *)) {
	(void) dev; (void) suspend; (void) resume; (void) shutdown;
	return 1;
}

static inline void pmf_device_deregister(device_t dev) {
	(void) dev;
}

static inline bool pmf_class_network_register(device_t dev, void *ifp) {
	(void) dev; (void) ifp;
	return true;
}

static inline bool device_is_a(device_t dev, const char *name) {
	return dev != NULL && strncmp(dev->dv_xname, name, strlen(name)) == 0;
}
static inline bool device_has_power(device_t dev) {
	(void) dev;
	return true;
}
static inline bool prop_dictionary_set_string(prop_dictionary_t dict,
	const char *key, const char *str) {
	(void) dict; (void) key; (void) str;
	return true;
}

#endif /* _COMPAT_SYS_DEVICE_H_ */
