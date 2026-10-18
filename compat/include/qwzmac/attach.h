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

#ifdef __cplusplus
}
#endif

#endif /* _QWZMAC_ATTACH_H_ */
