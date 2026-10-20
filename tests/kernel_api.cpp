/* Host substitutes for system APIs only. No qwzmac_timer_* implementation.
 * The production os_timer.cpp and os_lock.cpp are linked into the tests.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../kext/os_bridge.h"

extern "C" {
static unsigned allocations, live_calls, live_backings;
static int pause_entry, waiting;
unsigned test_timer_allocations() { return __atomic_load_n(&allocations, __ATOMIC_SEQ_CST); }
unsigned test_timer_live() { return __atomic_load_n(&live_calls, __ATOMIC_SEQ_CST); }
unsigned test_iokit_live() { return __atomic_load_n(&live_backings, __ATOMIC_SEQ_CST); }
void test_timer_pause_entry(int value) { __atomic_store_n(&pause_entry, value, __ATOMIC_SEQ_CST); }
void test_timer_reset_waiting() { __atomic_store_n(&waiting, 0, __ATOMIC_SEQ_CST); }
int test_timer_waiting() { return __atomic_load_n(&waiting, __ATOMIC_SEQ_CST); }
static int dispatched;
int test_timer_active() { return __atomic_load_n(&dispatched, __ATOMIC_SEQ_CST); }
void test_kernel_start()
{
	alarm(20); /* fail instead of hanging forever on a timer regression */
	assert(qwzmac_spl_init());
	assert(qwzmac_timers_init());
}
void test_kernel_stop()
{
	qwzmac_timers_fini();
	qwzmac_spl_fini();
}
}

static uint64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
static timespec realtime_after(uint64_t ns)
{
	timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t end = uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec + ns;
	return { time_t(end / 1000000000), long(end % 1000000000) };
}
void clock_get_uptime(uint64_t *p) { *p = now_ns(); }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *p) { *p = ns; }
thread_t current_thread() { return pthread_self(); }
void IOSleep(unsigned ms) { usleep(ms * 1000); }
void OSReportWithBacktrace(const char *, ...) { abort(); }
void *IOMalloc(size_t n)
{
	void *p = malloc(n);
	if (p) __atomic_add_fetch(&live_backings, 1, __ATOMIC_SEQ_CST);
	return p;
}
void IOFree(void *p, size_t)
{
	__atomic_sub_fetch(&live_backings, 1, __ATOMIC_SEQ_CST);
	free(p);
}

struct IOLock { pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; };
IOLock *IOLockAlloc() { return new IOLock; }
void IOLockFree(IOLock *l) { pthread_mutex_destroy(&l->m); delete l; }
void IOLockLock(IOLock *l) { pthread_mutex_lock(&l->m); }
void IOLockUnlock(IOLock *l) { pthread_mutex_unlock(&l->m); }

struct waiter { void *event; bool woken; waiter *next; };
struct IORecursiveLock {
	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	waiter *waiters = nullptr;
};
static thread_local IORecursiveLock *held;
static thread_local unsigned depth;
IORecursiveLock *IORecursiveLockAlloc() { return new IORecursiveLock; }
void IORecursiveLockFree(IORecursiveLock *l)
{
	assert(!l->waiters && held != l);
	pthread_cond_destroy(&l->cv);
	pthread_mutex_destroy(&l->m);
	delete l;
}
void IORecursiveLockLock(IORecursiveLock *l)
{
	if (held == l) { depth++; return; }
	assert(!held);
	pthread_mutex_lock(&l->m);
	held = l; depth = 1;
}
void IORecursiveLockUnlock(IORecursiveLock *l)
{
	assert(held == l && depth);
	if (--depth) return;
	held = nullptr;
	pthread_mutex_unlock(&l->m);
}
bool IORecursiveLockHaveLock(IORecursiveLock *l) { return held == l; }
int IORecursiveLockSleepDeadline(IORecursiveLock *l, void *event,
    uint64_t deadline, uint32_t)
{
	assert(held == l);
	waiter w = { event, false, l->waiters };
	l->waiters = &w;
	unsigned saved = depth;
	held = nullptr; depth = 0;
	int e = 0;
	while (!w.woken && e != ETIMEDOUT) {
		if (deadline) {
			uint64_t now = now_ns();
			if (now >= deadline) { e = ETIMEDOUT; break; }
			timespec ts = realtime_after(deadline - now);
			e = pthread_cond_timedwait(&l->cv, &l->m, &ts);
		} else e = pthread_cond_wait(&l->cv, &l->m);
	}
	for (waiter **pp = &l->waiters; *pp; pp = &(*pp)->next)
		if (*pp == &w) { *pp = w.next; break; }
	held = l; depth = saved;
	return e == ETIMEDOUT ? THREAD_TIMED_OUT : 0;
}
int IORecursiveLockSleep(IORecursiveLock *l, void *e, uint32_t type)
{ return IORecursiveLockSleepDeadline(l, e, 0, type); }
void IORecursiveLockWakeup(IORecursiveLock *l, void *event, bool one)
{
	assert(held == l);
	for (waiter *w = l->waiters; w; w = w->next)
		if (w->event == event && !w->woken) {
			w->woken = true;
			if (one) break;
		}
	pthread_cond_broadcast(&l->cv);
}

