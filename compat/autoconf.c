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
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_ra.h>

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

typedef int (*qwzmac_ioctl_fn)(struct ifnet *, u_long, void *);

static int
qwzmac_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	int error = ifp->if_ioctl(ifp, cmd, (caddr_t)data);

	/* Interface still down: settings apply when it comes up. */
	return error == ENETRESET ? 0 : error;
}

/*
 * The same settings straight into net80211, past qwz_ioctl(): ENETRESET
 * does not restart the firmware here; the caller rescans instead (see
 * qwzmac_apply_live()). Called with ioctl_rwl and the spl lock held, as
 * qwz_ioctl() would hold them.
 */
static int
qwzmac_ioctl80211(struct ifnet *ifp, u_long cmd, void *data)
{
	int error = ieee80211_ioctl(ifp, cmd, (caddr_t)data);

	return error == ENETRESET ? 0 : error;
}

/*
 * "ifconfig qwz0 nwid <ssid> wpakey <passphrase>": join exactly this SSID
 * with WPA2-PSK (CCMP). Setting nwid also turns auto-join off.
 */
static int
qwzmac_set_network(struct qwzmac_dev *qd, struct ifnet *ifp,
    qwzmac_ioctl_fn ioc)
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
	if ((error = ioc(ifp, SIOCS80211NWID, &ifr)) != 0)
		return error;

	memset(&wpa, 0, sizeof(wpa));
	strlcpy(wpa.i_name, ifp->if_xname, sizeof(wpa.i_name));
	wpa.i_enabled = 1;
	wpa.i_protos = IEEE80211_WPA_PROTO_WPA2;
	wpa.i_akms = IEEE80211_WPA_AKM_PSK;
	wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP;
	wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
	if ((error = ioc(ifp, SIOCS80211WPAPARMS, &wpa)) != 0)
		return error;

	memset(&psk, 0, sizeof(psk));
	strlcpy(psk.i_name, ifp->if_xname, sizeof(psk.i_name));
	psk.i_enabled = 1;
	memcpy(psk.i_psk, qd->psk, sizeof(psk.i_psk));
	error = ioc(ifp, SIOCS80211WPAPSK, &psk);
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

		error = qwzmac_set_network(qd, ifp, qwzmac_ioctl);
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

/* Tell the kext the radio's state (qwzmac_netif_ops.radio). */
static void
qwzmac_radio(struct qwzmac_dev *qd, int state)
{
	qwzmac_spl_lock();
	if (qd->ops.radio != NULL)
		qd->ops.radio(qd->ops.cookie, state);
	qwzmac_spl_unlock();
}

/* The state after a bring-up: up if the interface is up and running. */
static void
qwzmac_radio_now(struct qwzmac_dev *qd)
{
	qwzmac_radio(qd, qwzmac_qwz_power(qd) ? QWZMAC_RADIO_UP :
	    QWZMAC_RADIO_DOWN);
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
		qwzmac_radio_now(qd);
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
 * The current TX rate in bit/s, or 0 when not associated. TX completions
 * carry the rate the firmware used (qwz_dp_tx_complete_msdu() stores it in
 * ni_txmcs / ni_txrate). For HT that is only the per-stream MCS (0-7), not
 * the stream count, width or guard interval, so those are taken as
 * negotiated: the streams both sides support (as qwz_peer_assoc_h_ht()
 * counts them), 40 MHz as qwz sets up the peer, and short GI if both have it.
 */
/* 40 MHz as qwz sets the peer up (qwz_peer_assoc_h_phymode()). */
static int
qwzmac_chan40(struct ieee80211_node *ni)
{
	uint8_t sco = ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK;

	return IEEE80211_CHAN_40MHZ_ALLOWED(ni->ni_chan) &&
	    ieee80211_node_supports_ht_chan40(ni) &&
	    (sco == IEEE80211_HTOP0_SCO_SCA || sco == IEEE80211_HTOP0_SCO_SCB);
}

/* spl lock held, associated. Sets *mcsp to the HT MCS index or -1. */
static uint64_t
qwzmac_ni_tx_rate(struct ieee80211com *ic, struct ieee80211_node *ni,
    int *mcsp)
{
	const struct ieee80211_ht_rateset *rs;
	int i, mcs, nss, chan40, sgi;

	*mcsp = -1;
	if (ni->ni_flags & IEEE80211_NODE_HT) {
		for (i = 0, nss = 1; i < 4; i++)
			if (ic->ic_sup_mcs[i] & ni->ni_rxmcs[i])
				nss = i + 1;
		mcs = ni->ni_txmcs;
		if (mcs < 8)
			mcs += 8 * (nss - 1);
		*mcsp = mcs;
		chan40 = qwzmac_chan40(ni);
		sgi = ieee80211_ra_use_ht_sgi(ni);
		for (i = 0; i < IEEE80211_HT_NUM_RATESETS; i++) {
			rs = &ieee80211_std_ratesets_11n[i];
			if (rs->chan40 == chan40 && rs->sgi == sgi &&
			    mcs >= rs->min_mcs && mcs <= rs->max_mcs)
				return rs->rates[mcs - rs->min_mcs] * 500000ULL;
		}
		return 0;
	}
	if (ni->ni_txrate < ni->ni_rates.rs_nrates)
		return (ni->ni_rates.rs_rates[ni->ni_txrate] &
		    IEEE80211_RATE_VAL) * 500000ULL;
	return 0;
}

/*
 * The current TX rate in bit/s, or 0 when not associated. TX completions
 * carry the rate the firmware used (qwz_dp_tx_complete_msdu() stores it in
 * ni_txmcs / ni_txrate). For HT that is only the per-stream MCS (0-7), not
 * the stream count, width or guard interval, so those are taken as
 * negotiated: the streams both sides support (as qwz_peer_assoc_h_ht()
 * counts them), 40 MHz as qwz sets up the peer, and short GI if both have it.
 */
uint64_t
qwzmac_qwz_tx_rate(void *p)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	uint64_t rate = 0;
	int mcs;

	if (qd == NULL || !qd->attached)
		return 0;
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;

	qwzmac_spl_lock();
	if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss != NULL)
		rate = qwzmac_ni_tx_rate(ic, ic->ic_bss, &mcs);
	qwzmac_spl_unlock();
	return rate;
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
	qd->ops.radio = NULL;
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
 * Bounded by qwzmac_activate_bounded(). Bus mastering goes off last.
 */
