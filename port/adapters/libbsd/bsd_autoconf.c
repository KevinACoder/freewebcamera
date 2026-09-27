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

/* The build configuration (generated).  Bus-level switches (CONFIG_BUS_*)
 * gate the host stacks, line-level ones (CONFIG_NIC_*, CONFIG_UVC/UAC) the
 * drivers that ride them - a distinction that matters because the camera
 * needs the USB stack without any USB wireless driver. */
#include "config.h"

#include <sys/types.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mutex.h>
#include <sys/pool.h>
#include <sys/reboot.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/select.h>
#include <sys/compat_stub.h>

#include "cmsis_os2.h"

/* provided by wlan_adapter.c (the same primitive the osal uses) */
extern void *wlan_port_thread_create(void (*run)(void *), void *arg);
/* the port serializer (osal_cmsis_rtos2.c, declared in port.h) */
extern void wlan_port_serializer_lock(void);
extern void wlan_port_serializer_unlock(void);

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

/* xhci.c asks this around its softint-driven completion paths; the
 * port's softint handlers run on a plain worker thread, so the answer
 * is always "no" there (the same shape cpu_intr_p's predicate has) */
bool cpu_softintr_p(void) {
	return false;
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

/* usb shells: ehci1's usbus + the xHCI's two buses (USB3 + USB2);
 * roothub shells: one per usbus, same count */
#if CONFIG_BUS_USB
static device_t usb_devs[4];
static device_t uroothub_devs[4];
static device_t uhub_devs[4];
static device_t ehci_devs[2];
static device_t xhci_devs[2];
#endif
#if CONFIG_NIC_URTWN
static device_t urtwn_devs[2];
#endif

#if CONFIG_BUS_USB
struct cfdriver usb_cd = {
	.cd_devs = usb_devs,
	.cd_name = "usb",
	.cd_class = DV_DULL,
	.cd_ndevs = 4,
};

struct cfdriver uroothub_cd = {
	.cd_devs = uroothub_devs,
	.cd_name = "uroothub",
	.cd_class = DV_DULL,
	.cd_ndevs = 4,
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

/* the xHCI attaches manually (usb_xhci_platform.c composes its softc
 * and populates cd_devs like ehci's attach does); the cfdata table
 * never matches it, only its usbus children */
struct cfdriver xhci_cd = {
	.cd_devs = xhci_devs,
	.cd_name = "xhci",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};
#endif /* CONFIG_BUS_USB */

#if CONFIG_NIC_URTWN
struct cfdriver urtwn_cd = {
	.cd_devs = urtwn_devs,
	.cd_name = "urtwn",
	.cd_class = DV_NET,
	.cd_ndevs = 2,
};
#endif /* CONFIG_NIC_URTWN */

#if CONFIG_UVC
/* the UVC line: uvideo claims the video-control interface (usbifif) and
 * hands each stream to the video(4) middle layer over videobus */
static device_t uvideo_devs[2];
static device_t video_devs[2];

struct cfdriver uvideo_cd = {
	.cd_devs = uvideo_devs,
	.cd_name = "uvideo",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

struct cfdriver video_cd = {
	.cd_devs = video_devs,
	.cd_name = "video",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};
#endif /* CONFIG_UVC */

#if CONFIG_UAC
/* the UAC line: uaudio claims the audio-control interface (the same
 * usbifif walk uvideo rides) and attaches audio(4) on top of it over
 * audiobus */
static device_t uaudio_devs[2];
static device_t audio_devs[2];

struct cfdriver uaudio_cd = {
	.cd_devs = uaudio_devs,
	.cd_name = "uaudio",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

struct cfdriver audio_cd = {
	.cd_devs = audio_devs,
	.cd_name = "audio",
	.cd_class = DV_AUDIODEV,
	.cd_ndevs = 2,
};
#endif /* CONFIG_UAC */

#if CONFIG_BUS_USB
extern const struct cfattach usb_ca;
extern const struct cfattach uroothub_ca;
extern const struct cfattach uhub_ca;
#endif
#if CONFIG_NIC_URTWN
extern const struct cfattach urtwn_ca;
#endif

/* the port's post-attach hook (urtwn_reg.c): registers the driver with
 * the shell-facing adapter table.  Only the urtwn line defines it. */
#if CONFIG_NIC_URTWN
void wlan_port_post_attach(device_t dev);
#endif

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

void seldestroy(struct selinfo *sip) {
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

/* the audio(4) cdev switch's unused entry points (upstream routes these
 * to devenodev); the port never dispatches through this switch - the
 * consumer drives the fileops path - so they answer ENODEV like nowrite */
int noclose(dev_t dev, int flags, int ifmt, struct lwp *l) {
	(void) dev; (void) flags; (void) ifmt; (void) l;
	return ENODEV;
}

int noread(dev_t dev, struct uio *uio, int ioflag) {
	(void) dev; (void) uio; (void) ioflag;
	return ENODEV;
}

int nopoll(dev_t dev, int events, struct lwp *l) {
	(void) dev; (void) events; (void) l;
	return 0;
}

int noioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l) {
	(void) dev; (void) cmd; (void) data; (void) flag; (void) l;
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

int nokqfilter(dev_t dev, struct knote *kn) {
	(void) dev; (void) kn;
	return 1;	/* EINVAL upstream shape: kqueue is not served */
}

/* the video(4) detach path only; see compat sys/conf.h */
devmajor_t cdevsw_lookup_major(const struct cdevsw *cdev) {
	(void) cdev;
	return 193;	/* the video major the native fork used */
}

void vdevgone(devmajor_t maj, int min1, int min2, int type) {
	(void) maj; (void) min1; (void) min2; (void) type;
}

/* the pmap face is a shell: the port is identity-mapped (virtual ==
 * physical - the whole bus_dma backend hands out and flushes raw
 * addresses), so extraction is the identity and there is no map */
pmap_t pmap_kernel(void) {
	return NULL;
}

int pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap) {
	(void) pm;
	*pap = (paddr_t) va;
	return 1;
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

#if CONFIG_BUS_USB
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
#endif /* CONFIG_BUS_USB */

#if CONFIG_NIC_URTWN
static struct cfdata cfdata_urtwn = {
	.cf_name = "urtwn", .cf_atname = "urtwn",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};
#endif /* CONFIG_NIC_URTWN */

#if CONFIG_UVC
static struct cfdata cfdata_uvideo = {
	.cf_name = "uvideo", .cf_atname = "uvideo",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfdata cfdata_video = {
	.cf_name = "video", .cf_atname = "video",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};
#endif /* CONFIG_UVC */

#if CONFIG_UAC
static struct cfdata cfdata_uaudio = {
	.cf_name = "uaudio", .cf_atname = "uaudio",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};

static struct cfdata cfdata_audio = {
	.cf_name = "audio", .cf_atname = "audio",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};
#endif /* CONFIG_UAC */

#if CONFIG_NIC_IWM
/* the pcie endpoint: the native glue (pcie_glue.c) drives the DesignWare
 * host directly through include/pcie.h and config_founds only the radio
 * driver - no fdt world, no pci bus core, no ppb descent */
static device_t iwm_devs[2];
struct cfdriver iwm_cd = {
	.cd_devs = iwm_devs,
	.cd_name = "iwm",
	.cd_class = DV_DULL,
	.cd_ndevs = 2,
};

static struct cfdata cfdata_iwm = {
	.cf_name = "iwm", .cf_atname = "iwm",
	.cf_fstate = FSTATE_STAR, .cf_loc = dlocs_zero,
};
#endif /* CONFIG_NIC_IWM */

static struct cfentry cfentries[] = {
#if CONFIG_BUS_USB
	{ "usbus", &cfdata_usb, &usb_cd },
	{ "usbroothubif", &cfdata_uroothub, &uroothub_cd },
	{ "usbdevif", &cfdata_uhub, &uhub_cd },
#endif
#if CONFIG_NIC_URTWN
	{ "usbdevif", &cfdata_urtwn, &urtwn_cd },
#endif
#if CONFIG_UVC
	/* uvideo matches on the video-control interface class, the same
	 * usbifif config_found usb_subr does per unclaimed interface */
	{ "usbifif", &cfdata_uvideo, &uvideo_cd },
	/* video_attach_mi config_founds the middle layer per stream */
	{ "videobus", &cfdata_video, &video_cd },
#endif
#if CONFIG_UAC
	/* uaudio matches on the audio-control interface class: same iattr,
	 * the earlier video entry answers NONE for it and the walk falls
	 * through to this one */
	{ "usbifif", &cfdata_uaudio, &uaudio_cd },
	/* audio_attach_mi config_founds the middle layer over audiobus */
	{ "audiobus", &cfdata_audio, &audio_cd },
#endif
#if CONFIG_NIC_IWM
	{ "pci", &cfdata_iwm, &iwm_cd },
#endif
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
	dev->dv_cd = cd;
	snprintf(dev->dv_xname, sizeof(dev->dv_xname), "%s%d",
	    cf->cf_name, unit);
	cd->cd_devs[unit] = dev;
	return dev;
}

static struct cfattach *
cfattach_lookup(const char *atname)
{
#if CONFIG_BUS_USB
	if (strcmp(atname, "usb") == 0) {
		return __DECONST(struct cfattach *, &usb_ca);
	}
	if (strcmp(atname, "uroothub") == 0) {
		return __DECONST(struct cfattach *, &uroothub_ca);
	}
	if (strcmp(atname, "uhub") == 0) {
		return __DECONST(struct cfattach *, &uhub_ca);
	}
#endif
#if CONFIG_NIC_URTWN
	if (strcmp(atname, "urtwn") == 0) {
		return __DECONST(struct cfattach *, &urtwn_ca);
	}
#endif
#if CONFIG_NIC_IWM
	if (strcmp(atname, "iwm") == 0) {
		/* CFATTACH_DECL_NEW(iwm, ...) inside iwm_reg.c's compiled
		 * import of if_iwm.c */
		extern const struct cfattach iwm_ca;

		return __DECONST(struct cfattach *, &iwm_ca);
	}
#endif
#if CONFIG_UVC
	if (strcmp(atname, "uvideo") == 0) {
		/* CFATTACH_DECL2_NEW(uvideo, ...) inside the verbatim
		 * uvideo.c (compiled straight from the submodule tree) */
		extern const struct cfattach uvideo_ca;

		return __DECONST(struct cfattach *, &uvideo_ca);
	}
	if (strcmp(atname, "video") == 0) {
		/* CFATTACH_DECL_NEW(video, ...) inside the verbatim
		 * sys/dev/video.c middle layer */
		extern const struct cfattach video_ca;

		return __DECONST(struct cfattach *, &video_ca);
	}
#endif
#if CONFIG_UAC
	if (strcmp(atname, "uaudio") == 0) {
		/* CFATTACH_DECL2_NEW(uaudio, ...) inside the verbatim
		 * uaudio.c (compiled straight from the submodule tree) */
		extern const struct cfattach uaudio_ca;

		return __DECONST(struct cfattach *, &uaudio_ca);
	}
	if (strcmp(atname, "audio") == 0) {
		/* CFATTACH_DECL3_NEW(audio, ...) inside the verbatim
		 * sys/dev/audio/audio.c middle layer */
		extern const struct cfattach audio_ca;

		return __DECONST(struct cfattach *, &audio_ca);
	}
#endif
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

	/* NetBSD's attach line is printed by the autoconf itself, as
	 * "<child> at <parent>", and the cfprint is then called with pnp ==
	 * NULL - a cfprint only spells out the auxiliary path for the
	 * UNCONFIGURED case, which config_found's failure path handles below.
	 * The port used to call print(aux, device_xname(dev)), so every USB
	 * attach read "usb at usb0" (child name in the parent slot) and the
	 * child's own continuation (": USB revision 2.0", ": <devinfo>") came
	 * out headless on the next line.  The console line assembler joins the
	 * head and the continuation into the one line NetBSD shows. */
	printf("%s at %s", device_xname(dev), device_xname(parent));
	if (print != NULL) {
		(void) print(aux, NULL);
	}

	ca->ca_attach(parent, dev, aux);

	/*
	 * The attach is the only place a driver becomes usable, so this is
	 * where the port's adapter table learns about it (urtwn_reg.c's
	 * wlan_port_post_attach registers the shell-facing adapter).  It was
	 * declared but never called, which left every enumeration looking
	 * healthy in the log while `wlan scan` answered "no adapter": the
	 * device attached, nothing told the port core.
	 *
	 * The hook lives with the urtwn line's adapter TU; the PCIe line's glue
	 * calls its own registration directly (pcie_glue.c). */
#if CONFIG_NIC_URTWN
	wlan_port_post_attach(dev);
#endif
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

/* the parent tracks no child list here; uvideo's detach path calls it
 * to tear its video children down, but those die with the unit shell */
int
config_detach_children(device_t dev, int flags)
{
	(void) dev; (void) flags;
	return 0;
}

/* audio(4)'s rescan walks the table with config_probe before
 * config_attach; the static table's entries are already driver-specific,
 * so every one the walk reaches matches (config_stdsubmatch's answer) */
int
config_probe(device_t parent, cfdata_t cf, void *aux)
{
	(void) parent; (void) cf; (void) aux;
	return 1;
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
			/* On NetBSD a softint handler runs at its IPL, so the
			 * driver's splnet() regions and its device softint are
			 * mutually exclusive by construction.  Nothing raises an
			 * IPL here, so the handler takes the port serializer:
			 * without it the iwm command completion (this thread) and
			 * iwm_send_cmd (shell/supplicant/state worker) interleave
			 * on the command ring. */
			wlan_port_serializer_lock();
			fn(a);
			wlan_port_serializer_unlock();
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
	static volatile unsigned softint_drops;
	struct softint *si = sih;
	struct softint_q *q;

	if (((softint_q_tail + 1) % SOFTINT_Q_N) ==
	    (softint_q_head % SOFTINT_Q_N)) {
		/* queue full: drop and count loudly, once (this runs on
		 * the EHCI interrupt, so the notice must be ISR-safe; a
		 * dropped entry loses an xfer wakeup and the old comment
		 * "the usb watchdogs recover" is hope, not a mechanism) */
		if (softint_drops++ == 0) {
			extern void board_early_print(const char *);

			board_early_print(
			    "wlan: SOFTINT QUEUE FULL, dropping xfer completions\n");
		}
		return;
	}
	q = &softint_q[softint_q_tail % SOFTINT_Q_N];
	q->sq_fn = si->si_fn;
	q->sq_arg = si->si_arg;
	softint_q_tail++;
}
