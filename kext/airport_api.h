/*
 * QwzWifi <-> QwzAirport: the seam between the driver kext and the native
 * Wi-Fi front (airport/, docs/native-wifi-research.md §8.5).
 *
 * The two kexts do not link against each other. QwzAirport matches on the
 * QwzWifi service and calls
 *   provider->callPlatformFunction(QWZ_AIRPORT_CONNECT, false,
 *       &front, &driver, NULL, NULL)
 * QwzWifi fills `driver` and keeps a copy of `front`. Before it goes away,
 * QwzAirport calls QWZ_AIRPORT_DISCONNECT (p1 = &front's cookie); when that
 * returns, no front callback is running and none will start.
 *
 * Only plain C types cross this line: no OpenBSD (net80211) and no IO80211
 * types. All of net80211 stays behind the driver functions, in QwzWifi.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_AIRPORT_API_H
#define QWZ_AIRPORT_API_H

#include <stddef.h>
#include <stdint.h>

#include "../compat/include/qwzmac/attach.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any change of the structures below; both sides must agree. */
#define QWZ_AIRPORT_API_VERSION	8
#define QWZ_AIRPORT_CONNECT	"QwzAirportConnect"
#define QWZ_AIRPORT_DISCONNECT	"QwzAirportDisconnect"

/* Driver -> front events (qwz_airport_front.event). */
enum {
	QWZ_AIRPORT_EV_RADIO = 1,	/* data: int, QWZMAC_RADIO_* */
};

/*
 * Front -> driver, filled by QwzWifi. Each call may block (firmware
 * commands), so the front must not hold its work-loop gate across the slow
 * ones (set_power). They return 0 or an errno; EBUSY while bring-up, sleep
 * or shutdown owns the chip, ENXIO once QwzWifi is stopping.
 */
struct qwz_airport_driver {
	uint32_t	version;	/* QWZ_AIRPORT_API_VERSION */
	uint32_t	size;		/* sizeof(struct qwz_airport_driver) */
	void		*dev;		/* QwzWifi, opaque */

	/* 1 if the radio is up and running. */
	int	(*power)(void *dev);
	/* Radio on (joins nothing by itself in native mode) or off. Slow. */
	int	(*set_power)(void *dev, int on);
	/* The factory MAC address. */
	int	(*hw_address)(void *dev, uint8_t mac[6]);
	/* The firmware build id. */
	void	(*fw_version)(void *dev, char *buf, size_t len);
	/* The firmware's country, see qwzmac_qwz_country(). */
	int	(*country)(void *dev, char cc[3]);
	/* The allowed channels; returns how many, see qwzmac_qwz_channels(). */
	int	(*channels)(void *dev, struct qwzmac_chan *out, int max);
	/* Association state, see qwzmac_qwz_sta_info(). */
	int	(*sta_info)(void *dev, struct qwzmac_sta_info *si);
	/* Arm a scan with these parameters; ticket identifies the actual
	 * hardware cycle that consumes them. Poll returns EINPROGRESS until
	 * that ticket ends, then 0 or errno (including forced-cleanup errors).
	 * Never wait for firmware while holding the WCL gate. */
	int (*scan_start)(void *dev, const struct qwzmac_scan_request *, uint32_t *ticket);
	int (*scan_poll)(void *dev, uint32_t generation);
	/* Only nodes actually heard during this successfully completed ticket. */
	int (*scan_snapshot)(void *dev, uint32_t ticket, struct qwzmac_scan_bss *out, int max);
	/* True only when the Ethernet fallback is withheld for this front. */
	int (*data_owner)(void *dev);
	int (*set_mac)(void *dev, const uint8_t mac[6]);
	int (*join)(void *dev, struct qwzmac_native_join *);
	int (*leave)(void *dev);
	int (*join_status)(void *dev, struct qwzmac_native_status *);
	int (*output)(void *dev, const void *frame, size_t len);
};

/* Driver -> front, filled by QwzAirport. */
struct qwz_airport_front {
	uint32_t	version;	/* QWZ_AIRPORT_API_VERSION */
	uint32_t	size;		/* sizeof(struct qwz_airport_front) */
	void		*cookie;
	/*
	 * An event (QWZ_AIRPORT_EV_*). Called from a QwzWifi thread, never with
	 * qwz's lock held and never from the front's own work loop; it may take
	 * the front's gate. The data is only valid during the call.
	 */
	void	(*event)(void *cookie, int event, const void *data, size_t len);
	/* RX under the driver's recursive spl lock: copy before returning,
	 * NEVER acquire the WCL gate here. Disconnect drains this too. */
	void (*input)(void *cookie, const void *frame, size_t len);
	/* Quiesce native workers, keep both interfaces registered. Outside all
	 * gates/spl; must NOT disconnect the seam from inside this callback. */
	int (*park)(void *cookie);
};

#ifdef __cplusplus
}
#endif

#endif /* QWZ_AIRPORT_API_H */