#define QWZMAC_POWER_NSEC	SEC_TO_NSEC(10)

static int qwzmac_power_done;

struct qwzmac_power_job {
	struct qwzmac_dev	*qd;
	int			 act;	/* DVACT_* */
};

static void
qwzmac_power_thread(void *arg)
{
	struct qwzmac_power_job *job = arg;

	qwz_activate(job->qd->dev, job->act);

	qwzmac_spl_lock();
	qwzmac_power_done = 1;
	qwzmac_wakeup(&qwzmac_power_done, 0);
	qwzmac_spl_unlock();
}

/*
 * Run qwz_activate(act) in a thread and wait at most QWZMAC_POWER_NSEC,
 * so a hung chip cannot hang a macOS shutdown or sleep. Returns 1 if it
 * finished. The job stays allocated if it did not: the thread still
 * holds it.
 */
static int
qwzmac_activate_bounded(struct qwzmac_dev *qd, int act)
{
	struct qwzmac_power_job *job;
	uint64_t deadline;
	int done = 0;

	job = malloc(sizeof(*job), M_DEVBUF, M_WAITOK | M_ZERO);
	job->qd = qd;
	job->act = act;
	qwzmac_power_done = 0;
	if (qwzmac_thread_start(qwzmac_power_thread, job) != 0) {
		free(job, M_DEVBUF, sizeof(*job));
		return 0;
	}
	deadline = getnsecuptime() + QWZMAC_POWER_NSEC;
	qwzmac_spl_lock();
	while (!qwzmac_power_done) {
		uint64_t now = getnsecuptime();

		if (now >= deadline)
			break;
		qwzmac_sleep(&qwzmac_power_done, deadline - now);
	}
	done = qwzmac_power_done;
	qwzmac_spl_unlock();
	if (done)
		free(job, M_DEVBUF, sizeof(*job));
	return done;
}

void
qwzmac_qwz_shutdown(void *p)
{
	struct qwzmac_dev *qd = p;
	int done;

	if (qd == NULL || !qd->attached)
		return;
	/* No calls into the kext from here on. */
	qwzmac_qwz_disconnect(qd);

	done = qwzmac_activate_bounded(qd, DVACT_QUIESCE);
	printf("QwzWifi: shutdown: chip %s\n", done ? "powered down" :
	    "did not power down in time");
	/* Last step: whatever state it is in, no more DMA into RAM. */
	pci_bus_master_off(qd->pc);
}

/*
 * System sleep and wake, as OpenBSD's config_suspend_all() and
 * config_resume() drive qwz: DVACT_QUIESCE before sleep (qwz_stop(), then
 * qwz_core_deinit(): the chip is powered down, no DMA), DVACT_SUSPEND
 * (nothing in qwz); after wake DVACT_RESUME (HAL rings reset) and
 * DVACT_WAKEUP (qwz_init(): firmware loaded again, the interface is still
 * up, so net80211 joins the network again). That is the same bring-up as
 * the first "ifconfig up" after qwz_attach(). macOS saves and restores
 * the PCI config space (BARs, MSI) around sleep itself.
 */
static int qwzmac_suspended;	/* quiesced by qwzmac_qwz_sleep() */

