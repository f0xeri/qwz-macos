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
#include <net80211/ieee80211_ioctl.h>

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
#include <qwzmac/pbkdf2.h>

extern const struct cfattach qwz_pci_ca;

/* qwz can only reach the lower 4 GB (qwz_dmamem_alloc: 0..0xffffffff). */
static struct bus_dma_tag qwz_dmat32 = { 0, 0xffffffffUL };

/* Largest frame either way: 802.11 MTU (2304) plus the Ethernet header. */
#define QWZMAC_FRAME_MAX	(IEEE80211_MAX_LEN + ETHER_HDR_LEN)

struct qwzmac_dev {
	struct device		*dev;
	pci_chipset_tag_t	 pc;
	int			 attached;	/* ca_attach got to the end */
	struct qwzmac_netif_ops	 ops;		/* zeroed before detach */

	/* The network to join, PSK already derived. */
	int			 have_net;
	uint8_t			 ssid[IEEE80211_NWID_LEN];
	uint8_t			 ssid_len;
	uint8_t			 psk[32];

	uint8_t			 rxbuf[QWZMAC_FRAME_MAX]; /* spl lock */
};

/* There is one device; the mountroot hook only gets the softc. */
static struct qwzmac_dev *qwzmac_the_dev;

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

/* ifnet hooks (compat/if.c): frames and link state go to the kext. */
static void
qwzmac_if_input(struct ifnet *ifp, struct mbuf *m)
{
	struct qwzmac_dev *qd = ifp->if_qwzmac_cookie;
	int len = m->m_pkthdr.len;

	qwzmac_spl_lock();
	if (qd->ops.input == NULL || len < ETHER_HDR_LEN ||
	    len > (int)sizeof(qd->rxbuf)) {
		ifp->if_ierrors++;
		qwzmac_spl_unlock();
		m_freem(m);
		return;
	}
	m_copydata(m, 0, len, qd->rxbuf);
	m_freem(m);
	qd->ops.input(qd->ops.cookie, qd->rxbuf, len);
	qwzmac_spl_unlock();
}

static void
qwzmac_if_link_state(struct ifnet *ifp)
{
	struct qwzmac_dev *qd = ifp->if_qwzmac_cookie;
	struct ieee80211com *ic = (struct ieee80211com *)ifp;
	int up = ifp->if_link_state >= LINK_STATE_UP;

	if (up && ic->ic_bss != NULL) {
		char ssid[IEEE80211_NWID_LEN + 1];

		memcpy(ssid, ic->ic_bss->ni_essid, ic->ic_bss->ni_esslen);
		ssid[ic->ic_bss->ni_esslen] = '\0';
		printf("QwzWifi: stage 3: link up: \"%s\" bssid %s channel %u, "
		    "%llu Mbit/s\n", ssid, ether_sprintf(ic->ic_bss->ni_bssid),
		    ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan),
		    (unsigned long long)(ifp->if_baudrate / 1000000));
	} else
		printf("QwzWifi: link down\n");

	qwzmac_spl_lock();
	if (qd->ops.link != NULL)
		qd->ops.link(qd->ops.cookie, up, ifp->if_baudrate);
	qwzmac_spl_unlock();
}

static int
qwzmac_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	int error = ifp->if_ioctl(ifp, cmd, (caddr_t)data);

	/* Interface still down: settings apply when it comes up. */
	return error == ENETRESET ? 0 : error;
}

/*
 * "ifconfig qwz0 nwid <ssid> wpakey <passphrase>": join exactly this SSID
 * with WPA2-PSK (CCMP). Setting nwid also turns auto-join off.
 */
