/*
 * QwzWifi compat: OpenBSD <sys/socket.h> (kernel subset needed by net80211).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_SOCKET_H_
#define _QWZMAC_SYS_SOCKET_H_

#include <sys/types.h>

#define AF_UNSPEC	0
#define AF_LOCAL	1
#define AF_UNIX		AF_LOCAL
#define AF_INET		2
#define AF_LINK		18
#define AF_INET6	24
#define AF_MAX		36
#define PF_UNSPEC	AF_UNSPEC
#define PF_INET		AF_INET
#define PF_INET6	AF_INET6

struct sockaddr {
	uint8_t		sa_len;
	sa_family_t	sa_family;
	char		sa_data[14];
};

struct sockaddr_storage {
	uint8_t		ss_len;
	sa_family_t	ss_family;
	unsigned char	__ss_pad1[6];
	uint64_t	__ss_pad2;
	unsigned char	__ss_pad3[240];
};

#endif /* _QWZMAC_SYS_SOCKET_H_ */
