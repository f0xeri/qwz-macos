/*
 * QwzWifi compat: OpenBSD rwlock(9), real signatures. Implemented on top of
 * the global spl lock with sleep/wakeup (compat/sync.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_RWLOCK_H_
#define _QWZMAC_SYS_RWLOCK_H_

#include <sys/types.h>

struct rwlock {
	volatile unsigned long	rwl_owner;	/* QWZMAC: writer thread or 0 */
	volatile unsigned int	rwl_readers;
	volatile unsigned int	rwl_waiters;
	const char		*rwl_name;
};

#define RWLOCK_INITIALIZER(name)	{ 0, 0, 0, name }

#define RW_WRITE		0x0001UL
#define RW_READ			0x0002UL
#define RW_DOWNGRADE		0x0004UL
#define RW_OPMASK		0x0007UL
#define RW_INTR			0x0010UL
#define RW_SLEEPFAIL		0x0020UL
#define RW_NOSLEEP		0x0040UL
#define RW_RECURSEFAIL		0x0080UL
#define RW_DUPOK		0x0100UL

#define RW_WRLOCK		1
#define RW_RDLOCK		2

void	rw_init(struct rwlock *, const char *);
#define rw_init_flags(rwl, name, flags)	rw_init((rwl), (name))
void	rw_enter_read(struct rwlock *);
void	rw_enter_write(struct rwlock *);
void	rw_exit_read(struct rwlock *);
void	rw_exit_write(struct rwlock *);
int	rw_enter(struct rwlock *, int);
void	rw_exit(struct rwlock *);
int	rw_status(struct rwlock *);
void	rw_assert_wrlock(struct rwlock *);
void	rw_assert_rdlock(struct rwlock *);
void	rw_assert_anylock(struct rwlock *);
void	rw_assert_unlocked(struct rwlock *);

#endif /* _QWZMAC_SYS_RWLOCK_H_ */
