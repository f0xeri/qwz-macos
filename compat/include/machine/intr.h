/*
 * QwzWifi compat: OpenBSD spl(9). Every raised level is the same global
 * recursive lock (see qwzmac/os.h); splx() releases one level.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_MACHINE_INTR_H_
#define _QWZMAC_MACHINE_INTR_H_

#include <qwzmac/os.h>

#define IPL_NONE	0
#define IPL_SOFTCLOCK	1
#define IPL_SOFTNET	2
#define IPL_BIO		3
#define IPL_NET		4
#define IPL_SOFTTTY	5
#define IPL_TTY		6
#define IPL_VM		7
#define IPL_AUDIO	8
#define IPL_CLOCK	9
#define IPL_STATCLOCK	10
#define IPL_SCHED	11
#define IPL_HIGH	12
#define IPL_IPI		13
#define IPL_MPSAFE	0x100
#define IPL_MPFLOOR	IPL_TTY

static inline int qwzmac_splraise(void) { qwzmac_spl_lock(); return 1; }
static inline void splx(int s) { if (s) qwzmac_spl_unlock(); }

#define splnet()	qwzmac_splraise()
#define splsoftnet()	qwzmac_splraise()
#define splbio()	qwzmac_splraise()
#define spltty()	qwzmac_splraise()
#define splvm()		qwzmac_splraise()
#define splclock()	qwzmac_splraise()
#define splhigh()	qwzmac_splraise()
#define splsched()	qwzmac_splraise()
#define splsoftclock()	qwzmac_splraise()
#define spl0()		0

#endif /* _QWZMAC_MACHINE_INTR_H_ */
