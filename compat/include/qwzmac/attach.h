/*
 * QwzWifi: entry points the kext uses to run qwz the way OpenBSD's
 * autoconf(9) and ifconfig(8) would (adapter/).
 * Only plain C types cross this line.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_ATTACH_H_
#define _QWZMAC_ATTACH_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The network to join: "ifconfig qwz0 nwid <ssid> wpakey <passphrase>".
 * Exactly this SSID is joined, nothing else (no auto-join).
 */
struct qwzmac_network {
	char	ssid[33];	/* 1..32 bytes */
	char	passphrase[64];	/* 8..63 characters, WPA2-PSK */
};

/*
 * Callbacks into the kext. All may be called with the spl lock held,
 * except ready(), which runs in the bring-up thread without it.
 * qwzmac_qwz_disconnect() stops all but ready().
 */
struct qwzmac_netif_ops {
	void	*cookie;
	/* Firmware is up and the MAC is known: publish the interface. */
	void	(*ready)(void *cookie, const uint8_t mac[6]);
	/* A received Ethernet frame. The buffer is only valid during the call. */
	void	(*input)(void *cookie, const void *frame, size_t len);
	/* Link went up (associated, keys installed) or down. */
	void	(*link)(void *cookie, int up, uint64_t baudrate);
	/*
	 * The radio is about to go down, went down, or is up and running
	 * (QWZMAC_RADIO_*): after bring-up, around sleep and wake, and around
	 * qwzmac_qwz_set_power(). Optional; called with the spl lock held.
	 */
	void	(*radio)(void *cookie, int state);
	/* Eight more associated beacons received; signal only, spl lock held. */
	void	(*beacon)(void *cookie);
};
#define QWZMAC_RADIO_DOWN	0
#define QWZMAC_RADIO_UP		1
#define QWZMAC_RADIO_GOING_DOWN	2

/*
 * Match and attach qwz to the IOPCIDevice. Returns 0 and an opaque device
 * handle, or an errno. Firmware bring-up continues in a kernel thread;
 * then the interface comes up and joins *net, or only scans if net is
 * NULL. The passphrase is turned into the PSK and wiped from *net before
 * this returns.
 */
int	qwzmac_qwz_attach(void *pcidev, const struct qwzmac_netif_ops *ops,
	    struct qwzmac_network *net, void **devp);

/*
 * Send one Ethernet frame (destination, source, type, payload). Returns 0,
 * or an errno if it was dropped (link down, queue full, too big).
 */
int	qwzmac_qwz_output(void *dev, const void *frame, size_t len);

/*
 * The rate the firmware currently sends at, in bit/s (estimated for HT,
 * see adapter/status.c), or 0 when not associated.
 */
uint64_t qwzmac_qwz_tx_rate(void *dev);

/*
 * Stop all calls into the kext (ops). When this returns, no callback is
 * running and none will start. Frames can still be sent.
 */
void	qwzmac_qwz_disconnect(void *dev);

/* Wait for bring-up to finish, detach qwz and free everything. */
void	qwzmac_qwz_detach(void *dev);

/*
 * System shutdown/restart: power the chip down (qwz_activate(QUIESCE)),
 * then turn bus mastering off. Waits at most 10 s.
 */
void	qwzmac_qwz_shutdown(void *dev);

/*
 * System sleep: power the chip down (qwz_activate(QUIESCE)), waiting at
 * most 10 s. Wake: bring it up again in a thread (RESUME, WAKEUP); the
 * interface joins the network again on its own.
 */
void	qwzmac_qwz_sleep(void *dev);
void	qwzmac_qwz_wake(void *dev);

/*
 * Management, for a Wi-Fi app such as HeliPort. All may block (the join
 * reloads the firmware) and must not be called with the spl lock held.
 * They return 0 or an errno; EBUSY while bring-up, sleep or shutdown
 * owns the chip.
 */

/* Join exactly this network (open if the passphrase is empty; 64 hex
 * digits are taken as the PSK). The passphrase is wiped. Brings the
 * interface up. */
int	qwzmac_qwz_join(void *dev, struct qwzmac_network *net);
/* Forget the network: scan only, join nothing. */
int	qwzmac_qwz_leave(void *dev);
/* Interface up (radio on, joins the network if one is set) or down. */
int	qwzmac_qwz_set_power(void *dev, int on);
int	qwzmac_qwz_power(void *dev);	/* 1 if up and running */
/* Start a background scan while associated (scanning goes on anyway
 * while not associated). */
void	qwzmac_qwz_scan(void *dev);
/* The firmware build id, e.g. "WLAN.HMT.1.1.c5-00302-...". */
void	qwzmac_qwz_fw_version(void *dev, char *buf, size_t len);