void
qwzmac_qwz_sleep(void *p)
{
	struct qwzmac_dev *qd = p;
	int done;

	if (qd == NULL || !qd->attached || qwzmac_suspended)
		return;
	if (mountroot_pending > 0) {
		/* Bring-up still running; its interface comes up after wake. */
		printf("QwzWifi: sleep: bring-up still running, chip left as is\n");
		return;
	}
	qwzmac_radio(qd, QWZMAC_RADIO_GOING_DOWN);
	done = qwzmac_activate_bounded(qd, DVACT_QUIESCE);
	if (done)
		qwz_activate(qd->dev, DVACT_SUSPEND);
	else
		/* Hung: at least keep it from writing into RAM while we sleep. */
		pci_bus_master_off(qd->pc);
	qwzmac_suspended = done ? 1 : -1;
	qwzmac_radio(qd, QWZMAC_RADIO_DOWN);
	printf("QwzWifi: sleep: chip %s\n", done ? "powered down" :
	    "did not power down in time, bus mastering off");
}

static void
qwzmac_wake_thread(void *arg)
{
	struct qwzmac_dev *qd = arg;
	pcireg_t id = 0xffffffff;
	int i;

	/* The device should be back in D0; give it a moment if not. */
	for (i = 0; i < 50; i++) {
		id = pci_conf_read(qd->pc, 0, PCI_ID_REG);
		if (id != 0xffffffff && id != 0)
			break;
		tsleep_nsec(&qwzmac_suspended, 0, "qwzwake", MSEC_TO_NSEC(100));
	}
	if (id == 0xffffffff || id == 0) {
		printf("QwzWifi: wake: device not on the bus, giving up\n");
		return;
	}
	qwz_activate(qd->dev, DVACT_RESUME);
	qwz_activate(qd->dev, DVACT_WAKEUP);	/* qwz_init(): firmware up */
	printf("QwzWifi: wake: chip up again\n");
	qwzmac_radio_now(qd);
}

void
qwzmac_qwz_wake(void *p)
{
	struct qwzmac_dev *qd = p;
	int was;

	if (qd == NULL || !qd->attached)
		return;
	was = qwzmac_suspended;
	qwzmac_suspended = 0;
	if (was == 0)
		return;
	if (was < 0) {
		printf("QwzWifi: wake: chip did not power down before sleep, "
		    "leaving it off\n");
		return;
	}
	/* Firmware loading takes seconds: not in the power message. */
	if (qwzmac_thread_start(qwzmac_wake_thread, qd) != 0)
		printf("QwzWifi: wake: cannot start the wake thread\n");
}

/*
 * Management for a Wi-Fi app (HeliPort, via kext/QwzUserClient.cpp).
 * While the radio is on, a join or leave changes net80211's settings and
 * rescans, without restarting the firmware (qwzmac_apply_live()). With the
 * radio off it is done the way ifconfig(8) would: "ifconfig qwz0 nwid X
 * wpakey Y; ifconfig qwz0 up", the settings first so that qwz_ioctl()
 * loads the firmware once, not after each of the NWID/WPAPARMS/WPAPSK
 * ioctls (each returns ENETRESET).
 * Called in the app's thread, without the spl lock; they block.
 */
static int
qwzmac_mgmt_busy(struct qwzmac_dev *qd)
{
	return qd == NULL || !qd->attached || mountroot_pending > 0 ||
	    qwzmac_suspended != 0;
}

static int
qwzmac_ifflags(struct ifnet *ifp, int up)
{
	struct ifreq ifr;

	qwzmac_spl_lock();
	if (up)
		if_up(ifp);
	else
		if_down(ifp);
	qwzmac_spl_unlock();
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_flags = ifp->if_flags;
	return ifp->if_ioctl(ifp, SIOCSIFFLAGS, (caddr_t)&ifr);
}

/* "ifconfig qwz0 nwid <ssid>": it also turns WPA and WEP off. */
static int
qwzmac_set_nwid(struct ifnet *ifp, const uint8_t *ssid, size_t len,
    qwzmac_ioctl_fn ioc)
{
	struct ieee80211_nwid nwid;
	struct ifreq ifr;

	memset(&nwid, 0, sizeof(nwid));
	nwid.i_len = len;
	memcpy(nwid.i_nwid, ssid, len);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&nwid;
	return ioc(ifp, SIOCS80211NWID, &ifr);
}

/* "ifconfig qwz0 -nwid", plus the scan-only trick from qwzmac_up(). */
static int
qwzmac_scan_only(struct qwzmac_dev *qd, struct ifnet *ifp,
    qwzmac_ioctl_fn ioc)
{
	struct ieee80211com *ic = (struct ieee80211com *)ifp;
	int error;

	error = qwzmac_set_nwid(ifp, NULL, 0, ioc);
	qwzmac_spl_lock();
	memset(ic->ic_des_bssid, 0, IEEE80211_ADDR_LEN);
	ic->ic_flags |= IEEE80211_F_DESBSSID;
	qd->have_net = 0;
	qwzmac_spl_unlock();
	return error;
}

/* Join qd->ssid, WPA2-PSK with qd->psk if psk is set, else open. */
static int
qwzmac_set_join(struct qwzmac_dev *qd, struct ifnet *ifp, int psk,
    qwzmac_ioctl_fn ioc)
{
	struct ieee80211com *ic = (struct ieee80211com *)ifp;

	qwzmac_spl_lock();
	ic->ic_flags &= ~IEEE80211_F_DESBSSID;	/* undo scan-only */
	qwzmac_spl_unlock();
	if (psk)
		return qwzmac_set_network(qd, ifp, ioc);
	return qwzmac_set_nwid(ifp, qd->ssid, qd->ssid_len, ioc);
}

