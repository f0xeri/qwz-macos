/*
 * QwzWifi compat: OpenBSD <sys/param.h> (kernel subset).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_PARAM_H_
#define _QWZMAC_SYS_PARAM_H_

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/atomic.h>

#define NBBY		8
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1 << PAGE_SHIFT)
#define PAGE_MASK	(PAGE_SIZE - 1)
#define PATH_MAX	1024
#define MAXPATHLEN	PATH_MAX
#define MAXCOMLEN	16
#define CACHELINESIZE	64

#define setbit(a, i)	(((unsigned char *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)	(((unsigned char *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)	(((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)	((((const unsigned char *)(a))[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)

#define SET(t, f)	((t) |= (f))
#define CLR(t, f)	((t) &= ~(f))
#define ISSET(t, f)	((t) & (f))

#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y)	(((x) / (y)) * (y))
#define powerof2(x)	((((x) - 1) & (x)) == 0)
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#define ALIGNBYTES	(sizeof(long) - 1)
#define ALIGN(p)	(((unsigned long)(p) + ALIGNBYTES) & ~ALIGNBYTES)

static inline u_int min(u_int a, u_int b) { return a < b ? a : b; }
static inline u_int max(u_int a, u_int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline u_long ulmin(u_long a, u_long b) { return a < b ? a : b; }
static inline u_long ulmax(u_long a, u_long b) { return a > b ? a : b; }
static inline int ffs(int v) { return __builtin_ffs(v); }
static inline int fls(int v) { return v ? 32 - __builtin_clz((unsigned)v) : 0; }
static inline int flsl(long v) { return v ? 64 - __builtin_clzl((unsigned long)v) : 0; }

#endif /* _QWZMAC_SYS_PARAM_H_ */