static int
qwzmac_set_network(struct qwzmac_dev *qd, struct ifnet *ifp)
{
	struct ieee80211_nwid nwid;
	struct ieee80211_wpaparams wpa;
	struct ieee80211_wpapsk psk;
	struct ifreq ifr;
	int error;

	memset(&nwid, 0, sizeof(nwid));
	nwid.i_len = qd->ssid_len;
	memcpy(nwid.i_nwid, qd->ssid, qd->ssid_len);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&nwid;
	if ((error = qwzmac_ioctl(ifp, SIOCS80211NWID, &ifr)) != 0)
		return error;

	memset(&wpa, 0, sizeof(wpa));
	strlcpy(wpa.i_name, ifp->if_xname, sizeof(wpa.i_name));
	wpa.i_enabled = 1;
	wpa.i_protos = IEEE80211_WPA_PROTO_WPA2;
	wpa.i_akms = IEEE80211_WPA_AKM_PSK;
	wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP;
	wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
	if ((error = qwzmac_ioctl(ifp, SIOCS80211WPAPARMS, &wpa)) != 0)
		return error;

	memset(&psk, 0, sizeof(psk));
	strlcpy(psk.i_name, ifp->if_xname, sizeof(psk.i_name));
	psk.i_enabled = 1;
	memcpy(psk.i_psk, qd->psk, sizeof(psk.i_psk));
	error = qwzmac_ioctl(ifp, SIOCS80211WPAPSK, &psk);
	explicit_bzero(&psk, sizeof(psk));
	return error;
}

/*
 * After stage 1: configure the network and bring the interface up, like
 * ifconfig would. IFF_DEBUG makes net80211 log the scan results, the
 * association and the 4-way handshake.
 * Without a network this is stage 2, scan only: a desired BSSID no AP has
 * makes every candidate fail, so net80211 keeps rescanning and joins
 * nothing. Called from the bring-up thread without the spl lock.
 */
static void
qwzmac_up(struct qwzmac_dev *qd)
{
	struct qwz_softc *sc = (struct qwz_softc *)qd->dev;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ifreq ifr;
	int error;

	ifp->if_flags |= IFF_DEBUG;
	if (qd->have_net) {
		char ssid[IEEE80211_NWID_LEN + 1];

		error = qwzmac_set_network(qd, ifp);
		explicit_bzero(qd->psk, sizeof(qd->psk));
		if (error) {
			printf("QwzWifi: stage 3: network setup failed: %d\n",
			    error);
			return;
		}
		memcpy(ssid, qd->ssid, qd->ssid_len);
		ssid[qd->ssid_len] = '\0';
		printf("QwzWifi: stage 3: joining \"%s\" (WPA2-PSK)\n", ssid);
	} else {
		memset(ic->ic_des_bssid, 0, IEEE80211_ADDR_LEN);
		ic->ic_flags |= IEEE80211_F_DESBSSID;
	}

	if_up(ifp);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_flags = ifp->if_flags;
	error = ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr);
	if (error)
		printf("QwzWifi: interface up failed: %d\n", error);
	else if (!qd->have_net)
		printf("QwzWifi: stage 2: interface up, scanning "
		    "(scan only, networks are listed after each scan)\n");
}