/*
 * Change the network while the interface is up and running, without the
 * restart qwz_ioctl() does on ENETRESET (qwz_stop() and qwz_init(): the
 * firmware is loaded again, ~2 s). The settings go to net80211 directly,
 * then it rescans as after a beacon miss: qwz_newstate_task() takes
 * RUN -> SCAN with qwz_run_stop() and qwz_deauth() (peer and keys gone
 * from the firmware), and the end of the scan joins what the new settings
 * select. From SCAN the running scan simply ends with the new settings.
 * No deauth frame goes to the old AP (same as a beacon miss); it drops us
 * after its inactivity timeout.
 * Returns EAGAIN when the radio is off: the caller goes the ifconfig way.
 */
static int
qwzmac_apply_live(struct qwzmac_dev *qd, struct ifnet *ifp, int leave,
    int psk)
{
	struct qwz_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error;

	/* Same order as qwz_ioctl(): no qwz_init() can be running meanwhile. */
	rw_enter_write(&sc->ioctl_rwl);
	qwzmac_spl_lock();
	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
	    ic->ic_state == IEEE80211_S_INIT) {
		error = EAGAIN;
		goto out;
	}
	if (leave)
		error = qwzmac_scan_only(qd, ifp, qwzmac_ioctl80211);
	else
		error = qwzmac_set_join(qd, ifp, psk, qwzmac_ioctl80211);
	if (error == 0)
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
out:
	qwzmac_spl_unlock();
	rw_exit(&sc->ioctl_rwl);
	return error;
}

int
qwzmac_qwz_join(void *p, struct qwzmac_network *net)
{
	struct qwzmac_dev *qd = p;
	struct ifnet *ifp;
	uint8_t psk[32];
	size_t slen, plen;
	int error;

	slen = strnlen(net->ssid, sizeof(net->ssid));
	plen = strnlen(net->passphrase, sizeof(net->passphrase));
	if (slen == 0 || slen > IEEE80211_NWID_LEN ||
	    (plen != 0 && (plen < 8 || plen > 63))) {
		explicit_bzero(net->passphrase, sizeof(net->passphrase));
		return EINVAL;
	}
	if (plen != 0 && pkcs5_pbkdf2(net->passphrase, plen,
	    (const uint8_t *)net->ssid, slen, psk, sizeof(psk), 4096) != 0) {
		explicit_bzero(net->passphrase, sizeof(net->passphrase));
		return EINVAL;
	}
	explicit_bzero(net->passphrase, sizeof(net->passphrase));
	if (qwzmac_mgmt_busy(qd)) {
		explicit_bzero(psk, sizeof(psk));
		return EBUSY;
	}
	ifp = &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;

	qwzmac_spl_lock();
	memcpy(qd->ssid, net->ssid, slen);
	qd->ssid_len = slen;
	if (plen != 0)
		memcpy(qd->psk, psk, sizeof(qd->psk));
	qwzmac_spl_unlock();
	explicit_bzero(psk, sizeof(psk));

	error = qwzmac_apply_live(qd, ifp, 0, plen != 0);
	if (error == 0) {
		printf("QwzWifi: joining \"%.*s\" (%s), requested by an app, "
		    "no firmware restart\n", (int)slen, net->ssid,
		    plen ? "WPA2-PSK" : "open");
	} else if (error == EAGAIN) {
		/*
		 * Radio off (or not running): the ifconfig way, down first so
		 * that the settings do not each restart the firmware, then up
		 * (one firmware load).
		 */
		if ((error = qwzmac_ifflags(ifp, 0)) == 0)
			error = qwzmac_set_join(qd, ifp, plen != 0,
			    qwzmac_ioctl);
		if (error == 0) {
			printf("QwzWifi: joining \"%.*s\" (%s), requested by an "
			    "app\n", (int)slen, net->ssid,
			    plen ? "WPA2-PSK" : "open");
			error = qwzmac_ifflags(ifp, 1);
		}
	}
	explicit_bzero(qd->psk, sizeof(qd->psk));
	if (error) {
		if (qwzmac_apply_live(qd, ifp, 1, 0) == EAGAIN)
			qwzmac_scan_only(qd, ifp, qwzmac_ioctl);
		printf("QwzWifi: join failed: %d\n", error);
	} else
		qd->have_net = 1;
	return error;
}

int
qwzmac_qwz_leave(void *p)
{
	struct qwzmac_dev *qd = p;
	struct ifnet *ifp;
	int error;

	if (qwzmac_mgmt_busy(qd))
		return EBUSY;
	ifp = &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;
	error = qwzmac_apply_live(qd, ifp, 1, 0);
	if (error == EAGAIN)	/* radio off: nothing to leave */
		error = qwzmac_scan_only(qd, ifp, qwzmac_ioctl);
	if (error == 0)
		printf("QwzWifi: network forgotten by an app, scanning only\n");
	return error;
}

