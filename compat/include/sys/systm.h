/*
 * QwzWifi compat: OpenBSD <sys/systm.h>.
 * String/memory helpers bind to the macOS kernel's own exports (libkern KPI).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_SYSTM_H_
#define _QWZMAC_SYS_SYSTM_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <qwzmac/os.h>
#include <machine/intr.h>

/* macOS kernel exports (com.apple.kpi.libkern / bsd). */
void	*memcpy(void *, const void *, size_t);
void	*memmove(void *, const void *, size_t);
void	*memset(void *, int, size_t);
int	 memcmp(const void *, const void *, size_t);
void	 bcopy(const void *, void *, size_t);
void	 bzero(void *, size_t);
size_t	 strlen(const char *);
int	 strcmp(const char *, const char *);
int	 strncmp(const char *, const char *, size_t);
size_t	 strlcpy(char *, const char *, size_t);
size_t	 strlcat(char *, const char *, size_t);
int	 snprintf(char *, size_t, const char *, ...)
	    __attribute__((__format__(__printf__, 3, 4)));
int	 vsnprintf(char *, size_t, const char *, va_list);

/* Our own small helpers (compat/systm.c). */
size_t	 strnlen(const char *, size_t);
void	 explicit_bzero(void *, size_t);
int	 timingsafe_bcmp(const void *, const void *, size_t);

#define bcmp(a, b, n)	memcmp((a), (b), (n))

/* printf(9) goes to IOLog. */
int	 qwzmac_printf(const char *, ...)
	    __attribute__((__format__(__printf__, 1, 2)));
#define printf(...)	qwzmac_printf(__VA_ARGS__)
#define vprintf(f, ap)	qwzmac_vlog((f), (ap))
#define addlog(...)	qwzmac_printf(__VA_ARGS__)
#define log(lvl, ...)	qwzmac_printf(__VA_ARGS__)
#define LOG_ERR		3
#define LOG_WARNING	4
#define LOG_INFO	6
#define LOG_DEBUG	7

/*
 * panic(9) and KASSERT(9): log loudly instead of crashing the machine. A
 * panic during boot with -qwzon would mean a boot loop for the user.
 */
void	 qwzmac_panic(const char *file, int line, const char *fmt, ...)
	    __attribute__((__format__(__printf__, 3, 4)));
#define panic(...)	qwzmac_panic(__FILE__, __LINE__, __VA_ARGS__)
#define KASSERT(e)	do {						\
	if (__predict_false(!(e)))					\
		qwzmac_panic(__FILE__, __LINE__, "KASSERT %s", #e);	\
} while (0)
#define KASSERTMSG(e, ...) KASSERT(e)
#define KDASSERT(e)	do { } while (0)
#define CTASSERT(x)	_Static_assert((x), #x)

/* Sleeping and waking (tsleep(9)). */
/* hz/ticks are derived from the uptime clock; nothing needs a hardclock. */
#define hz		100
#define tick		(1000000 / hz)
#define ticks		((int)(qwzmac_nsecuptime() / (1000000000ULL / hz)))
#define INFSLP		UINT64_MAX
#define MAXTSLP		(UINT64_MAX - 1)
#define PSWP		0
#define PVM		4
#define PINOD		8
#define PRIBIO		16
#define PVFS		20
#define PZERO		22
#define PSOCK		24
#define PWAIT		32
#define PLOCK		36
#define PPAUSE		40
#define PUSER		50
#define PCATCH		0x100
#define PNORELOCK	0x200

int	 tsleep(const volatile void *, int, const char *, int);
int	 tsleep_nsec(const volatile void *, int, const char *, uint64_t);
void	 wakeup(const volatile void *);
void	 wakeup_n(const volatile void *, int);
#define wakeup_one(c)	wakeup_n((c), 1)

void	 delay(int);
#define DELAY(x)	delay(x)

/* Misc. */
void	 arc4random_buf(void *, size_t);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t);

#define splassert(x)	do { } while (0)
#define splsoftassert(x) do { } while (0)

#define KERNEL_LOCK()		do { } while (0)
#define KERNEL_UNLOCK()		do { } while (0)
#define KERNEL_ASSERT_LOCKED()	do { } while (0)
#define NET_LOCK()		do { } while (0)
#define NET_UNLOCK()		do { } while (0)
#define NET_ASSERT_LOCKED()	do { } while (0)

extern int cold;

/* QWZMAC: log the strings of a firmware RAM dump (compat/qwzmac_debug.c). */
void	 qwzmac_rddm_report(const uint8_t *, size_t);

#endif /* _QWZMAC_SYS_SYSTM_H_ */