static void
mountroot_thread(void *arg)
{
	struct mountroot_hook *h = arg;
	struct qwz_softc *sc = (struct qwz_softc *)h->dev;
	struct qwzmac_dev *qd = qwzmac_the_dev;
	uint64_t t0 = getnsecuptime();

	printf("QwzWifi: firmware bring-up thread started\n");
	h->fn(h->dev);
	printf("QwzWifi: bring-up took %llu ms\n",
	    (unsigned long long)((getnsecuptime() - t0) / 1000000));
	mountroot_report(h->dev);
	if (sc->attached && qd != NULL) {
		if (qd->ops.ready != NULL)
			qd->ops.ready(qd->ops.cookie, sc->sc_ic.ic_myaddr);
		qwzmac_up(qd);
	}

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

/* Validates the network and derives the PSK (what ifconfig does). */
static int
qwzmac_take_network(struct qwzmac_dev *qd, struct qwzmac_network *net)
{
	size_t slen, plen;
	int error = 0;

	slen = strnlen(net->ssid, sizeof(net->ssid));
	plen = strnlen(net->passphrase, sizeof(net->passphrase));
	if (slen == 0 || slen > IEEE80211_NWID_LEN) {
		printf("QwzWifi: config: SSID must be 1..32 bytes\n");
		error = EINVAL;
	} else if (plen < 8 || plen > 63) {
		printf("QwzWifi: config: WPA2 passphrase must be 8..63 "
		    "characters\n");
		error = EINVAL;
	} else if (pkcs5_pbkdf2(net->passphrase, plen,
	    (const uint8_t *)net->ssid, slen, qd->psk, sizeof(qd->psk),
	    4096) != 0) {
		error = EINVAL;
	} else {
		memcpy(qd->ssid, net->ssid, slen);
		qd->ssid_len = slen;
		qd->have_net = 1;
	}
	explicit_bzero(net->passphrase, sizeof(net->passphrase));
	return error;
}

int
qwzmac_qwz_attach(void *pcidev, const struct qwzmac_netif_ops *ops,
    struct qwzmac_network *net, void **devp)
{
	struct pci_attach_args pa;
	struct qwzmac_dev *qd;
	struct device *dev;
	struct ifnet *ifp;
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
		if (net != NULL)
			explicit_bzero(net, sizeof(*net));
		return ENODEV;
	}

	qd = malloc(sizeof(*qd), M_DEVBUF, M_WAITOK | M_CANFAIL | M_ZERO);
	if (qd == NULL) {
		if (net != NULL)
			explicit_bzero(net, sizeof(*net));
		return ENOMEM;
	}
	if (net != NULL && qwzmac_take_network(qd, net) != 0)
		printf("QwzWifi: no usable network in the config, "
		    "scanning only\n");

	if (qwzmac_taskq_init() != 0) {
		free(qd, M_DEVBUF, sizeof(*qd));
		return ENOMEM;
	}
	dev = malloc(qwz_pci_ca.ca_devsize, M_DEVBUF,
	    M_WAITOK | M_CANFAIL | M_ZERO);
	if (dev == NULL) {
		explicit_bzero(qd, sizeof(*qd));
		free(qd, M_DEVBUF, sizeof(*qd));
		qwzmac_taskq_fini();
		return ENOMEM;
	}
	dev->dv_class = DV_IFNET;
	dev->dv_unit = 0;
	strlcpy(dev->dv_xname, "qwz0", sizeof(dev->dv_xname));
	qd->dev = dev;
	qd->pc = pcidev;
	qd->ops = *ops;
	qwzmac_the_dev = qd;

	printf("qwz0 at pci: attaching (softc %zu bytes)\n",
	    qwz_pci_ca.ca_devsize);
	scheduled = mountroot_scheduled;
	/*
	 * OpenBSD attaches devices while "cold", with interrupts blocked.
	 * Holding the spl lock keeps qwz_pci_intr() out until attach is done
	 * and the hooks are set.
	 */
	qwzmac_spl_lock();
	qwz_pci_ca.ca_attach(NULL, dev, &pa);
	ifp = &((struct qwz_softc *)dev)->sc_ic.ic_if;
	ifp->if_qwzmac_cookie = qd;
	ifp->if_qwzmac_input = qwzmac_if_input;
	ifp->if_qwzmac_link_state = qwzmac_if_link_state;
	qwzmac_spl_unlock();
	/* qwz_pci_attach() schedules the hook only when it fully succeeded. */
	qd->attached = (mountroot_scheduled != scheduled);
	if (!qd->attached)
		printf("QwzWifi: qwz_pci_attach failed early\n");

	*devp = qd;
	return 0;
}

