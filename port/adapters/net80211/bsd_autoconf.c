/*
 * @file
 * @brief config(9) mini-framework and the softint(9) thread backend.
 *
 * Plays the role of the generated ioconf.c plus the parts of
 * subr_autoconf.c the compiled NetBSD set reaches: a static cfdata
 * table (usb / uroothub / uhub / urtwn, gated by the config_found
 * iattr the same way the real bus attributes gate them), device shell
 * allocation, and the deferred config_interrupts/config_defer hooks.
 * The usb/uhub/ehci/urtwn cfdriver definitions live here because
 * NetBSD generates them at kernel-build time from the config file.
 *
 * softint(9): handlers run on one worker thread woken by a flag set
 * (the ISR itself only schedules), the D49 discipline.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

#include <sys/types.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/select.h>
#include <sys/compat_stub.h>

#include "cmsis_os2.h"

/* provided by wlan_adapter.c (the same primitive the osal uses) */
extern void *wlan_port_thread_create(void (*run)(void *), void *arg);

#ifndef __arraycount
#define __arraycount(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* match score floor (usbdi.h spells the rest) */
#define UMATCH_NONE 0

struct softint;

int cold; /* cleared once the shell is up (wlan world boots shell-first) */

/*
 * "Are we in an interrupt?"  ThreadX's Cortex-A port maintains the ISR
 * nesting count in _tx_thread_system_state (the context-save vector
 * increments it, context-restore decrements it), which is precisely the
 * predicate NetBSD's sys/intr.h wants - the imported net80211 core
 * asserts !cpu_intr_p() at two points that were compiled out until the
 * KASSERT family became live.
 */
extern volatile unsigned long _tx_thread_system_state;

bool cpu_intr_p(void) {
	return _tx_thread_system_state != 0UL;
}

struct kmutex *proc_lock(struct proc *p) {
	(void) p;
	return NULL;
}

/* the compat hooks exist but nothing registers: hooked stays false and
 * MODULE_HOOK_CALL falls to its enosys default */
struct usb_subr_fill_30_hook_t usb_subr_fill_30_hook = { .hooked = false };
struct usb_subr_copy_30_hook_t usb_subr_copy_30_hook = { .hooked = false };

/* ------------------------------------------------------------------
 * cfdrivers: ioconf.c would generate these from the config file
 */

static device_t usb_devs[2];
static device_t uroothub_devs[2];
static device_t uhub_devs[4];
static device_t ehci_devs[2];
static device_t urtwn_devs[2];

