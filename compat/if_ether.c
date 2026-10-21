/*
 * QwzWifi compat: the part of OpenBSD if_ethersubr.c that net80211 uses.
 *
 * The kext exchanges complete Ethernet frames through if_enqueue() and
 * if_input(). There is no OpenBSD IP stack or ether_output() data path.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#define ETHER_CRC_POLY_LE	0xedb88320
#define ETHER_CRC_POLY_BE	0x04c11db6

u_int32_t
ether_crc32_le_update(u_int32_t crc, const u_int8_t *buf, size_t len)
{
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? ETHER_CRC_POLY_LE : 0);
	}
	return crc;
}

u_int32_t
ether_crc32_be_update(u_int32_t crc, const u_int8_t *buf, size_t len)
{
	size_t i;
	int bit, carry;
	u_int8_t c;

	for (i = 0; i < len; i++) {
		for (c = buf[i], bit = 0; bit < 8; bit++, c >>= 1) {
			carry = ((crc & 0x80000000U) ? 1 : 0) ^ (c & 0x01);
			crc <<= 1;
			if (carry)
				crc = (crc ^ ETHER_CRC_POLY_BE) | carry;
		}
	}
	return crc;
}

u_int32_t
ether_crc32_le(const u_int8_t *buf, size_t len)
{
	return ether_crc32_le_update(0xffffffff, buf, len);
}

u_int32_t
ether_crc32_be(const u_int8_t *buf, size_t len)
{
	return ether_crc32_be_update(0xffffffff, buf, len);
}

void
ether_ifattach(struct ifnet *ifp)
{
	struct arpcom *ac = (struct arpcom *)ifp;

	ifp->if_type = IFT_ETHER;
	ifp->if_addrlen = ETHER_ADDR_LEN;
	ifp->if_hdrlen = ETHER_HDR_LEN;
	ifp->if_mtu = ETHERMTU;
	if (ifp->if_output == NULL)
		ifp->if_output = ether_output;
	if (ifp->if_hardmtu == 0)
		ifp->if_hardmtu = ETHERMTU;
	LIST_INIT(&ac->ac_multiaddrs);
}

void
ether_ifdetach(struct ifnet *ifp)
{
	if_deactivate(ifp);
}

int
ether_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
    struct rtentry *rt)
{
	static int warned;

	(void)dst; (void)rt;
	if (!warned) {
		warned = 1;
		printf("%s: ether_output called, but frames are sent with "
		    "if_enqueue()\n", ifp->if_xname);
	}
	m_freem(m);
	return EOPNOTSUPP;
}

/*
 * QWZMAC: the multicast list is not kept. qwz never programs a multicast
 * filter from it; in OpenBSD its only effect is the ENETRESET return,
 * which makes qwz stop and re-init the whole device.
 */
int
ether_addmulti(struct ifreq *ifr, struct arpcom *ac)
{
	(void)ifr; (void)ac;
	return 0;
}

int
ether_delmulti(struct ifreq *ifr, struct arpcom *ac)
{
	(void)ifr; (void)ac;
	return 0;
}

int
ether_ioctl(struct ifnet *ifp, struct arpcom *arp, u_long cmd, caddr_t data)
{
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		break;

	case SIOCSIFMTU:
		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > ifp->if_hardmtu)
			error = EINVAL;
		else
			ifp->if_mtu = ifr->ifr_mtu;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_flags & IFF_MULTICAST) {
			error = (cmd == SIOCADDMULTI) ?
			    ether_addmulti(ifr, arp) :
			    ether_delmulti(ifr, arp);
		} else
			error = ENOTTY;
		break;

	default:
		error = ENOTTY;
	}

	return (error);
}
