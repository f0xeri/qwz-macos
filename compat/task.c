/*
 * QwzWifi compat: task(9)/taskq(9) and timeout(9).
 *
 * A taskq is one kernel thread draining a TAILQ under the spl lock; the
 * task function runs with the lock dropped, as OpenBSD runs tasks in
 * process context. A timeout is a thread_call; ordinary handlers run
 * under spl, while TIMEOUT_PROC handlers run with spl dropped.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/task.h>
#include <sys/timeout.h>

struct taskq {
	const char		*tq_name;
	struct task_list	 tq_worklist;
	int			 tq_running;	/* thread alive */
	int			 tq_dying;
	unsigned int		 tq_gen;	/* bumped after each task */
	struct task		*tq_current;
};

static struct taskq qwzmac_systq;
static struct taskq qwzmac_systqmp;
struct taskq *const systq = &qwzmac_systq;
struct taskq *const systqmp = &qwzmac_systqmp;

static void
taskq_thread(void *arg)
{
	struct taskq *tq = arg;
	struct task *t;
	void (*fn)(void *);
	void *fnarg;

	qwzmac_spl_lock();
	tq->tq_running = 1;
	qwzmac_wakeup(&tq->tq_running, 0);
	for (;;) {
		while ((t = TAILQ_FIRST(&tq->tq_worklist)) == NULL &&
		    !tq->tq_dying)
			qwzmac_sleep(tq, 0);
		if (t == NULL)
			break;
		TAILQ_REMOVE(&tq->tq_worklist, t, t_entry);
		t->t_flags &= ~TASK_ONQUEUE;
		fn = t->t_func;
		fnarg = t->t_arg;
		tq->tq_current = t;
		qwzmac_spl_unlock();

		fn(fnarg);

		qwzmac_spl_lock();
		tq->tq_current = NULL;
		tq->tq_gen++;
		qwzmac_wakeup(&tq->tq_gen, 0);
	}
	tq->tq_running = 0;
	qwzmac_wakeup(&tq->tq_running, 0);
	qwzmac_spl_unlock();
}

static int
taskq_start(struct taskq *tq, const char *name)
{
	tq->tq_name = name;
	TAILQ_INIT(&tq->tq_worklist);
	tq->tq_running = 0;
	tq->tq_dying = 0;
	tq->tq_gen = 0;
	tq->tq_current = NULL;
	if (qwzmac_thread_start(taskq_thread, tq) != 0)
		return ENOMEM;
	qwzmac_spl_lock();
	while (!tq->tq_running)
		qwzmac_sleep(&tq->tq_running, 0);
	qwzmac_spl_unlock();
	return 0;
}

static void
taskq_stop(struct taskq *tq)
{
	qwzmac_spl_lock();
	if (tq->tq_running) {
		tq->tq_dying = 1;
		qwzmac_wakeup(tq, 0);
		while (tq->tq_running)
			qwzmac_sleep(&tq->tq_running, 0);
	}
	qwzmac_spl_unlock();
}

int
qwzmac_taskq_init(void)
{
	int error;

	error = taskq_start(&qwzmac_systq, "systq");
	if (error)
		return error;
	error = taskq_start(&qwzmac_systqmp, "systqmp");
	if (error)
		taskq_stop(&qwzmac_systq);
	return error;
}

void
qwzmac_taskq_fini(void)
{
	taskq_stop(&qwzmac_systqmp);
	taskq_stop(&qwzmac_systq);
}

struct taskq *
taskq_create(const char *name, unsigned int nthreads, int ipl,
    unsigned int flags)
{
	struct taskq *tq;

	(void)nthreads; (void)ipl; (void)flags;	/* always one thread */
	tq = malloc(sizeof(*tq), M_DEVBUF, M_WAITOK | M_CANFAIL | M_ZERO);
	if (tq == NULL)
		return NULL;
	if (taskq_start(tq, name) != 0) {
		free(tq, M_DEVBUF, sizeof(*tq));
		return NULL;
	}
	return tq;
}

void
taskq_destroy(struct taskq *tq)
{
	taskq_stop(tq);
	free(tq, M_DEVBUF, sizeof(*tq));
}

static void
taskq_barrier_task(void *arg)
{
	volatile int *done = arg;

	qwzmac_spl_lock();
	*done = 1;
	qwzmac_wakeup((const void *)done, 0);
	qwzmac_spl_unlock();
}

/* Wait until everything queued before this call has run. */
void
taskq_barrier(struct taskq *tq)
{
	volatile int done = 0;
	struct task t;

	task_set(&t, taskq_barrier_task, (void *)&done);
	qwzmac_spl_lock();
	if (task_add(tq, &t)) {
		while (!done && tq->tq_running)
			qwzmac_sleep((const void *)&done, 0);
	}
	qwzmac_spl_unlock();
}

void
taskq_del_barrier(struct taskq *tq, struct task *t)
{
	task_del(tq, t);
	qwzmac_spl_lock();
	while (tq->tq_current == t && tq->tq_running)
		qwzmac_sleep(&tq->tq_gen, 0);
	qwzmac_spl_unlock();
}

void
task_set(struct task *t, void (*fn)(void *), void *arg)
{
	t->t_func = fn;
	t->t_arg = arg;
	t->t_flags = 0;
}

