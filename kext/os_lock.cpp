/* QwzWifi: macOS synchronization bridge. SPDX-License-Identifier: GPL-2.0-only */
#include <IOKit/IOLocks.h>
#include <libkern/OSDebug.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include "../compat/include/qwzmac/os.h"
#include "os_bridge.h"
#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

static IORecursiveLock *gSpl;

bool qwzmac_spl_init(void)
{
	gSpl = IORecursiveLockAlloc();
	return gSpl != nullptr;
}

void qwzmac_spl_fini(void)
{
	if (gSpl != nullptr)
		IORecursiveLockFree(gSpl);
	gSpl = nullptr;
}

/* spl lock, sleep, wakeup. */

void
qwzmac_spl_lock(void)
{
	IORecursiveLockLock(gSpl);
}

void
qwzmac_spl_unlock(void)
{
	IORecursiveLockUnlock(gSpl);
}

int
qwzmac_spl_held(void)
{
	return IORecursiveLockHaveLock(gSpl) ? 1 : 0;
}

int
qwzmac_sleep(const void *ident, uint64_t nsecs)
{
	int res;

	if (!IORecursiveLockHaveLock(gSpl)) {
		QLOG("qwzmac_sleep without the spl lock");
		OSReportWithBacktrace("QwzWifi: bad sleep");
		return EINVAL;
	}
	if (nsecs == 0) {
		res = IORecursiveLockSleep(gSpl, (void *)ident, THREAD_UNINT);
	} else {
		uint64_t abs, deadline;

		nanoseconds_to_absolutetime(nsecs, &abs);
		clock_get_uptime(&deadline);
		deadline += abs;
		res = IORecursiveLockSleepDeadline(gSpl, (void *)ident,
		    deadline, THREAD_UNINT);
	}
	return res == THREAD_TIMED_OUT ? EWOULDBLOCK : 0;
}

void
qwzmac_wakeup(const void *ident, int one)
{
	IORecursiveLockWakeup(gSpl, (void *)ident, one != 0);
}
