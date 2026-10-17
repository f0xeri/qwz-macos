/*
 * QwzWifi compat: rwlock(9) and refcnt(9) on the global spl lock plus
 * sleep/wakeup. These locks are logical (they may be held across sleeps);
 * the spl lock itself is only held while their state is examined.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/rwlock.h>
#include <sys/refcnt.h>

/* Any non-zero per-thread value works as the writer identity. */
static unsigned long
curthread_id(void)
{
	return qwzmac_thread_self();
}

void
rw_init(struct rwlock *rwl, const char *name)
{
	rwl->rwl_owner = 0;
	rwl->rwl_readers = 0;
	rwl->rwl_waiters = 0;
	rwl->rwl_name = name;
}

int
rw_enter(struct rwlock *rwl, int flags)
{
	unsigned long self = curthread_id();
	int op = flags & RW_OPMASK;
	int error = 0;

	qwzmac_spl_lock();
	if (op == RW_WRITE) {
		if (rwl->rwl_owner == self) {
			qwzmac_spl_unlock();
			if (flags & RW_RECURSEFAIL)
				return EDEADLK;
			panic("rw_enter: %s: recursive write lock",
			    rwl->rwl_name);
			return EDEADLK;
		}
		while (rwl->rwl_owner != 0 || rwl->rwl_readers != 0) {
			if (flags & RW_NOSLEEP) {
				error = EBUSY;
				goto out;
			}
			rwl->rwl_waiters++;
			qwzmac_sleep(rwl, 0);
			rwl->rwl_waiters--;
		}
		rwl->rwl_owner = self;
	} else if (op == RW_READ) {
		while (rwl->rwl_owner != 0) {
			if (flags & RW_NOSLEEP) {
				error = EBUSY;
				goto out;
			}
			rwl->rwl_waiters++;
			qwzmac_sleep(rwl, 0);
			rwl->rwl_waiters--;
		}
		rwl->rwl_readers++;
	} else if (op == RW_DOWNGRADE) {
		if (rwl->rwl_owner == self) {
			rwl->rwl_owner = 0;
			rwl->rwl_readers++;
			if (rwl->rwl_waiters)
				qwzmac_wakeup(rwl, 0);
		}
	}
out:
	qwzmac_spl_unlock();
	return error;
}

void
rw_enter_write(struct rwlock *rwl)
{
	rw_enter(rwl, RW_WRITE);
}

void
rw_enter_read(struct rwlock *rwl)
{
	rw_enter(rwl, RW_READ);
}

void
rw_exit(struct rwlock *rwl)
{
	qwzmac_spl_lock();
	if (rwl->rwl_owner != 0) {
		if (rwl->rwl_owner != curthread_id())
			printf("QwzWifi: rw_exit %s: not owner\n",
			    rwl->rwl_name);
		rwl->rwl_owner = 0;
	} else if (rwl->rwl_readers > 0) {
		rwl->rwl_readers--;
	} else {
		printf("QwzWifi: rw_exit %s: not locked\n", rwl->rwl_name);
	}
	if (rwl->rwl_waiters)
		qwzmac_wakeup(rwl, 0);
	qwzmac_spl_unlock();
}

void
rw_exit_write(struct rwlock *rwl)
{
	rw_exit(rwl);
}

void
rw_exit_read(struct rwlock *rwl)
{
	rw_exit(rwl);
}

int
rw_status(struct rwlock *rwl)
{
	if (rwl->rwl_owner != 0)
		return rwl->rwl_owner == curthread_id() ? RW_WRITE : 0x0008;
	if (rwl->rwl_readers > 0)
		return RW_READ;
	return 0;
}

void
rw_assert_wrlock(struct rwlock *rwl)
{
	if (rwl->rwl_owner != curthread_id())
		panic("%s: lock not held by this thread", rwl->rwl_name);
}

void
rw_assert_rdlock(struct rwlock *rwl)
{
	if (rwl->rwl_readers == 0)
		panic("%s: lock not shared", rwl->rwl_name);
}

void
rw_assert_anylock(struct rwlock *rwl)
{
	if (rwl->rwl_owner == 0 && rwl->rwl_readers == 0)
		panic("%s: lock not held", rwl->rwl_name);
}

void
rw_assert_unlocked(struct rwlock *rwl)
{
	if (rwl->rwl_owner == curthread_id())
		panic("%s: lock held", rwl->rwl_name);
}

/* refcnt(9) */
void
refcnt_init(struct refcnt *r)
{
	__atomic_store_n(&r->r_refs, 1, __ATOMIC_SEQ_CST);
	r->r_traceidx = 0;
}

void
refcnt_take(struct refcnt *r)
{
	unsigned int refs;

	refs = __atomic_add_fetch(&r->r_refs, 1, __ATOMIC_SEQ_CST);
	KASSERT(refs != 0);
}

int
refcnt_rele(struct refcnt *r)
{
	unsigned int refs;

	refs = __atomic_sub_fetch(&r->r_refs, 1, __ATOMIC_SEQ_CST);
	KASSERT(refs != ~0U);
	return refs == 0;
}

void
refcnt_rele_wake(struct refcnt *r)
{
	if (refcnt_rele(r))
		wakeup_one(r);
}

void
refcnt_finalize(struct refcnt *r, const char *wmesg)
{
	unsigned int refs;

	qwzmac_spl_lock();
	refs = __atomic_sub_fetch(&r->r_refs, 1, __ATOMIC_SEQ_CST);
	while (refs != 0) {
		qwzmac_sleep(r, 0);
		refs = __atomic_load_n(&r->r_refs, __ATOMIC_SEQ_CST);
	}
	qwzmac_spl_unlock();
	(void)wmesg;
}

unsigned int
refcnt_read(const struct refcnt *r)
{
	return __atomic_load_n(&r->r_refs, __ATOMIC_SEQ_CST);
}