int
qwzmac_qwz_set_power(void *p, int on)
{
	struct qwzmac_dev *qd = p;
	struct ifnet *ifp;
	int error;

	if (qwzmac_mgmt_busy(qd))
		return EBUSY;
	ifp = &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;
	printf("QwzWifi: radio turned %s by an app\n", on ? "on" : "off");
	if (!on)
		qwzmac_radio(qd, QWZMAC_RADIO_GOING_DOWN);
	error = qwzmac_ifflags(ifp, on);
	qwzmac_radio_now(qd);
	return error;
}

int
qwzmac_qwz_power(void *p)
{
	struct qwzmac_dev *qd = p;
	struct ifnet *ifp;

	if (qd == NULL || !qd->attached)
		return 0;
	ifp = &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;
	return (ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING);
}

void
qwzmac_qwz_scan(void *p)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;

	if (qwzmac_mgmt_busy(qd))
		return;
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;
	qwzmac_spl_lock();
	if (ic->ic_state == IEEE80211_S_RUN)
		ieee80211_begin_bgscan(&ic->ic_if);
	qwzmac_spl_unlock();
}

void
qwzmac_qwz_fw_version(void *p, char *buf, size_t len)
{
	struct qwzmac_dev *qd = p;
	const char *id, *eq;

	if (len == 0)
		return;
	buf[0] = '\0';
	if (qd == NULL || !qd->attached)
		return;
	id = ((struct qwz_softc *)qd->dev)->qmi_target.fw_build_id;
	/* The firmware reports "QC_IMAGE_VERSION_STRING=WLAN.HMT...". */
	for (eq = id; *eq != '\0'; eq++)
		if (*eq == '=') {
			id = eq + 1;
			break;
		}
	strlcpy(buf, id, len);
}

/* qwz stores the signal as a signed dBm value in net80211's u_int8_t. */
#define QWZMAC_RSSI_DBM(ni)	((int)(int8_t)(ni)->ni_rssi)

int
qwzmac_qwz_sta_info(void *p, struct qwzmac_sta_info *si)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	int error = 0;

	memset(si, 0, sizeof(*si));
	si->mcs = -1;
	if (qd == NULL || !qd->attached)
		return ENXIO;
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;

	qwzmac_spl_lock();
	si->state = ic->ic_state;
	ni = ic->ic_bss;
	if (ic->ic_state != IEEE80211_S_RUN || ni == NULL ||
	    ni->ni_chan == NULL) {
		error = ENOTCONN;
	} else {
		si->phy = ic->ic_curmode > IEEE80211_MODE_AUTO ?
		    ic->ic_curmode - 1 : 0;
		si->ssid_len = MIN(ni->ni_esslen, sizeof(si->ssid));
		memcpy(si->ssid, ni->ni_essid, si->ssid_len);
		memcpy(si->bssid, ni->ni_bssid, sizeof(si->bssid));
		si->channel = ieee80211_chan2ieee(ic, ni->ni_chan);
		si->width_mhz = (ni->ni_flags & IEEE80211_NODE_HT) &&
		    qwzmac_chan40(ni) ? 40 : 20;
		si->rssi_dbm = QWZMAC_RSSI_DBM(ni);
		si->noise_dbm = ATH12K_DEFAULT_NOISE_FLOOR;
		si->rate_bps = qwzmac_ni_tx_rate(ic, ni, &si->mcs);
	}
	qwzmac_spl_unlock();
	return error;
}

int
qwzmac_qwz_scan_results(void *p, struct qwzmac_bss *out, int max)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	struct qwzmac_bss *b;
	int n = 0;

	if (qd == NULL || !qd->attached)
		return 0;
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;

	qwzmac_spl_lock();
	RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
		if (n >= max)
			break;
		if (ni->ni_esslen == 0 || ni->ni_essid[0] == '\0' ||
		    ni->ni_chan == NULL)
			continue;	/* hidden: cannot be picked from a list */
		b = &out[n++];
		memset(b, 0, sizeof(*b));
		b->ssid_len = MIN(ni->ni_esslen, sizeof(b->ssid));
		memcpy(b->ssid, ni->ni_essid, b->ssid_len);
		memcpy(b->bssid, ni->ni_bssid, sizeof(b->bssid));
		b->channel = ieee80211_chan2ieee(ic, ni->ni_chan);
		b->rssi_dbm = QWZMAC_RSSI_DBM(ni);
		b->supported_rsnprotos = ni->ni_supported_rsnprotos;
		b->rsnprotos = ni->ni_rsnprotos;
		b->supported_rsnakms = ni->ni_supported_rsnakms;
		b->rsnakms = ni->ni_rsnakms;
		b->rsnciphers = ni->ni_rsnciphers;
		b->rsngroupcipher = ni->ni_rsngroupcipher;
		b->rsngroupmgmtcipher = ni->ni_rsngroupmgmtcipher;
		b->rsncipher = ni->ni_rsncipher;
		b->capinfo = ni->ni_capinfo;
		b->rsncaps = ni->ni_rsncaps;
	}
	qwzmac_spl_unlock();
	return n;
}