/* The system work item has one worker. ONCE permits re-enqueue, not overlap.
 * A free from its callback releases the work item after that callback returns.
 */
struct thread_call {
	thread_call_func_t fn;
	void *arg;
	pthread_t worker;
	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	uint64_t deadline = 0;
	bool pending = false, running = false, stop = false, self_free = false;
};
static void release(thread_call_t t)
{
	pthread_cond_destroy(&t->cv);
	pthread_mutex_destroy(&t->m);
	__atomic_sub_fetch(&live_calls, 1, __ATOMIC_SEQ_CST);
	delete t;
}
static void *work(void *p)
{
	auto t = static_cast<thread_call_t>(p);
	pthread_mutex_lock(&t->m);
	while (!t->stop) {
		if (!t->pending) { pthread_cond_wait(&t->cv, &t->m); continue; }
		uint64_t now = now_ns();
		if (now < t->deadline) {
			timespec ts = realtime_after(t->deadline - now);
			pthread_cond_timedwait(&t->cv, &t->m, &ts);
			continue;
		}
		t->pending = false; t->running = true;
		__atomic_add_fetch(&dispatched, 1, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock(&t->m);
		while (__atomic_load_n(&pause_entry, __ATOMIC_SEQ_CST))
			usleep(1000);
		t->fn(t->arg, nullptr);
		pthread_mutex_lock(&t->m);
		t->running = false;
		__atomic_sub_fetch(&dispatched, 1, __ATOMIC_SEQ_CST);
	}
	bool self_free = t->self_free;
	pthread_mutex_unlock(&t->m);
	if (self_free) { pthread_detach(pthread_self()); release(t); }
	return nullptr;
}
thread_call_t thread_call_allocate_with_options(thread_call_func_t fn,
    void *arg, int, unsigned options)
{
	assert(options == THREAD_CALL_OPTIONS_ONCE);
	auto t = new thread_call;
	t->fn = fn; t->arg = arg;
	__atomic_add_fetch(&allocations, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&live_calls, 1, __ATOMIC_SEQ_CST);
	assert(pthread_create(&t->worker, nullptr, work, t) == 0);
	return t;
}
thread_call_t thread_call_allocate(thread_call_func_t fn, void *arg)
{ return thread_call_allocate_with_options(fn, arg, 0, THREAD_CALL_OPTIONS_ONCE); }
bool thread_call_enter_delayed(thread_call_t t, uint64_t deadline)
{
	pthread_mutex_lock(&t->m);
	bool was = t->pending;
	t->pending = true; t->deadline = deadline;
	pthread_cond_signal(&t->cv);
	pthread_mutex_unlock(&t->m);
	return was;
}
bool thread_call_cancel(thread_call_t t)
{
	pthread_mutex_lock(&t->m);
	bool was = t->pending;
	t->pending = false;
	pthread_cond_signal(&t->cv);
	pthread_mutex_unlock(&t->m);
	return was;
}
bool thread_call_isactive(thread_call_t t)
{
	pthread_mutex_lock(&t->m);
	bool active = t->pending || t->running;
	if (active && t->worker != pthread_self())
		__atomic_store_n(&waiting, 1, __ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&t->m);
	return active;
}
bool thread_call_free(thread_call_t t)
{
	pthread_mutex_lock(&t->m);
	bool self = t->worker == pthread_self();
	if (t->pending || (t->running && !self)) {
		pthread_mutex_unlock(&t->m);
		return false;
	}
	t->stop = true; t->self_free = self;
	pthread_cond_signal(&t->cv);
	pthread_mutex_unlock(&t->m);
	if (!self) { pthread_join(t->worker, nullptr); release(t); }
	return true;
}
