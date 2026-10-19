/*
 * QwzWifi compat: OpenBSD <net/if.h> and <net/if_var.h> (driver subset).
 * struct ifnet keeps OpenBSD's field names; the macOS side (an
 * IOEthernetController in stage 3) bridges to it. ifq is a simple list.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_NET_IF_H_
#define _QWZMAC_NET_IF_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mbuf.h>
#include <sys/task.h>
#include <sys/timeout.h>
#include <sys/refcnt.h>
#include <qwzmac/attach.h>

#define	IF_NAMESIZE	16
#define	IFNAMSIZ	IF_NAMESIZE
#define	IFDESCRSIZE	64

#define LINK_STATE_UNKNOWN	0
#define LINK_STATE_INVALID	1
#define LINK_STATE_DOWN		2
#define LINK_STATE_KALIVE_DOWN	3
#define LINK_STATE_UP		4
#define LINK_STATE_HALF_DUPLEX	5
#define LINK_STATE_FULL_DUPLEX	6
#define LINK_STATE_IS_UP(_s)	\
		((_s) >= LINK_STATE_UP || (_s) == LINK_STATE_UNKNOWN)

#define	IFF_UP		0x1
#define	IFF_BROADCAST	0x2
#define	IFF_DEBUG	0x4
#define	IFF_LOOPBACK	0x8
#define	IFF_POINTOPOINT	0x10
#define	IFF_STATICARP	0x20
#define	IFF_RUNNING	0x40
#define	IFF_NOARP	0x80
#define	IFF_PROMISC	0x100
#define	IFF_ALLMULTI	0x200
#define	IFF_OACTIVE	0x400
#define	IFF_SIMPLEX	0x800
#define	IFF_LINK0	0x1000
#define	IFF_LINK1	0x2000
#define	IFF_LINK2	0x4000
#define	IFF_MULTICAST	0x8000

#define	IFXF_MPSAFE		0x1
#define	IFXF_MONITOR		0x100

#define	IF_Kbps(x)	((x) * 1000ULL)
#define	IF_Mbps(x)	(IF_Kbps((x) * 1000ULL))
#define	IF_Gbps(x)	(IF_Mbps((x) * 1000ULL))

#define	IFCAP_CSUM_IPv4		0x00000001
#define	IFCAP_CSUM_TCPv4	0x00000002
#define	IFCAP_CSUM_UDPv4	0x00000004
#define	IFCAP_VLAN_MTU		0x00000010
#define	IFCAP_VLAN_HWTAGGING	0x00000020

#define	IF_WIRELESS_DEFAULT_PRIORITY	4

#define	IFT_ETHER	0x06
#define	IFT_IEEE80211	0x47

#define IFQ_MAXLEN	256

struct ifnet;

struct ifqueue {
	struct ifnet		*ifq_if;
	struct mbuf_list	 ifq_list;	/* QWZMAC: plain FIFO */
	unsigned int		 ifq_len;
	unsigned int		 ifq_oactive;
	unsigned int		 ifq_maxlen;
	uint64_t		 ifq_qdrops;
};

enum if_counters {
	ifc_ipackets,
	ifc_ierrors,
	ifc_opackets,
	ifc_oerrors,
	ifc_collisions,
	ifc_ibytes,
	ifc_obytes,
	ifc_imcasts,
	ifc_omcasts,
	ifc_iqdrops,
	ifc_oqdrops,
	ifc_noproto,
	ifc_ncounters
};

struct ifnet {
	void	*if_softc;
	char	if_xname[IFNAMSIZ];
	caddr_t	if_bpf;
	unsigned int if_index;
	short	if_timer;
	unsigned short if_flags;
	int	if_xflags;
	u_char	if_type;
	u_char	if_addrlen;
	u_char	if_hdrlen;
	u_char	if_link_state;
	uint32_t if_mtu;
	uint64_t if_baudrate;
	uint32_t if_capabilities;
	uint64_t if_data_counters[ifc_ncounters];
	uint32_t if_hardmtu;
	int	if_priority;

	void	(*if_start)(struct ifnet *);
	int	(*if_ioctl)(struct ifnet *, u_long, caddr_t);
	void	(*if_watchdog)(struct ifnet *);
	int	(*if_output)(struct ifnet *, struct mbuf *, struct sockaddr *,
		    struct rtentry *);

	struct	ifqueue if_snd;
	struct	timeout if_slowtimo;	/* QWZMAC: drives if_watchdog */

	/* QWZMAC: macOS-side hooks, set by the kext glue. */
	void	(*if_qwzmac_input)(struct ifnet *, struct mbuf *);
	void	(*if_qwzmac_link_state)(struct ifnet *);
	void	*if_qwzmac_cookie;
	uint32_t if_qwzmac_scan_generation;
	int if_qwzmac_scan_error;
	struct qwzmac_scan_request if_qwzmac_scan_request;
	uint32_t if_qwzmac_scan_ticket, if_qwzmac_scan_active, if_qwzmac_scan_done;
	int if_qwzmac_scan_pending, if_qwzmac_scan_done_error;
};

