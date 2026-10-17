/*
 * QwzWifi compat: OpenBSD task(9), real signatures.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_TASK_H_
#define _QWZMAC_SYS_TASK_H_

#include <sys/queue.h>

struct taskq;

struct task {
	TAILQ_ENTRY(task) t_entry;
	void		(*t_func)(void *);
	void		*t_arg;
	unsigned int	t_flags;
};

#define TASK_ONQUEUE		1

TAILQ_HEAD(task_list, task);

#define TASKQ_MPSAFE		(1 << 0)

#define TASK_INITIALIZER(_f, _a)  {{ NULL, NULL }, (_f), (_a), 0 }

extern struct taskq *const systq;
extern struct taskq *const systqmp;

struct taskq	*taskq_create(const char *, unsigned int, int, unsigned int);
void		 taskq_destroy(struct taskq *);
void		 taskq_barrier(struct taskq *);
void		 taskq_del_barrier(struct taskq *, struct task *);

void		 task_set(struct task *, void (*)(void *), void *);
int		 task_add(struct taskq *, struct task *);
int		 task_del(struct taskq *, struct task *);

#define task_pending(_t)	((_t)->t_flags & TASK_ONQUEUE)

/* QWZMAC: start/stop the shared system queues (called from the kext). */
int		 qwzmac_taskq_init(void);
void		 qwzmac_taskq_fini(void);

#endif /* _QWZMAC_SYS_TASK_H_ */
