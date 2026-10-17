/*
 * QwzWifi compat: OpenBSD <sys/atomic.h> on clang builtins, plus the one
 * Linux-ism (atomic_t) that qwzvar.h still carries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_ATOMIC_H_
#define _QWZMAC_SYS_ATOMIC_H_

#include <sys/types.h>

#define atomic_inc_int(p)	((void)__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define atomic_dec_int(p)	((void)__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define atomic_inc_int_nv(p)	__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define atomic_dec_int_nv(p)	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define atomic_add_int(p, v)	((void)__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST))
#define atomic_sub_int(p, v)	((void)__atomic_sub_fetch((p), (v), __ATOMIC_SEQ_CST))
#define atomic_add_int_nv(p, v)	__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define atomic_sub_int_nv(p, v)	__atomic_sub_fetch((p), (v), __ATOMIC_SEQ_CST)
#define atomic_setbits_int(p, v) ((void)__atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST))
#define atomic_clearbits_int(p, v) ((void)__atomic_and_fetch((p), ~(v), __ATOMIC_SEQ_CST))
#define atomic_load_int(p)	__atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_store_int(p, v)	__atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_cas_uint(p, o, n) __sync_val_compare_and_swap((p), (o), (n))
#define atomic_swap_uint(p, v)	__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

#define membar_enter()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define membar_exit()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define membar_producer()	__atomic_thread_fence(__ATOMIC_RELEASE)
#define membar_consumer()	__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define membar_sync()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define membar_enter_after_atomic()	membar_sync()
#define membar_exit_before_atomic()	membar_sync()

typedef struct {
	volatile int counter;
} atomic_t;

#define atomic_read(v)		__atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)
#define atomic_set(v, i)	__atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc(v)		((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec(v)		((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec_and_test(v)	(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST) == 0)

#endif /* _QWZMAC_SYS_ATOMIC_H_ */
