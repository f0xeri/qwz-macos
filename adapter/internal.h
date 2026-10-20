/* QwzWifi adapter internals. SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWZMAC_ADAPTER_INTERNAL_H
#define QWZMAC_ADAPTER_INTERNAL_H

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

typedef int (*qwzmac_ioctl_fn)(struct ifnet *, u_long, void *);

/* Shared only between adapter translation units; public API is qwzmac/attach.h.
 * See the .c definitions for locking and call context. */
int qwzmac_ioctl(struct ifnet *ifp, u_long cmd, void *data);
int qwzmac_mgmt_busy(struct qwzmac_dev *qd);
void qwzmac_radio(struct qwzmac_dev *qd, int state);
void qwzmac_radio_now(struct qwzmac_dev *qd);
int qwzmac_set_network(struct qwzmac_dev *qd, struct ifnet *ifp,
    qwzmac_ioctl_fn ioc);

#endif
