/*
 * QwzWifi: the user client protocol of HeliPort, the Wi-Fi menu app of
 * itlwm (https://github.com/OpenIntelWireless/HeliPort, ClientKit/Common.h
 * and IoctlId.h, BSD-3-Clause). Same layout, renamed (hp_ prefix) so it
 * cannot clash with anything else. The sizes are checked below against
 * what clang gives HeliPort's own header on x86_64.
 *
 * HeliPort calls IOConnectCallStructMethod(selector, ...): the selector
 * is an HP_IOCTL_* id, with HP_IOCTL_MASK set for a "set". A get returns
 * the struct as output, a set passes it as input.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_HELIPORT_H
#define QWZ_HELIPORT_H

#include <stdint.h>

#define QWZ_IOCTL_HELIPORT_MODE 0x51575a01u /* privileged, no payload */

#define HP_IOCTL_MASK		0x800000
#define HP_IOCTL_VERSION	1
#define HP_NWID_LEN		32
#define HP_WPA_KEY_LEN		128
#define HP_ETHER_ADDR_LEN	6
#define HP_NO_SCAN_RESULT	20008	/* returned when the list is done */

enum hp_ioctl_id {
	HP_IOCTL_DRIVER_INFO,
	HP_IOCTL_STA_INFO,
	HP_IOCTL_POWER,
	HP_IOCTL_STATE,
	HP_IOCTL_NW_ID,
	HP_IOCTL_WPA_KEY,
	HP_IOCTL_ASSOCIATE,
	HP_IOCTL_DISASSOCIATE,
	HP_IOCTL_JOIN,
	HP_IOCTL_SCAN,
	HP_IOCTL_SCAN_RESULT,
	HP_IOCTL_TX_POWER_LEVEL,
	HP_IOCTL_NW_BSSID,
	HP_IOCTL_ID_MAX
};

struct hp_driver_info {
	unsigned int	version;
	char		fw_version[32];
	char		driver_version[32];
	char		bsd_name[32];		/* e.g. en4 */
};

struct hp_sta_info {
	unsigned int	version;
	int		op_mode;		/* 0 11a, 1 11b, 2 11g, 3 11n, 4 11ac, 5 11ax */
	int		max_mcs;
	int		cur_mcs;
	unsigned int	channel;
	uint16_t	band_width;		/* MHz */
	int16_t		rssi;			/* dBm */
	int16_t		noise;			/* dBm */
	unsigned int	rate;			/* Mbit/s */
	unsigned char	ssid[HP_NWID_LEN];
	uint8_t		bssid[HP_ETHER_ADDR_LEN];
};

struct hp_power {
	unsigned int	version;
	unsigned int	enabled;
	unsigned int	maxsleep;
};

struct hp_state {
	unsigned int	version;
	int		state;			/* net80211 IEEE80211_S_* */
};

struct hp_nw_id {
	unsigned int	version;
	unsigned int	len;
	char		nwid[HP_NWID_LEN];
};

struct hp_nw_bssid {
	unsigned int	version;
	char		bssid[HP_ETHER_ADDR_LEN];
};

struct hp_wpa_key {
	unsigned int	version;
	unsigned int	len;
	char		key[HP_WPA_KEY_LEN];
};

/* HeliPort fills nwid.nwid and wpa_key.key only, not the len fields. */
struct hp_associate {
	unsigned int	version;
	struct hp_nw_id	nwid;
	struct hp_wpa_key wpa_key;
};

struct hp_disassociate {
	unsigned int	version;
	unsigned char	ssid[HP_NWID_LEN];
};

struct hp_version_only {			/* scan, scan_result, tx_power */
	unsigned int	version;
};

struct hp_network_info {
	unsigned char	ssid[HP_NWID_LEN];
	int16_t		noise;
	int16_t		rssi;
	uint8_t		bssid[HP_ETHER_ADDR_LEN];
	uint32_t	channel;
	unsigned int	supported_rsnprotos;	/* net80211 values, as itlwm */
	unsigned int	rsn_protos;
	unsigned int	supported_rsnakms;
	unsigned int	rsn_akms;
	unsigned int	rsn_ciphers;
	int		rsn_groupcipher;
	int		rsn_groupmgmtcipher;
	uint16_t	ni_rsncaps;
	int		ni_rsncipher;
	uint32_t	recv_timestamp;
};

/* Measured with HeliPort's ClientKit/Common.h (clang, x86_64). */
static_assert(sizeof(struct hp_driver_info) == 100, "hp_driver_info");
static_assert(sizeof(struct hp_sta_info) == 72, "hp_sta_info");
static_assert(__builtin_offsetof(struct hp_sta_info, rate) == 28, "hp_sta_info.rate");
static_assert(__builtin_offsetof(struct hp_sta_info, bssid) == 64, "hp_sta_info.bssid");
static_assert(sizeof(struct hp_power) == 12, "hp_power");
static_assert(sizeof(struct hp_state) == 8, "hp_state");
static_assert(sizeof(struct hp_nw_id) == 40, "hp_nw_id");
static_assert(sizeof(struct hp_nw_bssid) == 12, "hp_nw_bssid");
static_assert(sizeof(struct hp_wpa_key) == 136, "hp_wpa_key");
static_assert(sizeof(struct hp_associate) == 180, "hp_associate");
static_assert(__builtin_offsetof(struct hp_associate, wpa_key) == 44, "hp_associate.wpa_key");
static_assert(sizeof(struct hp_disassociate) == 36, "hp_disassociate");
static_assert(sizeof(struct hp_version_only) == 4, "hp_version_only");
static_assert(sizeof(struct hp_network_info) == 88, "hp_network_info");
static_assert(__builtin_offsetof(struct hp_network_info, channel) == 44, "hp_network_info.channel");
static_assert(__builtin_offsetof(struct hp_network_info, ni_rsncaps) == 76, "hp_network_info.ni_rsncaps");
static_assert(__builtin_offsetof(struct hp_network_info, recv_timestamp) == 84, "hp_network_info.recv_timestamp");

#endif /* QWZ_HELIPORT_H */
