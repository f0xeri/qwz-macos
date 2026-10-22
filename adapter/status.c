/*
 * QwzWifi adapter: Read-only station, channel and scan snapshots for the macOS clients.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "internal.h"

/* qwz stores the signal as a signed dBm value in net80211's u_int8_t. */
#define QWZMAC_RSSI_DBM(ni)	((int)(int8_t)(ni)->ni_rssi)

/* Match the 40 MHz peer setup in qwz_peer_assoc_h_phymode(). */
static int
qwzmac_chan40(struct ieee80211_node *ni)
{
	uint8_t sco = ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK;

	return IEEE80211_CHAN_40MHZ_ALLOWED(ni->ni_chan) &&
	    ieee80211_node_supports_ht_chan40(ni) &&
	    (sco == IEEE80211_HTOP0_SCO_SCA || sco == IEEE80211_HTOP0_SCO_SCB);
}

/* Width in MHz the station runs on: the width the firmware vdev was started
 * with, else what the HT operation element says. */
static int
qwzmac_width(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	struct qwz_softc *sc = ic->ic_softc;
	int width = qwz_vdev_width(&sc->sc_vif);

	if (width != 0)
		return width;
	return (ni->ni_flags & IEEE80211_NODE_HT) && qwzmac_chan40(ni) ? 40 : 20;
}

/* Spatial streams of a VHT link: those both sides handle, at most our chains. */
static int
qwzmac_vht_nss(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	struct qwz_softc *sc = ic->ic_softc;
	int ss, nss = 1;

	for (ss = 1; ss <= IEEE80211_VHT_NUM_SS && ss <= sc->num_tx_chains; ss++)
		if (((ni->ni_vht_rxmcs & IEEE80211_VHT_MCS_FOR_SS_MASK(ss)) >>
		    IEEE80211_VHT_MCS_FOR_SS_SHIFT(ss)) !=
		    IEEE80211_VHT_MCS_SS_NOT_SUPP)
			nss = ss;
	return nss;
}

/*
 * Rate of a VHT link in bit/s, from the MCS the firmware reported with its
 * last transmit completion. The base rates are 802.11ac-2013 Table 22-30..,
 * one stream, 20 MHz, long guard interval; wider channels carry
 * 52/108/234 data subcarriers instead of 52, a short guard interval adds a
 * ninth. (net80211's own table of MIMO2 80 MHz rates misses MCS 7.)
 */
static uint64_t
qwzmac_vht_rate(struct ieee80211com *ic, struct ieee80211_node *ni, int mcs)
{
	static const uint32_t mcs_kbps[] = { 6500, 13000, 19500, 26000, 39000,
	    52000, 58500, 65000, 78000, 86700 };
	int width = qwzmac_width(ic, ni);
	int nsd = width >= 80 ? 234 : width >= 40 ? 108 : 52;
	uint64_t kbps;
	int sgi;

	if (mcs < 0 || mcs >= (int)nitems(mcs_kbps))
		return 0;
	kbps = (uint64_t)mcs_kbps[mcs] * qwzmac_vht_nss(ic, ni) * nsd / 52;
	if (width >= 80)
		sgi = (ni->ni_flags & IEEE80211_NODE_VHT_SGI80) != 0;
	else
		sgi = ieee80211_ra_use_ht_sgi(ni);
	if (sgi)
		kbps = kbps * 10 / 9;
	return kbps * 1000;
}

/* spl lock held, associated. Sets *mcsp to the HT (or VHT) MCS index or -1. */
static uint64_t
qwzmac_ni_tx_rate(struct ieee80211com *ic, struct ieee80211_node *ni,
    int *mcsp)
{
	const struct ieee80211_ht_rateset *rs;
	int i, mcs, nss, chan40, sgi;

	*mcsp = -1;
	if (ni->ni_flags & IEEE80211_NODE_VHT) {
		*mcsp = ni->ni_txmcs;
		return qwzmac_vht_rate(ic, ni, ni->ni_txmcs);
	}
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

/* TX rate in bit/s, or 0 when disconnected. Firmware supplies the MCS;
 * HT stream count, width and guard interval come from negotiated settings. */
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
		si->width_mhz = qwzmac_width(ic, ni);
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
		/*
		 * Every channel the regulatory data allows, not net80211's
		 * active set: that one follows the current phy mode, and
		 * once associated to an 802.11ac AP (mode 11ac) it keeps only
		 * the 5 GHz VHT channels, which dropped the 2.4 GHz networks
		 * from the macOS Wi-Fi list.
		 */
		if (c->ic_flags == 0)
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

static int
qwzmac_scan_copy(void *p, uint32_t ticket, struct qwzmac_scan_bss *out, int max, int progress)
{
	struct qwzmac_dev *qd = p;
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	int n = 0;
	if (out == NULL || max <= 0 || qwzmac_mgmt_busy(qd))
		return 0;
	qwzmac_spl_lock();
	ic = &((struct qwz_softc *)qd->dev)->sc_ic;
	/* Live results follow the reference fronts' per-RX ScanResult path.
	 * The final snapshot still requires successful firmware completion. */
	if (!qwzmac_scan_publishable(ticket, ic->ic_if.if_qwzmac_scan_active,
	    ic->ic_if.if_qwzmac_scan_done, ic->ic_if.if_qwzmac_scan_done_error, progress)) {
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

int
qwzmac_qwz_scan_snapshot(void *p, uint32_t ticket, struct qwzmac_scan_bss *out, int max)
{
	return qwzmac_scan_copy(p, ticket, out, max, 0);
}

int
qwzmac_qwz_scan_progress(void *p, uint32_t ticket, struct qwzmac_scan_bss *out, int max)
{
	return qwzmac_scan_copy(p, ticket, out, max, 1);
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
		/* Current-channel metadata for airportd's post-join link event.
		 * This OpenBSD has no separate CHAN_DFS/HT40U flags: use the same
		 * dfs_set rule as qwz's WMI channel list, and the negotiated HT op. */
		if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) status->channel_flags |= QWZMAC_CHAN_2GHZ;
		if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan)) status->channel_flags |= QWZMAC_CHAN_5GHZ;
		if (ni->ni_chan->ic_flags & IEEE80211_CHAN_PASSIVE) status->channel_flags |= QWZMAC_CHAN_PASSIVE;
		if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan) &&
		    (ni->ni_chan->ic_flags & IEEE80211_CHAN_PASSIVE)) status->channel_flags |= QWZMAC_CHAN_DFS;
		if ((ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK) == IEEE80211_HTOP0_SCO_SCA)
			status->channel_flags |= QWZMAC_CHAN_EXT_ABOVE;
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
