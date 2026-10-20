#pragma once
#include <pthread.h>
using thread_t = pthread_t;
#define THREAD_NULL nullptr
#define THREAD_UNINT 0
#define THREAD_TIMED_OUT 1
thread_t current_thread();