int
task_add(struct taskq *tq, struct task *w)
{
	int rv = 0;

	qwzmac_spl_lock();
	if (!ISSET(w->t_flags, TASK_ONQUEUE)) {
		rv = 1;
		SET(w->t_flags, TASK_ONQUEUE);
		TAILQ_INSERT_TAIL(&tq->tq_worklist, w, t_entry);
		qwzmac_wakeup(tq, 1);
	}
	qwzmac_spl_unlock();
	return rv;
}

int
task_del(struct taskq *tq, struct task *w)
{
	int rv = 0;

	qwzmac_spl_lock();
	if (ISSET(w->t_flags, TASK_ONQUEUE)) {
		rv = 1;
		CLR(w->t_flags, TASK_ONQUEUE);
		TAILQ_REMOVE(&tq->tq_worklist, w, t_entry);
	}
	qwzmac_spl_unlock();
	return rv;
}

/* timeout(9) */
static void
timeout_trampoline(void *arg)
{
	struct timeout *to = arg;
	void (*fn)(void *);
	void *fnarg;
	int process;

	/* The backend enters with spl held: serialize ordinary callbacks/free. */
	if (!(to->to_flags & TIMEOUT_ONQUEUE)) {
		/* Deleted after the thread_call already fired. */
		return;
	}
	to->to_flags &= ~TIMEOUT_ONQUEUE;
	to->to_flags |= TIMEOUT_TRIGGERED;
	fn = to->to_func;
	fnarg = to->to_arg;
	process = to->to_flags & TIMEOUT_PROC;
	if (process)
		qwzmac_spl_unlock();
	fn(fnarg);
	/* The callback may have destroyed its owner; use only local copies. */
	if (process)
		qwzmac_spl_lock();
}

void
timeout_set_flags(struct timeout *to, void (*fn)(void *), void *arg,
    int kclock, int flags)
{
	to->to_func = fn;
	to->to_arg = arg;
	to->to_kclock = kclock;
	to->to_flags = flags | TIMEOUT_INITIALIZED;
}

void
timeout_set(struct timeout *to, void (*fn)(void *), void *arg)
{
	timeout_set_flags(to, fn, arg, KCLOCK_NONE, 0);
}

void
timeout_set_proc(struct timeout *to, void (*fn)(void *), void *arg)
{
	timeout_set_flags(to, fn, arg, KCLOCK_NONE, TIMEOUT_PROC);
}

int
timeout_add_nsec(struct timeout *to, uint64_t nsecs)
{
	int ret;
	void *tc;

	/* QWZMAC: backing belongs to this address, never to copied fields. */
	qwzmac_spl_lock();
	if (!(to->to_flags & TIMEOUT_INITIALIZED)) {
		qwzmac_spl_unlock();
		panic("timeout_add: not initialized");
		return 0;
	}
	tc = qwzmac_timer_lookup(timeout_trampoline, to);
	if (tc == NULL) {
		tc = qwzmac_timer_alloc(timeout_trampoline, to);
		if (tc == NULL) {
			qwzmac_spl_unlock();
			printf("QwzWifi: timeout_add: no thread_call\n");
			return 0;
		}
	}

	ret = !(to->to_flags & TIMEOUT_ONQUEUE);
	to->to_flags |= TIMEOUT_ONQUEUE;
	to->to_flags &= ~TIMEOUT_TRIGGERED;
	qwzmac_timer_enter(tc, nsecs ? nsecs : 1);
	qwzmac_spl_unlock();
	return ret;
}

int
timeout_add(struct timeout *to, int to_ticks)
{
	if (to_ticks < 1)
		to_ticks = 1;
	return timeout_add_nsec(to, (uint64_t)to_ticks * (1000000000ULL / hz));
}

int
timeout_add_sec(struct timeout *to, int secs)
{
	return timeout_add_nsec(to, SEC_TO_NSEC(secs > 0 ? secs : 0));
}

int
timeout_add_msec(struct timeout *to, uint64_t msecs)
{
	return timeout_add_nsec(to, MSEC_TO_NSEC(msecs));
}

int
timeout_add_usec(struct timeout *to, uint64_t usecs)
{
	return timeout_add_nsec(to, USEC_TO_NSEC(usecs));
}

int
timeout_del(struct timeout *to)
{
	int ret;
	void *tc;

	qwzmac_spl_lock();
	ret = (to->to_flags & TIMEOUT_ONQUEUE) != 0;
	to->to_flags &= ~(TIMEOUT_ONQUEUE | TIMEOUT_TRIGGERED);
	tc = qwzmac_timer_lookup(timeout_trampoline, to);
	if (tc != NULL)
		qwzmac_timer_cancel(tc);
	qwzmac_spl_unlock();
	return ret;
}

int
timeout_del_barrier(struct timeout *to)
{
	int ret;

	ret = timeout_del(to);
	timeout_barrier(to);
	return ret;
}

void
timeout_barrier(struct timeout *to)
{
	void *tc;

	qwzmac_spl_lock();
	tc = qwzmac_timer_lookup(timeout_trampoline, to);
	if (tc != NULL)
		qwzmac_timer_cancel_wait(tc);
	qwzmac_spl_unlock();
}

void
timeout_destroy(struct timeout *to)
{
	void *tc;

	qwzmac_spl_lock();
	timeout_del_barrier(to);
	tc = qwzmac_timer_lookup(timeout_trampoline, to);
	if (tc != NULL)
		qwzmac_timer_free(tc);
	qwzmac_spl_unlock();
}
