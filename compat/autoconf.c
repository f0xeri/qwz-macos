/*
 * QwzWifi compat: the slice of OpenBSD autoconf(9) needed to run qwz's
 * cfattach (match/attach/detach) and config_mountroot(9) hook unchanged.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/task.h>
#include <sys/rwlock.h>
#include <sys/refcnt.h>
#include <sys/sockio.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

/* The same Linux helper macros qwz.c defines before its headers. */
#define BITS_PER_LONG		64
#define GENMASK(h, l) (((~0UL) >> (BITS_PER_LONG - (h) - 1)) & ((~0UL) << (l)))
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_GET(_m, _v) ((typeof(_m))(((_v) & (_m)) >> __bf_shf(_m)))
#define FIELD_PREP(_m, _v) (((typeof(_m))(_v) << __bf_shf(_m)) & (_m))
#define BIT(x)               (1UL << (x))

#include <dev/ic/qwzreg.h>
#include <dev/ic/qwzvar.h>

#include <qwzmac/attach.h>

extern const struct cfattach qwz_pci_ca;

/* qwz can only reach the lower 4 GB (qwz_dmamem_alloc: 0..0xffffffff). */
static struct bus_dma_tag qwz_dmat32 = { 0, 0xffffffffUL };

/* config_mountroot(9): run the hook in a thread and remember its state. */
static int mountroot_pending;	/* hooks scheduled but not finished */
static int mountroot_scheduled;	/* hooks ever scheduled */

struct mountroot_hook {
	struct device	*dev;
	void		(*fn)(struct device *);
};

static void
mountroot_report(struct device *dev)
{
	struct qwz_softc *sc = (struct qwz_softc *)dev;
	unsigned long n;
	unsigned long long bytes;

	qwzmac_dma_stats(&n, &bytes);
	if (sc->attached)
		printf("QwzWifi: stage 1 OK: firmware came up, MAC %s\n",
		    ether_sprintf(sc->sc_ic.ic_myaddr));
	else
		printf("QwzWifi: stage 1 FAILED: firmware did not come up "
		    "(see qwz0 messages above)\n");
	printf("QwzWifi: %lu DMA buffers, %llu KB still allocated\n",
	    n, bytes / 1024);
}

/*
 * Stage 2: scan only. This is "ifconfig qwz0 debug bssid <none> up":
 *  - IFF_DEBUG makes net80211's ieee80211_match_bss() print every network
 *    found (bssid, channel, RSSI, SSID) after each scan;
 *  - a desired BSSID that no AP has makes every candidate fail, so
 *    net80211 never joins anything and keeps rescanning (marked '!').
 * Called from the bring-up thread without the spl lock, like an ioctl.
 */
static void
stage2_scan(struct qwz_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ifreq ifr;
	int error;

	memset(ic->ic_des_bssid, 0, IEEE80211_ADDR_LEN);
	ic->ic_flags |= IEEE80211_F_DESBSSID;
	ifp->if_flags |= IFF_DEBUG;

	if_up(ifp);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_flags = ifp->if_flags;
	error = ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr);
	if (error)
		printf("QwzWifi: stage 2: interface up failed: %d\n", error);
	else
		printf("QwzWifi: stage 2: interface up, scanning "
		    "(scan only, networks are listed after each scan)\n");
}

static void
mountroot_thread(void *arg)
{
	struct mountroot_hook *h = arg;
	struct qwz_softc *sc = (struct qwz_softc *)h->dev;
	uint64_t t0 = getnsecuptime();

	printf("QwzWifi: firmware bring-up thread started\n");
	h->fn(h->dev);
	printf("QwzWifi: bring-up took %llu ms\n",
	    (unsigned long long)((getnsecuptime() - t0) / 1000000));
	mountroot_report(h->dev);
	if (sc->attached)
		stage2_scan(sc);

	qwzmac_spl_lock();
	mountroot_pending--;
	qwzmac_wakeup(&mountroot_pending, 0);
	qwzmac_spl_unlock();
	free(h, M_DEVBUF, sizeof(*h));
}

void
config_mountroot(struct device *dev, void (*fn)(struct device *))
{
	struct mountroot_hook *h;

	h = malloc(sizeof(*h), M_DEVBUF, M_WAITOK | M_CANFAIL);
	if (h == NULL) {
		printf("QwzWifi: config_mountroot: no memory\n");
		return;
	}
	h->dev = dev;
	h->fn = fn;
	qwzmac_spl_lock();
	mountroot_pending++;
	mountroot_scheduled++;
	qwzmac_spl_unlock();
	if (qwzmac_thread_start(mountroot_thread, h) != 0) {
		printf("QwzWifi: config_mountroot: no thread\n");
		qwzmac_spl_lock();
		mountroot_pending--;
		mountroot_scheduled--;
		qwzmac_spl_unlock();
		free(h, M_DEVBUF, sizeof(*h));
	}
}

static void
mountroot_wait(void)
{
	qwzmac_spl_lock();
	while (mountroot_pending > 0)
		qwzmac_sleep(&mountroot_pending, 0);
	qwzmac_spl_unlock();
}

