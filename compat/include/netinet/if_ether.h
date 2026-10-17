/*
 * QwzWifi compat: OpenBSD <netinet/if_ether.h> (driver subset).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_NETINET_IF_ETHER_H_
#define _QWZMAC_NETINET_IF_ETHER_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <net/if.h>

#define	ETHER_ADDR_LEN	6
#define ETHER_TYPE_LEN	2
#define ETHER_CRC_LEN	4
#define ETHER_HDR_LEN	((ETHER_ADDR_LEN * 2) + ETHER_TYPE_LEN)
#define ETHER_MIN_LEN	64
#define ETHER_MAX_LEN	1518
#define ETHER_MAX_DIX_LEN	1536
#define ETHER_VLAN_ENCAP_LEN	4
#define ETHERMTU	(ETHER_MAX_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#define ETHERMIN	(ETHER_MIN_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#define ETHER_ALIGN	2

#define ETHERTYPE_IP		0x0800
#define ETHERTYPE_ARP		0x0806
#define ETHERTYPE_REVARP	0x8035
#define ETHERTYPE_VLAN		0x8100
#define ETHERTYPE_IPV6		0x86DD
#define ETHERTYPE_PAE		0x888E
#define ETHERTYPE_EAPOL		ETHERTYPE_PAE
#define ETHERTYPE_QINQ		0x88A8

struct ether_addr {
	u_int8_t ether_addr_octet[ETHER_ADDR_LEN];
};

struct	ether_header {
	u_int8_t  ether_dhost[ETHER_ADDR_LEN];
	u_int8_t  ether_shost[ETHER_ADDR_LEN];
	u_int16_t ether_type;
};

#define	ETHER_IS_MULTICAST(addr) (*(addr) & 0x01)
#define	ETHER_IS_BROADCAST(addr) \
	(((addr)[0] & (addr)[1] & (addr)[2] & \
	  (addr)[3] & (addr)[4] & (addr)[5]) == 0xff)
#define	ETHER_IS_ANYADDR(addr)		\
	(((addr)[0] | (addr)[1] | (addr)[2] | \
	  (addr)[3] | (addr)[4] | (addr)[5]) == 0x00)
#define	ETHER_IS_EQ(a1, a2)	(memcmp((a1), (a2), ETHER_ADDR_LEN) == 0)

struct ether_multi;

struct	arpcom {
	struct	 ifnet ac_if;
	u_int8_t ac_enaddr[ETHER_ADDR_LEN];
	char	 ac__pad[2];
	LIST_HEAD(, ether_multi) ac_multiaddrs;
	int	 ac_multicnt;
	int	 ac_multirangecnt;
};

extern u_int8_t etherbroadcastaddr[ETHER_ADDR_LEN];
extern u_int8_t etheranyaddr[ETHER_ADDR_LEN];

void	ether_ifattach(struct ifnet *);
void	ether_ifdetach(struct ifnet *);
int	ether_ioctl(struct ifnet *, struct arpcom *, u_long, caddr_t);
int	ether_output(struct ifnet *, struct mbuf *, struct sockaddr *,
	    struct rtentry *);
char	*ether_sprintf(u_char *);
int	ether_addmulti(struct ifreq *, struct arpcom *);
int	ether_delmulti(struct ifreq *, struct arpcom *);
u_int32_t ether_crc32_le_update(u_int32_t, const u_int8_t *, size_t);
u_int32_t ether_crc32_be_update(u_int32_t, const u_int8_t *, size_t);
u_int32_t ether_crc32_le(const u_int8_t *, size_t);
u_int32_t ether_crc32_be(const u_int8_t *, size_t);

#endif /* _QWZMAC_NETINET_IF_ETHER_H_ */