struct qwzmac_sta_info {
	int		state;		/* 0 INIT, 1 SCAN, 2 AUTH, 3 ASSOC, 4 RUN */
	int		phy;		/* 0 11a, 1 11b, 2 11g, 3 11n, 4 11ac, 5 11ax */
	uint8_t		ssid[32];
	uint8_t		ssid_len;
	uint8_t		bssid[6];
	unsigned	channel;
	unsigned	width_mhz;
	int		rssi_dbm;
	int		noise_dbm;
	int		mcs;		/* HT/VHT/HE MCS index of the last TX, or -1 */
	uint64_t	rate_bps;	/* see qwzmac_qwz_tx_rate() */
	unsigned	gi_ns;		/* guard interval of that rate: 400, 800 */
	unsigned	nss;		/* spatial streams of the link */
};
/* state is always set; the rest only when associated (returns ENOTCONN
 * otherwise). */
int	qwzmac_qwz_sta_info(void *dev, struct qwzmac_sta_info *si);

struct qwzmac_bss {
	uint8_t		ssid[32];
	uint8_t		ssid_len;
	uint8_t		bssid[6];
	unsigned	channel;
	int		rssi_dbm;
	/* net80211's RSN fields as is (IEEE80211_PROTO_*, _AKM_*, _CIPHER_*). */
	uint32_t	supported_rsnprotos, rsnprotos;
	uint32_t	supported_rsnakms, rsnakms;
	uint32_t	rsnciphers, rsngroupcipher, rsngroupmgmtcipher, rsncipher;
	uint16_t	capinfo, rsncaps;
};
/* Copy up to max networks from the last scans; returns how many. */
int	qwzmac_qwz_scan_results(void *dev, struct qwzmac_bss *out, int max);

/* Native observations for one completed ticket (not the entire BSS cache).
 * Only real beacon/probe RX during that scan tags a node.
 * Security IE copied unchanged from net80211.
 * SSID/rates/channel IEs are reconstructed from the same cached node. */
struct qwzmac_scan_bss {
	uint8_t ssid[32], ssid_len, bssid[6];
	uint16_t channel, interval, capability, ie_len;
	int32_t rssi_dbm;
	uint8_t ies[384];
};
struct qwzmac_scan_request {
	uint8_t active, restricted, ssid_len;
	uint8_t ssid[32], channels[32], bssid[6];
};
int qwzmac_qwz_scan_start(void *dev, const struct qwzmac_scan_request *, uint32_t *ticket);
int qwzmac_qwz_scan_poll(void *dev, uint32_t generation);
int qwzmac_qwz_scan_snapshot(void *dev, uint32_t ticket, struct qwzmac_scan_bss *out, int max);
/* Fresh RX for this running (or successfully completed) ticket, without
 * claiming scan completion. No observations from queued or failed tickets. */
int qwzmac_qwz_scan_progress(void *dev, uint32_t ticket, struct qwzmac_scan_bss *out, int max);

/* macOS supplies a derived PMK; never treat it as a password. */
struct qwzmac_native_join {
	uint8_t ssid[32], ssid_len, bssid[6], secured;
	uint8_t pmk[32];
};
struct qwzmac_native_status {
	struct qwzmac_sta_info station;
	uint32_t channel_flags; /* QWZMAC_CHAN_* of the actual associated channel */
	struct qwzmac_scan_bss bss;
	uint32_t authorized, beacon_sequence, beacon_length;
	uint8_t beacon[2048];
	uint8_t rate_count, rates[32], ht_mcs[10];
	uint16_t vht_mcs, he_mcs, negotiated_rsn_length;
	uint8_t negotiated_rsn[257];
};
int qwzmac_qwz_set_mac(void *, const uint8_t[6]);
int qwzmac_qwz_native_join(void *, struct qwzmac_native_join *);
int qwzmac_qwz_native_leave(void *);
int qwzmac_qwz_native_status(void *, struct qwzmac_native_status *);

struct qwzmac_chan {
	unsigned	channel;	/* IEEE channel number */
	unsigned	freq_mhz;
	uint32_t	flags;		/* QWZMAC_CHAN_* */
};
#define QWZMAC_CHAN_2GHZ	0x01
#define QWZMAC_CHAN_5GHZ	0x02
#define QWZMAC_CHAN_PASSIVE	0x04	/* no probe requests: listen only */
#define QWZMAC_CHAN_40MHZ	0x08	/* HT40 allowed */
#define QWZMAC_CHAN_80MHZ	0x10	/* VHT80 allowed */
#define QWZMAC_CHAN_DFS	0x20
#define QWZMAC_CHAN_EXT_ABOVE 0x40 /* HT40 extension channel above primary */
#define QWZMAC_CHAN_HE		0x80	/* 802.11ax allowed (and not turned off) */
/* The channels the firmware's regulatory domain allows; returns how many. */
int	qwzmac_qwz_channels(void *dev, struct qwzmac_chan *out, int max);
/*
 * The country of the firmware's regulatory domain ("US", "00" for world)
 * into cc[3]. Only reported to macOS; nothing applies macOS's country to
 * the chip. ENXIO before the firmware sent one.
 */
int	qwzmac_qwz_country(void *dev, char cc[3]);

#ifdef __cplusplus
}
#endif

#endif /* _QWZMAC_ATTACH_H_ */
