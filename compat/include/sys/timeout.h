/*
 * QwzWifi compat: OpenBSD timeout(9), real signatures, backed by
 * thread_call. Handlers always run in a thread ("process context").
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_TIMEOUT_H_
#define _QWZMAC_SYS_TIMEOUT_H_

#include <sys/time.h>

struct timeout {
	void	(*to_func)(void *);
	void	*to_arg;
	void	*to_tc;			/* QWZMAC: thread_call, lazily made */
	int	to_flags;
	int	to_kclock;
};

#define TIMEOUT_PROC		0x01
#define TIMEOUT_ONQUEUE		0x02
#define TIMEOUT_INITIALIZED	0x04
#define TIMEOUT_TRIGGERED	0x08
#define TIMEOUT_MPSAFE		0x10

#define timeout_pending(to)	((to)->to_flags & TIMEOUT_ONQUEUE)
#define timeout_initialized(to)	((to)->to_flags & TIMEOUT_INITIALIZED)
#define timeout_triggered(to)	((to)->to_flags & TIMEOUT_TRIGGERED)

#define KCLOCK_NONE	(-1)
#define KCLOCK_UPTIME	0
#define KCLOCK_MAX	1

#define TIMEOUT_INITIALIZER_FLAGS(_fn, _arg, _kclock, _flags) {		\
	.to_func = (_fn),						\
	.to_arg = (_arg),						\
	.to_tc = NULL,							\
	.to_flags = (_flags) | TIMEOUT_INITIALIZED,			\
	.to_kclock = (_kclock)						\
}

#define TIMEOUT_INITIALIZER(_f, _a)					\
    TIMEOUT_INITIALIZER_FLAGS((_f), (_a), KCLOCK_NONE, 0)

void timeout_set(struct timeout *, void (*)(void *), void *);
void timeout_set_flags(struct timeout *, void (*)(void *), void *, int, int);
void timeout_set_proc(struct timeout *, void (*)(void *), void *);

int timeout_add(struct timeout *, int);
int timeout_add_sec(struct timeout *, int);
int timeout_add_msec(struct timeout *, uint64_t);
int timeout_add_usec(struct timeout *, uint64_t);
int timeout_add_nsec(struct timeout *, uint64_t);

int timeout_del(struct timeout *);
int timeout_del_barrier(struct timeout *);
void timeout_barrier(struct timeout *);

/* QWZMAC: release the thread_call; the timeout must not be pending. */
void timeout_destroy(struct timeout *);

#endif /* _QWZMAC_SYS_TIMEOUT_H_ */
