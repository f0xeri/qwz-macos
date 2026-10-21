/*
 * QwzWifi adapter: Device lifetime, firmware startup, frame delivery and sleep/wake.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "internal.h"

extern const struct cfattach qwz_pci_ca;

/* qwz can only reach the lower 4 GB (qwz_dmamem_alloc: 0..0xffffffff). */
static struct bus_dma_tag qwz_dmat32 = { 0, 0xffffffffUL };

/* There is one device; the mountroot hook only gets the softc. */
static struct qwzmac_dev *qwzmac_the_dev;

/* config_mountroot(9) stays with device lifetime: detach waits for its
 * firmware hook, which also publishes readiness and starts the interface. */
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


static void
qwzmac_if_beacon(struct ifnet *ifp)
{
	struct qwzmac_dev *qd = ifp->if_qwzmac_cookie;
	if (qd->ops.beacon != NULL) qd->ops.beacon(qd->ops.cookie);
}

/* Configure and start the interface from the bring-up thread, without spl.
 * IFF_DEBUG logs scans, association and the handshake. With no network set,
 * an all-zero desired BSSID prevents automatic association. */
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
void
qwzmac_radio(struct qwzmac_dev *qd, int state)
{
	qwzmac_spl_lock();
	if (qd->ops.radio != NULL)
		qd->ops.radio(qd->ops.cookie, state);
	qwzmac_spl_unlock();
}

/* The state after a bring-up: up if the interface is up and running. */
void
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
	ifp->if_qwzmac_beacon_event = qwzmac_if_beacon;
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
	qd->ops.radio = NULL;
	qd->ops.beacon = NULL;
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

/* Power down on shutdown/restart through qwz's suspend path: stop,
 * core deinit, MHI M3 and SoC reset. The module retains standby power across
 * reboot/S5; leaving it running can make it disappear from the PCI bus.
 * The wait is bounded; bus mastering is disabled last. */
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

/* Follow OpenBSD's suspend/resume order: QUIESCE stops firmware and DMA;
 * SUSPEND does nothing in qwz; RESUME resets rings; WAKEUP reloads firmware.
 * Interface configuration survives. macOS restores PCI BARs and MSI. */
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
	printf("QwzWifi: wake: hardware %s\n",
	    qwzmac_qwz_power(qd) ? "running" : "initialization failed (radio off)");
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

int
qwzmac_mgmt_busy(struct qwzmac_dev *qd)
{
	return qd == NULL || !qd->attached || mountroot_pending > 0 ||
	    qwzmac_suspended != 0;
}