int
qwzmac_qwz_channels(void *p, struct qwzmac_chan *out, int max)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ieee80211_channel *c;
	struct qwzmac_chan *o;
	int i, n = 0;

	if (qd == NULL || !qd->attached)
		return 0;
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;

	qwzmac_spl_lock();
	for (i = 1; i <= IEEE80211_CHAN_MAX && n < max; i++) {
		c = &ic->ic_channels[i];
		if (c->ic_flags == 0 || !isset(ic->ic_chan_active, i))
			continue;
		o = &out[n++];
		o->channel = i;
		o->freq_mhz = c->ic_freq;
		o->flags = 0;
		if (IEEE80211_IS_CHAN_2GHZ(c))
			o->flags |= QWZMAC_CHAN_2GHZ;
		if (IEEE80211_IS_CHAN_5GHZ(c))
			o->flags |= QWZMAC_CHAN_5GHZ;
		if (c->ic_flags & IEEE80211_CHAN_PASSIVE)
			o->flags |= QWZMAC_CHAN_PASSIVE;
		if (IEEE80211_CHAN_40MHZ_ALLOWED(c))
			o->flags |= QWZMAC_CHAN_40MHZ;
		if (IEEE80211_CHAN_80MHZ_ALLOWED(c))
			o->flags |= QWZMAC_CHAN_80MHZ;
	}
	qwzmac_spl_unlock();
	return n;
}

int
qwzmac_qwz_country(void *p, char cc[3])
{
	struct qwzmac_dev *qd = p;
	struct qwz_softc *sc;
	int error = 0;

	cc[0] = cc[1] = cc[2] = '\0';
	if (qd == NULL || !qd->attached)
		return ENXIO;
	sc = (struct qwz_softc *)qd->dev;
	qwzmac_spl_lock();
	if (sc->qwzmac_alpha2[0] == '\0')
		error = ENXIO;
	else
		memcpy(cc, sc->qwzmac_alpha2, 2);
	qwzmac_spl_unlock();
	return error;
}

/* Native scans run outside the front's WCL gate; all net80211 access is
 * serialized by spl. A request during continuous SCAN configures the next hardware cycle. */
int
qwzmac_qwz_scan_start(void *p, const struct qwzmac_scan_request *request,
    uint32_t *ticket)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int error = 0, usable = 0;
	unsigned c;
	if (ticket == NULL || request == NULL || request->ssid_len > 32)
		return EINVAL;
	if (qwzmac_mgmt_busy(qd)) return EBUSY;
	qwzmac_spl_lock();
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;
	ifp = &ic->ic_if;
	for (c = 1; c <= IEEE80211_CHAN_MAX && c < 256; ++c)
		if (ic->ic_channels[c].ic_flags && (!request->restricted ||
		    (request->channels[c / 8] & (1U << (c % 8))))) usable = 1;
	if (!usable) error = EOPNOTSUPP;
	else if (!(ifp->if_flags & IFF_RUNNING)) error = ENETDOWN;
	else if (ic->ic_state != IEEE80211_S_RUN && ic->ic_state != IEEE80211_S_SCAN)
		error = EBUSY;
	else if (ic->ic_state == IEEE80211_S_RUN && (ic->ic_flags & IEEE80211_F_BGSCAN))
		error = EBUSY; /* do not commandeer a scan already owned by HeliPort */
	if (!error) {
		ifp->if_qwzmac_scan_request = *request;
		ifp->if_qwzmac_scan_pending = 1;
		if (++ifp->if_qwzmac_scan_ticket == 0) ++ifp->if_qwzmac_scan_ticket;
		*ticket = ifp->if_qwzmac_scan_ticket;
		if (ic->ic_state == IEEE80211_S_RUN) {
			ieee80211_begin_bgscan(ifp);
			if (!(ic->ic_flags & IEEE80211_F_BGSCAN)) {
				ifp->if_qwzmac_scan_pending = 0;
				error = EBUSY;
			}
		}
	}
	qwzmac_spl_unlock();
	return error;
}

int
qwzmac_qwz_scan_poll(void *p, uint32_t ticket)
{
	struct qwzmac_dev *qd = p;
	struct ifnet *ifp;
	int error;
	if (qwzmac_mgmt_busy(qd)) return EBUSY;
	qwzmac_spl_lock();
	ifp = &((struct qwz_softc *)qd->dev)->sc_ic.ic_if;
	if (!(ifp->if_flags & IFF_RUNNING)) error = ENETDOWN;
	else if (ticket != ifp->if_qwzmac_scan_done) error = EINPROGRESS;
	else error = ifp->if_qwzmac_scan_done_error;
	qwzmac_spl_unlock();
	return error;
}

