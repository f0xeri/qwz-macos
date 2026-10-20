/*
 * QwzWifi adapter: Connection, radio and scan requests translated into net80211 operations.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "internal.h"

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

int
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
int
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
	printf("QwzWifi: native join accepted by net80211\n");
	rw_enter_write(&sc->ioctl_rwl);
	qwzmac_spl_lock();
	if (!(ifp->if_flags & IFF_RUNNING)) { error = ENETDOWN; goto unlock; }
	/* As in AirportItlwm/AirportMtkWlan startWCLJoin: install the target
	 * atomically, then use the existing scan. Do not first clear the SSID
	 * and queue a separate scan-only transition via native_leave(). */
	qd->have_net = 0;
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
		/* An in-progress hardware scan selects this BSSID at completion.
		 * Only request SCAN when not already there, as the reference fronts do. */
		if (ic->ic_state != IEEE80211_S_SCAN)
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
