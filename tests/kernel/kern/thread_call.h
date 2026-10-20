#pragma once
#include <stdint.h>
using thread_call_param_t = void *;
using thread_call_func_t = void (*)(void *, void *);
struct thread_call;
using thread_call_t = thread_call *;
enum { THREAD_CALL_PRIORITY_KERNEL = 0, THREAD_CALL_OPTIONS_ONCE = 1 };
thread_call_t thread_call_allocate(thread_call_func_t, void *);
thread_call_t thread_call_allocate_with_options(thread_call_func_t, void *, int, unsigned);
bool thread_call_enter_delayed(thread_call_t, uint64_t);
bool thread_call_cancel(thread_call_t);
bool thread_call_free(thread_call_t);
bool thread_call_isactive(thread_call_t);