#define if_ipackets	if_data_counters[ifc_ipackets]
#define if_ierrors	if_data_counters[ifc_ierrors]
#define if_opackets	if_data_counters[ifc_opackets]
#define if_oerrors	if_data_counters[ifc_oerrors]
#define if_collisions	if_data_counters[ifc_collisions]
#define if_ibytes	if_data_counters[ifc_ibytes]
#define if_obytes	if_data_counters[ifc_obytes]
#define if_imcasts	if_data_counters[ifc_imcasts]
#define if_omcasts	if_data_counters[ifc_omcasts]
#define if_iqdrops	if_data_counters[ifc_iqdrops]
#define if_oqdrops	if_data_counters[ifc_oqdrops]
#define if_noproto	if_data_counters[ifc_noproto]

struct	ifreq {
	char	ifr_name[IFNAMSIZ];
	union {
		struct	sockaddr	ifru_addr;
		struct	sockaddr	ifru_dstaddr;
		struct	sockaddr	ifru_broadaddr;
		short			ifru_flags;
		int			ifru_metric;
		int64_t			ifru_vnetid;
		uint64_t		ifru_media;
		caddr_t			ifru_data;
		unsigned int		ifru_index;
	} ifr_ifru;
#define	ifr_addr	ifr_ifru.ifru_addr
#define	ifr_dstaddr	ifr_ifru.ifru_dstaddr
#define	ifr_broadaddr	ifr_ifru.ifru_broadaddr
#define	ifr_flags	ifr_ifru.ifru_flags
#define	ifr_metric	ifr_ifru.ifru_metric
#define	ifr_mtu		ifr_ifru.ifru_metric
#define	ifr_hardmtu	ifr_ifru.ifru_metric
#define	ifr_media	ifr_ifru.ifru_media
#define	ifr_data	ifr_ifru.ifru_data
#define ifr_index	ifr_ifru.ifru_index
};

/* message format used to pass 80211 interface info */
struct if_ieee80211_data {
	uint8_t		ifie_channel;	/* IEEE80211_CHAN_MAX  == 255 */
	uint8_t		ifie_nwid_len;
	uint32_t	ifie_flags;	/* ieee80211com.ic_flags */
	uint32_t	ifie_xflags;	/* ieee80211com.ic xflags */
	uint8_t		ifie_nwid[32];	/* IEEE80211_NWID_LEN */
	uint8_t		ifie_addr[6];	/* IEEE80211_ADDR_LEN */
};

struct ifmediareq {
	char		ifm_name[IFNAMSIZ];
	uint64_t	ifm_current;
	uint64_t	ifm_mask;
	uint64_t	ifm_status;
	uint64_t	ifm_active;
	int		ifm_count;
	uint64_t	*ifm_ulist;
};

struct ifaddr {
	struct sockaddr	*ifa_addr;
	struct ifnet	*ifa_ifp;
};

/* ifq(9) */
void		 ifq_init(struct ifqueue *, struct ifnet *, unsigned int);
int		 ifq_enqueue(struct ifqueue *, struct mbuf *);
struct mbuf	*ifq_dequeue(struct ifqueue *);
unsigned int	 ifq_purge(struct ifqueue *);
#define ifq_len(_ifq)		((_ifq)->ifq_len)
#define ifq_empty(_ifq)		(ifq_len(_ifq) == 0)
#define ifq_set_maxlen(_ifq, _l) ((_ifq)->ifq_maxlen = (_l))
#define ifq_is_oactive(_ifq)	((_ifq)->ifq_oactive)
#define ifq_set_oactive(_ifq)	((_ifq)->ifq_oactive = 1)
#define ifq_clr_oactive(_ifq)	((_ifq)->ifq_oactive = 0)
static inline void ifq_restart(struct ifqueue *ifq) { (void)ifq; }

/* if(9) */
void	if_attach(struct ifnet *);
void	if_detach(struct ifnet *);
void	if_link_state_change(struct ifnet *);
int	if_setlladdr(struct ifnet *, const uint8_t *);
void	if_input(struct ifnet *, struct mbuf_list *);
int	if_enqueue(struct ifnet *, struct mbuf *);
void	if_start(struct ifnet *);
int	if_output(struct ifnet *, struct mbuf *);
void	if_purge(struct ifnet *);
int	ifpromisc(struct ifnet *, int);
struct ifnet *if_get(unsigned int);
void	if_put(struct ifnet *);
void	if_up(struct ifnet *);
void	if_down(struct ifnet *);
void	if_deactivate(struct ifnet *);
void	if_addgroup(struct ifnet *, const char *);

#endif /* _QWZMAC_NET_IF_H_ */