/* Stop the device from mastering the bus: no more DMA, whatever its state. */
static void
pci_bus_master_off(pci_chipset_tag_t pc)
{
	pcireg_t reg;

	reg = pci_conf_read(pc, 0, PCI_COMMAND_STATUS_REG);
	if (reg & PCI_COMMAND_MASTER_ENABLE) {
		pci_conf_write(pc, 0, PCI_COMMAND_STATUS_REG,
		    reg & ~PCI_COMMAND_MASTER_ENABLE);
		printf("QwzWifi: bus mastering disabled\n");
	}
}

struct qwzmac_dev {
	struct device		*dev;
	pci_chipset_tag_t	 pc;
	int			 attached;	/* ca_attach got to the end */
};

int
qwzmac_qwz_attach(void *pcidev, void **devp)
{
	struct pci_attach_args pa;
	struct qwzmac_dev *qd;
	struct device *dev;
	int scheduled;

	memset(&pa, 0, sizeof(pa));
	pa.pa_pc = pcidev;
	pa.pa_tag = 0;
	pa.pa_dmat = &qwz_dmat32;
	pa.pa_dmat64 = &qwz_dmat32;
	pa.pa_id = pci_conf_read(pcidev, 0, PCI_ID_REG);
	pa.pa_class = pci_conf_read(pcidev, 0, PCI_CLASS_REG);
	pa.pa_subsys = pci_conf_read(pcidev, 0, PCI_SUBSYS_ID_REG);

	if (!qwz_pci_ca.ca_match(NULL, NULL, &pa)) {
		printf("QwzWifi: qwz does not match id 0x%08x\n", pa.pa_id);
		return ENODEV;
	}

	if (qwzmac_taskq_init() != 0)
		return ENOMEM;

	qd = malloc(sizeof(*qd), M_DEVBUF, M_WAITOK | M_CANFAIL | M_ZERO);
	dev = malloc(qwz_pci_ca.ca_devsize, M_DEVBUF,
	    M_WAITOK | M_CANFAIL | M_ZERO);
	if (qd == NULL || dev == NULL) {
		free(qd, M_DEVBUF, sizeof(*qd));
		free(dev, M_DEVBUF, qwz_pci_ca.ca_devsize);
		qwzmac_taskq_fini();
		return ENOMEM;
	}
	dev->dv_class = DV_IFNET;
	dev->dv_unit = 0;
	strlcpy(dev->dv_xname, "qwz0", sizeof(dev->dv_xname));
	qd->dev = dev;
	qd->pc = pcidev;

	printf("qwz0 at pci: attaching (softc %zu bytes)\n",
	    qwz_pci_ca.ca_devsize);
	scheduled = mountroot_scheduled;
	/*
	 * OpenBSD attaches devices while "cold", with interrupts blocked.
	 * Holding the spl lock keeps qwz_pci_intr() out until attach is done.
	 */
	qwzmac_spl_lock();
	qwz_pci_ca.ca_attach(NULL, dev, &pa);
	qwzmac_spl_unlock();
	/* qwz_pci_attach() schedules the hook only when it fully succeeded. */
	qd->attached = (mountroot_scheduled != scheduled);
	if (!qd->attached)
		printf("QwzWifi: qwz_pci_attach failed early\n");

	*devp = qd;
	return 0;
}

void
qwzmac_qwz_detach(void *p)
{
	struct qwzmac_dev *qd = p;

	if (qd == NULL)
		return;

	mountroot_wait();

	if (qd->attached) {
		struct ifnet *ifp =
		    &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;

		qwz_pci_ca.ca_detach(qd->dev, 0);
		pci_bus_master_off(qd->pc);
		/*
		 * QWZMAC: qwz's detach leaves net80211 attached (OpenBSD
		 * never detaches it in practice). Undo it, then stop every
		 * timeout and task before the softc that holds them goes.
		 */
		if_down(ifp);
		ieee80211_ifdetach(ifp);
		if_detach(ifp);
		qwzmac_timers_quiesce();
		qwzmac_taskq_fini();
		free(qd->dev, M_DEVBUF, qwz_pci_ca.ca_devsize);
	} else {
		/*
		 * A half-attached softc is not safe to detach; qwz already
		 * released what it could. Keep the device off the bus and
		 * leak the rest rather than risk freeing memory it may use.
		 */
		pci_bus_master_off(qd->pc);
		printf("QwzWifi: leaking half-attached softc\n");
		qwzmac_taskq_fini();
	}

	free(qd, M_DEVBUF, sizeof(*qd));
	printf("QwzWifi: qwz detached\n");
}

/*
 * System shutdown or restart: the chip may be scanning, i.e. writing into
 * host RAM. Stop it from mastering the bus so nothing it does can reach
 * memory the next boot stage reuses. No firmware teardown: it can block.
 */
void
qwzmac_qwz_shutdown(void *p)
{
	struct qwzmac_dev *qd = p;

	if (qd == NULL)
		return;
	pci_bus_master_off(qd->pc);
}
