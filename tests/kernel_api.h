/* Probes at the fake system API boundary; no driver state-machine copies. */
#ifndef TEST_KERNEL_API_H
#define TEST_KERNEL_API_H
unsigned test_timer_allocations(void);
unsigned test_timer_live(void);
unsigned test_iokit_live(void);
int test_timer_active(void);
void test_timer_pause_entry(int);
void test_timer_reset_waiting(void);
int test_timer_waiting(void);
void test_kernel_start(void);
void test_kernel_stop(void);
#endif
