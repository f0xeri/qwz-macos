/*
 * QwzWifi compat: OpenBSD malloc(9).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_MALLOC_H_
#define _QWZMAC_SYS_MALLOC_H_

#include <sys/types.h>

#define M_WAITOK	0x0001
#define M_NOWAIT	0x0002
#define M_CANFAIL	0x0004
#define M_ZERO		0x0008

#define M_FREE		0
#define M_DEVBUF	2
#define M_PCB		4
#define M_RTABLE	5
#define M_IFADDR	9
#define M_SYSCTL	11
#define M_COUNTERS	12
#define M_IOCTLOPS	14
#define M_TEMP		127
#define M_80211_NODE	89
#define M_80211_CRYPTO	90
#define M_80211_RSN	91

/*
 * The block size is kept in a small header, so free(9) callers that pass
 * 0 as "unknown size" are handled correctly.
 */
void	*malloc(size_t, int, int);
void	*mallocarray(size_t, size_t, int, int);
void	 free(void *, int, size_t);

#endif /* _QWZMAC_SYS_MALLOC_H_ */
