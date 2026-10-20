/* QwzWifi: native timer backing. SPDX-License-Identifier: GPL-2.0-only */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include "../compat/include/qwzmac/os.h"
#include "os_bridge.h"
#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

struct qwzmac_timer;
static IOLock *gTimerLock;
static qwzmac_timer *gTimers;
static volatile int gTimersDead, gTimersRunning;

bool qwzmac_timers_init(void)
{
	gTimerLock = IOLockAlloc();
	gTimers = nullptr;
	gTimersDead = gTimersRunning = 0;
	return gTimerLock != nullptr;
}

/* Timers. */

/*
 * Use the approved native isactive to include the dispatch window before
 * wrapper entry. Ordinary timeout callbacks and owner cleanup share spl.
 *
 * The list owns lazy backing by timeout address; node cleanup releases it.
 * qwzmac_timers_quiesce() stops remaining softc timers before teardown,
 * os_fini frees them.
 */
struct qwzmac_timer {
	thread_call_t	tc;
	void		(*fn)(void *);
	void		*arg;
	thread_t	runner;		/* protected by spl */
	bool		retired;
	unsigned	waiters;	/* pins backing while spl is dropped */
	qwzmac_timer	*next, **prevp;
};


static void qwzmac_timer_release(qwzmac_timer *);

static void
qwzmac_timer_main(thread_call_param_t p0, thread_call_param_t p1)
{
	qwzmac_timer *t = (qwzmac_timer *)p0;

	(void)p1;
	__atomic_add_fetch(&gTimersRunning, 1, __ATOMIC_SEQ_CST);
	qwzmac_spl_lock();
	t->runner = current_thread();
	if (!t->retired && !__atomic_load_n(&gTimersDead, __ATOMIC_SEQ_CST))
		t->fn(t->arg);
	t->runner = THREAD_NULL;
	if (t->retired && t->waiters == 0)
		qwzmac_timer_release(t); /* free from this callback is legal */
	qwzmac_spl_unlock();
	__atomic_sub_fetch(&gTimersRunning, 1, __ATOMIC_SEQ_CST);
}

void
qwzmac_timers_quiesce(void)
{
	__atomic_store_n(&gTimersDead, 1, __ATOMIC_SEQ_CST);
	if (gTimerLock != nullptr) {
		IOLockLock(gTimerLock);
		for (qwzmac_timer *t = gTimers; t != nullptr; t = t->next)
			thread_call_cancel(t->tc);
		IOLockUnlock(gTimerLock);
	}
	while (__atomic_load_n(&gTimersRunning, __ATOMIC_SEQ_CST) > 0)
		IOSleep(1);
}

void
qwzmac_timers_fini(void)
{
	qwzmac_timers_quiesce();
	if (gTimerLock == nullptr)
		return;
	for (;;) {
		IOLockLock(gTimerLock);
		qwzmac_timer *t = gTimers;
		IOLockUnlock(gTimerLock);
		if (t == nullptr)
			break;
		qwzmac_timer_free(t);
	}
	IOLockFree(gTimerLock);
	gTimerLock = nullptr;
}

void *
qwzmac_timer_alloc(void (*fn)(void *), void *arg)
{
	qwzmac_timer *t;

	t = (qwzmac_timer *)IOMalloc(sizeof(*t));
	if (t == nullptr)
		return nullptr;
	t->fn = fn;
	t->arg = arg;
	t->runner = THREAD_NULL;
	t->retired = false;
	t->waiters = 0;
	t->tc = thread_call_allocate_with_options(qwzmac_timer_main, t,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (t->tc == nullptr) {
		IOFree(t, sizeof(*t));
		return nullptr;
	}
	IOLockLock(gTimerLock);
	t->next = gTimers;
	if (gTimers != nullptr)
		gTimers->prevp = &t->next;
	t->prevp = &gTimers;
	gTimers = t;
	IOLockUnlock(gTimerLock);
	return t;
}

void *
qwzmac_timer_lookup(void (*fn)(void *), void *arg)
{
	qwzmac_timer *t;

	IOLockLock(gTimerLock);
	for (t = gTimers; t != nullptr; t = t->next) {
		if (t->fn == fn && t->arg == arg)
			break;
	}
	IOLockUnlock(gTimerLock);
	return t;
}

/* Called under spl, after the body and all external waiters are finished. */
static void
qwzmac_timer_release(qwzmac_timer *t)
{
	if (!thread_call_free(t->tc)) {
		QLOG("timer still pending at free; keeping it registered");
		return;
	}
	IOLockLock(gTimerLock);
	*t->prevp = t->next;
	if (t->next != nullptr)
		t->next->prevp = t->prevp;
	IOLockUnlock(gTimerLock);
	IOFree(t, sizeof(*t));
}

void
qwzmac_timer_free(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	if (t == nullptr)
		return;
	qwzmac_spl_lock();
	t->retired = true;
	thread_call_cancel(t->tc);
	if (t->runner != current_thread()) {
		t->waiters++; /* keep backing alive through cancel_wait */
		qwzmac_timer_cancel_wait(t);
		if (--t->waiters == 0)
			qwzmac_timer_release(t);
	}
	/* Self-destruction is completed by the wrapper or its last waiter. */
	qwzmac_spl_unlock();
}

int
qwzmac_timer_enter(void *p, uint64_t nsecs)
{
	qwzmac_timer *t = (qwzmac_timer *)p;
	uint64_t abs, deadline;

	if (t->retired || __atomic_load_n(&gTimersDead, __ATOMIC_SEQ_CST))
		return 0;
	nanoseconds_to_absolutetime(nsecs, &abs);
	clock_get_uptime(&deadline);
	deadline += abs;
	return thread_call_enter_delayed(t->tc, deadline) ? 1 : 0;
}

int
qwzmac_timer_cancel(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	return thread_call_cancel(t->tc) ? 1 : 0;
}

int
qwzmac_timer_cancel_wait(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	int cancelled;

	qwzmac_spl_lock();
	t->waiters++;
	cancelled = thread_call_cancel(t->tc) ? 1 : 0;
	if (t->runner != current_thread()) {
		while (thread_call_isactive(t->tc)) {
			qwzmac_sleep(t, 1000000);
			thread_call_cancel(t->tc);
		}
	}
	if (--t->waiters == 0 && t->retired && t->runner == THREAD_NULL)
		qwzmac_timer_release(t);
	qwzmac_spl_unlock();
	return cancelled;
}