int
qwzmac_qwz_output(void *p, const void *frame, size_t len)
{
	struct qwzmac_dev *qd = p;
	struct qwz_softc *sc;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	struct mbuf *m;
	int error;

	if (qd == NULL || !qd->attached)
		return ENXIO;
	if (len < ETHER_HDR_LEN || len > QWZMAC_FRAME_MAX)
		return EMSGSIZE;
	sc = (struct qwz_softc *)qd->dev;
	ic = &sc->sc_ic;
	ifp = &ic->ic_if;

	qwzmac_spl_lock();
	if (ic->ic_state != IEEE80211_S_RUN ||
	    (ifp->if_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
	    !LINK_STATE_IS_UP(ifp->if_link_state)) {
		qwzmac_spl_unlock();
		return ENETDOWN;
	}
	m = m_devget((char *)frame, len, 0);
	if (m == NULL) {
		ifp->if_oerrors++;
		qwzmac_spl_unlock();
		return ENOBUFS;
	}
	/* Ownership of m passes to if_enqueue() even when it fails. */
	error = if_enqueue(ifp, m);
	if (error)
		ifp->if_oerrors++;
	else
		ifp->if_opackets++;
	qwzmac_spl_unlock();
	return error;
}

/*
 * input() and link() run with the spl lock held, so clearing ops under it
 * waits for a running one. ready() runs without it: mountroot_wait() in
 * detach covers that one.
 */
void
qwzmac_qwz_disconnect(void *p)
{
	struct qwzmac_dev *qd = p;

	if (qd == NULL)
		return;
	qwzmac_spl_lock();
	qd->ops.input = NULL;
	qd->ops.link = NULL;
	qwzmac_spl_unlock();
}

void
qwzmac_qwz_detach(void *p)
{
	struct qwzmac_dev *qd = p;

	if (qd == NULL)
		return;

	/* No more calls into the kext, which is going away. */
	qwzmac_spl_lock();
	memset(&qd->ops, 0, sizeof(qd->ops));
	qwzmac_spl_unlock();

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

	qwzmac_the_dev = NULL;
	explicit_bzero(qd, sizeof(*qd));
	free(qd, M_DEVBUF, sizeof(*qd));
	printf("QwzWifi: qwz detached\n");
}

/*
 * System shutdown or restart: power the chip down, as Linux ath12k does in
 * its PCI .shutdown hook (ath12k_pci_hw_group_power_down()). In qwz that is
 * the suspend path, qwz_activate(DVACT_QUIESCE): qwz_stop(), then
 * qwz_core_deinit(), which ends in qwz_pci_power_down() (MHI to M3, SoC
 * reset). OpenBSD itself does not do this at reboot (its boot() only takes
 * interfaces down), but here the module keeps standby power across reboot
 * and S5, and a chip left running vanished from the bus (runs 7, 8, 12).
 * QWZMAC glue: it runs in a thread with QWZMAC_SHUTDOWN_NSEC, so a hung
 * chip cannot hang the macOS shutdown. Bus mastering goes off last.
 */
#define QWZMAC_SHUTDOWN_NSEC	SEC_TO_NSEC(10)

static int qwzmac_shutdown_done;

static void
qwzmac_shutdown_thread(void *arg)
{
	struct qwzmac_dev *qd = arg;

	qwz_activate(qd->dev, DVACT_QUIESCE);

	qwzmac_spl_lock();
	qwzmac_shutdown_done = 1;
	qwzmac_wakeup(&qwzmac_shutdown_done, 0);
	qwzmac_spl_unlock();
}

void
qwzmac_qwz_shutdown(void *p)
{
	struct qwzmac_dev *qd = p;
	uint64_t deadline;
	int done = 0;

	if (qd == NULL || !qd->attached)
		return;
	/* No calls into the kext from here on. */
	qwzmac_qwz_disconnect(qd);

	qwzmac_shutdown_done = 0;
	if (qwzmac_thread_start(qwzmac_shutdown_thread, qd) == 0) {
		deadline = getnsecuptime() + QWZMAC_SHUTDOWN_NSEC;
		qwzmac_spl_lock();
		while (!qwzmac_shutdown_done) {
			uint64_t now = getnsecuptime();

			if (now >= deadline)
				break;
			qwzmac_sleep(&qwzmac_shutdown_done, deadline - now);
		}
		done = qwzmac_shutdown_done;
		qwzmac_spl_unlock();
	}
	printf("QwzWifi: shutdown: chip %s\n", done ? "powered down" :
	    "did not power down in time");
	/* Last step: whatever state it is in, no more DMA into RAM. */
	pci_bus_master_off(qd->pc);
}
