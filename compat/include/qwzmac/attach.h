/*
 * QwzWifi: entry points the kext uses to run qwz the way OpenBSD's
 * autoconf(9) and ifconfig(8) would (compat/autoconf.c).
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
 */
struct qwzmac_netif_ops {
	void	*cookie;
	/* Firmware is up and the MAC is known: publish the interface. */
	void	(*ready)(void *cookie, const uint8_t mac[6]);
	/* A received Ethernet frame. The buffer is only valid during the call. */
	void	(*input)(void *cookie, const void *frame, size_t len);
	/* Link went up (associated, keys installed) or down. */
	void	(*link)(void *cookie, int up, uint64_t baudrate);
};

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
 * see compat/autoconf.c), or 0 when not associated.
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
	int		mcs;		/* HT MCS index of the last TX, or -1 */
	uint64_t	rate_bps;	/* see qwzmac_qwz_tx_rate() */
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

#ifdef __cplusplus
}
#endif

#endif /* _QWZMAC_ATTACH_H_ */