static void
qwzmac_copy_bss(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct qwzmac_scan_bss *b)
{
	unsigned channel = ieee80211_chan2ieee(ic, ni->ni_chan), rates;
	uint8_t *ie;
	memset(b, 0, sizeof(*b));
	b->ssid_len = ni->ni_esslen;
	memcpy(b->ssid, ni->ni_essid, b->ssid_len);
	memcpy(b->bssid, ni->ni_bssid, 6);
	b->channel = channel;
	b->interval = ni->ni_intval;
	b->capability = ni->ni_capinfo;
	b->rssi_dbm = QWZMAC_RSSI_DBM(ni);
	ie = b->ies;
	*ie++ = 0; *ie++ = b->ssid_len;
	memcpy(ie, b->ssid, b->ssid_len); ie += b->ssid_len;
	rates = MIN(ni->ni_rates.rs_nrates, 8);
	*ie++ = 1; *ie++ = rates;
	memcpy(ie, ni->ni_rates.rs_rates, rates); ie += rates;
	*ie++ = 3; *ie++ = 1; *ie++ = channel;
	/* Preserve RSN/WPA bytes; do not reinterpret security capabilities. */
	if (ni->ni_rsnie != NULL) {
		size_t len = (size_t)ni->ni_rsnie[1] + 2;
		memcpy(ie, ni->ni_rsnie, len); ie += len;
	}
	b->ie_len = ie - b->ies; /* <= 34+10+3+257 = 304 */
}

int
qwzmac_qwz_scan_snapshot(void *p, uint32_t ticket, struct qwzmac_scan_bss *out, int max)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	int n = 0;
	if (out == NULL || max <= 0 || qwzmac_mgmt_busy(qd))
		return 0;
	qwzmac_spl_lock();
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;
	/* Only the successfully completed request may publish observations. */
	if (ticket == 0 || ic->ic_if.if_qwzmac_scan_done != ticket ||
	    ic->ic_if.if_qwzmac_scan_done_error != 0) {
		qwzmac_spl_unlock();
		return 0;
	}
	RBT_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
		struct qwzmac_scan_bss *b;
		unsigned channel;
		if (n >= max) break;
		/* Cache membership is not proof that the AP was heard this scan. */
		if (!qwzmac_scan_observed(&ni->ni_qwzmac_scan_seen, ticket)) continue;
		if (ni->ni_chan == NULL || ni->ni_chan == IEEE80211_CHAN_ANYC ||
		    ni->ni_esslen > 32) continue;
		channel = ieee80211_chan2ieee(ic, ni->ni_chan);
		if (channel == 0 || channel > 196) continue;
		b = &out[n++];
		qwzmac_copy_bss(ic, ni, b);
	}
	qwzmac_spl_unlock();
	return n;
}

/* Airport-only runtime MAC path. The ordinary HeliPort join/power paths are
 * unchanged. down/up alone preserves the firmware vdev, so explicitly replace
 * that vdev before restarting scans (same effect as MediaTek uni_add_dev). */
extern int qwz_init(struct ifnet *);
extern int qwzmac_recreate_vdev(struct qwz_softc *, const uint8_t[6]);
int
qwzmac_qwz_set_mac(void *p, const uint8_t mac[6])
{
	struct qwzmac_dev *qd = p;
	struct qwz_softc *sc;
	struct ifnet *ifp;
	uint8_t any = 0;
	int i, error, was_up;
	if (!mac) return EINVAL;
	for (i = 0; i < 6; ++i) any |= mac[i];
	if (!any || (mac[0] & 1)) return EINVAL;
	if (qwzmac_mgmt_busy(qd)) return EBUSY;
	sc = (struct qwz_softc *)qd->dev;
	ifp = &sc->sc_ic.ic_if;
	rw_enter_write(&sc->ioctl_rwl);
	qwzmac_spl_lock();
	if (sc->qwzmac_native_mac_fault) { error = EIO; goto out; }
	if (!memcmp(sc->qwzmac_vdev_mac, mac, 6) &&
	    !memcmp(sc->sc_ic.ic_myaddr, mac, 6)) { error = 0; goto out; }
	was_up = !!(ifp->if_flags & IFF_UP);
	if (ifp->if_flags & IFF_RUNNING) qwz_stop(ifp);
	error = qwzmac_recreate_vdev(sc, mac);
	if (!error && was_up) error = qwz_init(ifp);
	if (error) if_down(ifp);
out:
	qwzmac_spl_unlock();
	rw_exit(&sc->ioctl_rwl);
	return error;
}

