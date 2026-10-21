/*
 * QwzWifi compat: OpenBSD mutex(9), backed by the global recursive spl lock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_MUTEX_H_
#define _QWZMAC_SYS_MUTEX_H_

#include <qwzmac/os.h>

struct mutex {
	int	mtx_wantipl;
};

#define MUTEX_INITIALIZER(ipl)		{ (ipl) }
#define mtx_init(m, ipl)		((m)->mtx_wantipl = (ipl))
#define mtx_init_flags(m, ipl, n, f)	mtx_init((m), (ipl))
#define mtx_enter(m)			qwzmac_spl_lock()
#define mtx_leave(m)			qwzmac_spl_unlock()
#define mtx_enter_try(m)		(qwzmac_spl_lock(), 1)
#define MUTEX_ASSERT_LOCKED(m)		do { } while (0)
#define MUTEX_ASSERT_UNLOCKED(m)	do { } while (0)

#endif /* _QWZMAC_SYS_MUTEX_H_ */
