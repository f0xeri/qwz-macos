/* QwzWifi compat: OpenBSD <netinet/in.h> (minimal). SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QWZMAC_NETINET_IN_H_
#define _QWZMAC_NETINET_IN_H_
#include <sys/types.h>
#include <sys/socket.h>
struct in_addr {
	in_addr_t s_addr;
};
#define IPPROTO_IP	0
#define IPPROTO_ICMP	1
#define IPPROTO_TCP	6
#define IPPROTO_UDP	17
#define IPPROTO_IPV6	41
#define IPPROTO_ICMPV6	58
#endif