int
qwzmac_qwz_native_join(void *p, struct qwzmac_native_join *request)
{
	struct qwzmac_dev *qd = p;
	struct qwz_softc *sc;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int error = EINVAL;
	if (request == NULL) return EINVAL;
	if (request->ssid_len == 0 || request->ssid_len > 32 ||
	    (request->bssid[0] & 1)) goto wipe;
	if (qwzmac_mgmt_busy(qd)) { error = EBUSY; goto wipe; }
	sc = (struct qwz_softc *)qd->dev;
	ic = &sc->sc_ic;
	ifp = &ic->ic_if;
	error = qwzmac_qwz_native_leave(p);
	if (error) goto wipe;
	printf("QwzWifi: native join accepted by net80211\n");
	rw_enter_write(&sc->ioctl_rwl);
	qwzmac_spl_lock();
	if (!(ifp->if_flags & IFF_RUNNING)) { error = ENETDOWN; goto unlock; }
	/* Keys, scan parameters and the target change as one net80211 operation. */
	ieee80211_disable_rsn(ic);
	ieee80211_disable_wep(ic);
	explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
	memcpy(qd->ssid, request->ssid, request->ssid_len);
	qd->ssid_len = request->ssid_len;
	explicit_bzero(qd->psk, sizeof(qd->psk));
	if (request->secured) memcpy(qd->psk, request->pmk, sizeof(qd->psk));
	ifp->if_qwzmac_scan_pending = 0;
	ifp->if_qwzmac_beacon_length = 0;
	error = request->secured ? qwzmac_set_network(qd, ifp, qwzmac_ioctl80211) :
	    qwzmac_set_nwid(ifp, qd->ssid, qd->ssid_len, qwzmac_ioctl80211);
	if (!error) {
		memcpy(ic->ic_des_bssid, request->bssid, 6);
		ic->ic_flags |= IEEE80211_F_DESBSSID;
		/* Same live transition as HeliPort, without firmware reload. */
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	}
	/* net80211 owns the PMK now; don't retain an extra management copy. */
	explicit_bzero(qd->psk, sizeof(qd->psk));
unlock:
	if (error) {
		explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
		explicit_bzero(qd->psk, sizeof(qd->psk));
	}
	qwzmac_spl_unlock();
	rw_exit(&sc->ioctl_rwl);
wipe:
	explicit_bzero(request->pmk, sizeof(request->pmk));
	return error;
}

int
qwzmac_qwz_native_status(void *p, struct qwzmac_native_status *status)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ifnet *ifp;
	memset(status, 0, sizeof(*status));
	if (qwzmac_mgmt_busy(qd)) return EBUSY;
	qwzmac_spl_lock();
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;
	ifp = &ic->ic_if;
	qwzmac_qwz_sta_info(p, &status->station); /* recursive spl lock */
	if (ic->ic_state == IEEE80211_S_RUN && ic->ic_bss && ic->ic_bss->ni_chan &&
	    ic->ic_bss->ni_chan != IEEE80211_CHAN_ANYC) {
		qwzmac_copy_bss(ic, ic->ic_bss, &status->bss);
		struct ieee80211_node *ni = ic->ic_bss;
		status->rate_count = MIN(ni->ni_rates.rs_nrates, sizeof(status->rates));
		memcpy(status->rates, ni->ni_rates.rs_rates, status->rate_count);
		if (ni->ni_flags & IEEE80211_NODE_HT)
			memcpy(status->ht_mcs, ni->ni_rxmcs, sizeof(status->ht_mcs));
		status->vht_mcs = (ni->ni_flags & IEEE80211_NODE_VHT) ? ni->ni_vht_txmcs : 0xffff;
		status->he_mcs = 0xffff;
		if (ni->ni_flags & IEEE80211_NODE_HE) {
			uint16_t local = ic->ic_he_rxmcs_80, peer = ni->ni_he_txmcs_80;
			for (unsigned n = 0; n < 8; ++n) {
				unsigned a = (local >> (2*n)) & 3, b = (peer >> (2*n)) & 3;
				unsigned common = a == 3 || b == 3 ? 3 : MIN(a,b);
				status->he_mcs = (status->he_mcs & ~(3U << (2*n))) | (common << (2*n));
			}
		}
		if (ic->ic_flags & IEEE80211_F_RSNON) {
			uint8_t *end = ieee80211_add_rsn(status->negotiated_rsn, ic, ni);
			status->negotiated_rsn_length = end - status->negotiated_rsn;
		}
		status->authorized = ifp->if_link_state >= LINK_STATE_UP &&
		    (!(ic->ic_flags & IEEE80211_F_RSNON) || ic->ic_bss->ni_port_valid);
		status->beacon_sequence = ifp->if_qwzmac_beacon_sequence;
		status->beacon_length = ifp->if_qwzmac_beacon_length;
		memcpy(status->beacon, ifp->if_qwzmac_beacon, status->beacon_length);
	}
	qwzmac_spl_unlock();
	return 0;
}

int
qwzmac_qwz_native_leave(void *p)
{
	struct qwzmac_dev *qd = p;
	int error = qwzmac_qwz_leave(p);
	if (qd != NULL && qd->attached) {
		struct qwz_softc *sc = (struct qwz_softc *)qd->dev;
		rw_enter_write(&sc->ioctl_rwl);
		qwzmac_spl_lock();
		ieee80211_disable_rsn(&sc->sc_ic);
		explicit_bzero(sc->sc_ic.ic_psk, sizeof(sc->sc_ic.ic_psk));
		explicit_bzero(qd->psk, sizeof(qd->psk));
		qd->have_net = 0;
		qwzmac_spl_unlock();
		rw_exit(&sc->ioctl_rwl);
	}
	return error;
}