struct cfdriver usb_cd = {
	.cd_devs = usb_devs,
	.cd_name = "usb",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

struct cfdriver uroothub_cd = {
	.cd_devs = uroothub_devs,
	.cd_name = "uroothub",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

struct cfdriver uhub_cd = {
	.cd_devs = uhub_devs,
	.cd_name = "uhub",
	.cd_class = DV_DULL,
	.cd_ndevs = 4,
};

struct cfdriver ehci_cd = {
	.cd_devs = ehci_devs,
	.cd_name = "ehci",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

struct cfdriver urtwn_cd = {
	.cd_devs = urtwn_devs,
	.cd_name = "urtwn",
	.cd_class = DV_NET,
	.cd_ndevs = 2,
};

extern const struct cfattach usb_ca;
extern const struct cfattach uroothub_ca;
extern const struct cfattach uhub_ca;
extern const struct cfattach urtwn_ca;

/* the port's post-attach hook (urtwn_reg.c): registers the driver with
 * the shell-facing adapter table */
void wlan_port_post_attach(device_t dev);

/* the usb event-device selection shells (no device table here) */
int root_is_mounted;

/* usbdebug is defined by usb.c now that USB_DEBUG is on (opt_usb.h is
 * force-included); the bring-up still raises the level explicitly */

const char ostype[] = "NetBSD";
const char osrelease[] = "11.0";
const char version[] = "freewebcamera net80211 carrier (netbsd-11 import)";

size_t strlcpy(char *dst, const char *src, size_t size) {
	size_t n = 0;

	if (src == NULL) {
		return 0;
	}
	while (src[n] != '\0') {
		n++;
	}
	if (size != 0) {
		size_t i;

		for (i = 0; i + 1 < size && i < n; i++) {
			dst[i] = src[i];
		}
		dst[i] = '\0';
	}
	return n;
}

void selinit(struct selinfo *sip) {
	(void) sip;
}

void selrecord(struct lwp *l, struct selinfo *sip) {
	(void) l; (void) sip;
}

void selnotify(struct selinfo *sip, int events, long knoteflags) {
	(void) sip; (void) events; (void) knoteflags;
}

void selrecord_knote(struct selinfo *sip, struct knote *kn) {
	(void) sip; (void) kn;
}

void selremove_knote(struct selinfo *sip, struct knote *kn) {
	(void) sip; (void) kn;
}

int nowrite(dev_t dev, struct uio *uio, int ioflag) {
	(void) dev; (void) uio; (void) ioflag;
	return ENODEV;
}

int nostop(struct tty *tp, int flags) {
	(void) tp; (void) flags;
	return 0;
}

int nodiscard(dev_t dev, off_t pos, off_t len) {
	(void) dev; (void) pos; (void) len;
	return 0;
}

paddr_t nommap(dev_t dev, off_t pos, int flags) {
	(void) dev; (void) pos; (void) flags;
	return 0;
}

/* ------------------------------------------------------------------
 * the cfdata table: (iattr gate, cfdata, cfdriver)
 */

struct cfentry {
	const char *ce_iattr;	/* config_found iattr that may claim it */
	cfdata_t ce_cf;
	struct cfdriver *ce_cd;
};

static int dlocs_zero[1] = { -1 };

static struct cfdata cfdata_usb = {
	.cf_name = "usb", .cf_atname = "usb",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfdata cfdata_uroothub = {
	.cf_name = "uroothub", .cf_atname = "uroothub",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfdata cfdata_uhub = {
	.cf_name = "uhub", .cf_atname = "uhub",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfdata cfdata_urtwn = {
	.cf_name = "urtwn", .cf_atname = "urtwn",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfentry cfentries[] = {
	{ "usbus", &cfdata_usb, &usb_cd },
	{ "usbroothubif", &cfdata_uroothub, &uroothub_cd },
	{ "usbdevif", &cfdata_uhub, &uhub_cd },
	{ "usbdevif", &cfdata_urtwn, &urtwn_cd },
};

/* ------------------------------------------------------------------
 * device shells
 */

static device_t
dev_alloc(struct cfdriver *cd, cfdata_t cf)
{
	device_t dev;
	int unit = -1;
	int i;

	for (i = 0; i < cd->cd_ndevs; i++) {
		if (cd->cd_devs[i] == NULL) {
			unit = i;
			break;
		}
	}
	if (unit == -1) {
		return NULL;
	}

	dev = kmem_zalloc(sizeof(*dev), KM_SLEEP);
	if (dev == NULL) {
		return NULL;
	}
	dev->dv_unit = unit;
	dev->dv_cfdata = cf;
	snprintf(dev->dv_xname, sizeof(dev->dv_xname), "%s%d",
	    cf->cf_name, unit);
	cd->cd_devs[unit] = dev;
	return dev;
}

static struct cfattach *
cfattach_lookup(const char *atname)
{
	if (strcmp(atname, "usb") == 0) {
		return __DECONST(struct cfattach *, &usb_ca);
	}
	if (strcmp(atname, "uroothub") == 0) {
		return __DECONST(struct cfattach *, &uroothub_ca);
	}
	if (strcmp(atname, "uhub") == 0) {
		return __DECONST(struct cfattach *, &uhub_ca);
	}
	if (strcmp(atname, "urtwn") == 0) {
		return __DECONST(struct cfattach *, &urtwn_ca);
	}
	return NULL;
}

/* ------------------------------------------------------------------
 * config(9)
 */

static device_t
config_attach_internal(device_t parent, cfdata_t cf, void *aux,
	cfprint_t print, const struct cfargs *cfargs,
	struct cfdriver *cd)
{
	struct cfattach *ca;
	device_t dev;

	ca = cfattach_lookup(cf->cf_atname);
	if (ca == NULL) {
		panic("config_attach: no cfattach for %s", cf->cf_atname);
	}

	dev = dev_alloc(cd, cf);
	if (dev == NULL) {
		return NULL;
	}
	dev->dv_parent = parent;

	/* allocate the softc the ca_attach expects as device_private */
	dev->dv_private = kmem_zalloc(ca->ca_devsize, KM_SLEEP);
	if (dev->dv_private == NULL) {
		cd->cd_devs[dev->dv_unit] = NULL;
		return NULL;
	}

	if (print != NULL) {
		print(aux, device_xname(dev));
	}

	ca->ca_attach(parent, dev, aux);

	/*
	 * The attach is the only place a driver becomes usable, so this is
	 * where the port's adapter table learns about it (urtwn_reg.c's
	 * wlan_port_post_attach registers the shell-facing adapter).  It was
	 * declared but never called, which left every enumeration looking
	 * healthy in the log while `wlan scan` answered "no adapter": the
	 * device attached, nothing told the port core.
	 */
	wlan_port_post_attach(dev);
	return dev;
}

device_t
config_found(device_t parent, void *aux, cfprint_t print,
	const struct cfargs *cfargs)
{
	const char *iattr = NULL;
	size_t i;

	if (cfargs != NULL) {
		iattr = cfargs->iattr;
	}

	for (i = 0; i < __arraycount(cfentries); i++) {
		struct cfentry *ce = &cfentries[i];
		struct cfattach *ca;
		int score;

		if (iattr != NULL && ce->ce_iattr != NULL &&
		    strcmp(iattr, ce->ce_iattr) != 0) {
			continue;
		}

		ca = cfattach_lookup(ce->ce_cf->cf_atname);
		if (ca == NULL || ca->ca_match == NULL) {
			continue;
		}
		score = ca->ca_match(parent, ce->ce_cf, aux);
		if (score <= UMATCH_NONE) {
			continue;
		}
		return config_attach_internal(parent, ce->ce_cf, aux,
		    print, cfargs, ce->ce_cd);
	}

	if (print != NULL) {
		print(aux, device_xname(parent));
	}
	return NULL;
}

cfdata_t
config_search(device_t parent, void *aux, const struct cfargs *cfargs)
{
	/* direct configuration only: the static table is walked by
	 * config_found; nothing searches */
	return NULL;
}

int
config_stdsubmatch(device_t parent, cfdata_t cf, const int *locs, void *aux)
{
	/* every entry in the static table is already driver-specific */
	return 1;
}

device_t
config_attach(device_t parent, cfdata_t cf, void *aux, cfprint_t print,
	const struct cfargs *cfargs)
{
	/* used only by explicit-attach paths; walk back to the driver */
	size_t i;

	for (i = 0; i < __arraycount(cfentries); i++) {
		if (cfentries[i].ce_cf == cf) {
			return config_attach_internal(parent, cf, aux,
			    print, cfargs, cfentries[i].ce_cd);
		}
	}
	return NULL;
}

int
config_detach(device_t dev, int flags)
{
	return 0;
}

/* ------------------------------------------------------------------
 * deferred configuration: config_interrupts / config_defer hooks run
 * on one worker thread, in arrival order
 */

struct config_hook {
	device_t ch_dev;
	void (*ch_fn)(device_t);
	struct config_hook *ch_next;
};

static struct config_hook *config_hook_head, *config_hook_tail;
static volatile int config_hook_pending;
static void *config_worker;

static void config_worker_run(void *arg)
{
	(void) arg;

	for (;;) {
		struct config_hook *ch;

		while (config_hook_pending == 0) {
			osDelay(10);
		}
		config_hook_pending = 0;
		while (config_hook_head != NULL) {
			ch = config_hook_head;
			config_hook_head = ch->ch_next;
			if (config_hook_head == NULL) {
				config_hook_tail = NULL;
			}
			ch->ch_fn(ch->ch_dev);
			kmem_free(ch, sizeof(*ch));
		}
	}
}

static void
config_hook_enqueue(device_t dev, void (*fn)(device_t))
{
	struct config_hook *ch;

	ch = kmem_zalloc(sizeof(*ch), KM_SLEEP);
	if (ch == NULL) {
		panic("config: no memory for a deferred hook");
	}
	ch->ch_dev = dev;
	ch->ch_fn = fn;

	if (config_hook_tail != NULL) {
		config_hook_tail->ch_next = ch;
	} else {
		config_hook_head = ch;
	}
	config_hook_tail = ch;
	config_hook_pending = 1;
}

void config_defer(device_t dev, void (*fn)(device_t))
{
	config_hook_enqueue(dev, fn);
}

void config_interrupts(device_t dev, void (*fn)(device_t))
{
	config_hook_enqueue(dev, fn);
}

void config_mountroot(device_t dev, void (*fn)(device_t))
{
	/* firmware blobs are embedded; run synchronously */
	fn(dev);
}

void config_deferred_run(void)
{
	if (config_worker == NULL) {
		config_worker = wlan_port_thread_create(config_worker_run,
		    NULL);
	}
}

void config_pending_incr(device_t dev)
{
	(void) dev;
}

void config_pending_decr(device_t dev)
{
	(void) dev;
}

void config_init(void)
{
	config_deferred_run();
}

/* ------------------------------------------------------------------
 * softint(9): one worker, handlers drained in schedule order
 */

struct softint {
	void (*si_fn)(void *);
	void *si_arg;
};

struct softint_q {
	void (*sq_fn)(void *);
	void *sq_arg;
};

#define SOFTINT_Q_N 16
static struct softint_q softint_q[SOFTINT_Q_N];
static volatile unsigned softint_q_head, softint_q_tail;
static void *softint_worker;

static void softint_worker_run(void *arg)
{
	(void) arg;

	for (;;) {
		while (softint_q_head == softint_q_tail) {
			osDelay(1);
		}
		while (softint_q_head != softint_q_tail) {
			struct softint_q *q =
			    &softint_q[softint_q_head % SOFTINT_Q_N];
			void (*fn)(void *) = q->sq_fn;
			void *a = q->sq_arg;

			softint_q_head++;
			fn(a);
		}
	}
}

void
softint_disestablish(void *sih)
{
	kmem_free(sih, sizeof(struct softint));
}

void *
softint_establish(int flags, void (*func)(void *), void *arg)
{
	struct softint *sih;

	(void) flags;

	if (softint_worker == NULL) {
		softint_worker = wlan_port_thread_create(
		    softint_worker_run, NULL);
		if (softint_worker == NULL) {
			return NULL;
		}
	}
	sih = kmem_zalloc(sizeof(*sih), KM_SLEEP);
	if (sih == NULL) {
		return NULL;
	}
	sih->si_fn = func;
	sih->si_arg = arg;
	return sih;
}

void
softint_schedule(void *sih)
{
	struct softint *si = sih;
	struct softint_q *q;

	if (((softint_q_tail + 1) % SOFTINT_Q_N) ==
	    (softint_q_head % SOFTINT_Q_N)) {
		/* queue full: drop; the usb watchdogs recover */
		return;
	}
	q = &softint_q[softint_q_tail % SOFTINT_Q_N];
	q->sq_fn = si->si_fn;
	q->sq_arg = si->si_arg;
	softint_q_tail++;
}
