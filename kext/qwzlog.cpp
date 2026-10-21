/*
 * QwzWifi: driver log that survives early boot.
 *
 * IOLog output from this kext does not reach the unified log (`log show`)
 * during early boot on this machine, so every message is also kept in a
 * buffer and published as the "QwzLog" string property of the PCI device
 * node (pci17cb,1107). That node outlives a failed start(), so the log of a
 * failed attach can still be read. Use scripts/qwzlog.sh (ioreg).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSString.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "../compat/include/qwzmac/os.h"
#include "os_bridge.h"

#define QWZLOG_SIZE	(512 * 1024)
#define QWZLOG_HEAD	(64 * 1024)	/* never dropped: attach messages */
#define QWZLOG_LINE	512

static char		gBuf[QWZLOG_SIZE];
static size_t		gLen;
static bool		gAtLineStart = true;
static bool		gDirty;
static char		gPend[QWZLOG_LINE];	/* IOLog line being assembled */
static size_t		gPendLen;
static unsigned long	gDropped;
static IOLock * volatile gLock;
static IOService	*gOwner;
static thread_call_t	gPublishTc;

static IOLock *
log_lock(void)
{
	IOLock *l = gLock;

	if (l == nullptr) {
		IOLock *n = IOLockAlloc();

		if (n == nullptr)
			return nullptr;
		if (!__sync_bool_compare_and_swap(&gLock, (IOLock *)nullptr, n))
			IOLockFree(n);
		l = gLock;
	}
	return l;
}

/* Called with the log lock held. */
static void
log_append(const char *s, size_t n)
{
	static const char mark[] = "\n...[older messages dropped]...\n";

	if (n >= QWZLOG_SIZE - QWZLOG_HEAD - sizeof(mark) - 1)
		return;
	if (gLen + n + 1 > QWZLOG_SIZE) {
		/* Drop a quarter of the tail part, keep the head. */
		size_t drop = (QWZLOG_SIZE - QWZLOG_HEAD) / 4;
		size_t from = QWZLOG_HEAD + drop;

		memmove(gBuf + QWZLOG_HEAD + sizeof(mark) - 1, gBuf + from,
		    gLen - from);
		memcpy(gBuf + QWZLOG_HEAD, mark, sizeof(mark) - 1);
		gLen = gLen - from + QWZLOG_HEAD + sizeof(mark) - 1;
		gDropped++;
	}
	memcpy(gBuf + gLen, s, n);
	gLen += n;
	gBuf[gLen] = '\0';
	gDirty = true;
}

void
qwzmac_vlog(const char *fmt, va_list ap)
{
	char line[QWZLOG_LINE], stamp[32];
	uint64_t abs, ns;
	IOLock *l;
	int n;

	n = vsnprintf(line, sizeof(line), fmt, ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = sizeof(line) - 1;

	l = log_lock();
	if (l == nullptr) {
		IOLog("%s", line);
		return;
	}
	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	snprintf(stamp, sizeof(stamp), "[%5llu.%03llu] ",
	    (unsigned long long)(ns / 1000000000ULL),
	    (unsigned long long)(ns / 1000000ULL % 1000));

	IOLockLock(l);
	/* Timestamp every line, including continuation fragments. */
	for (char *p = line; *p != '\0';) {
		char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);

		if (gAtLineStart)
			log_append(stamp, strlen(stamp));
		log_append(p, len);
		gAtLineStart = (nl != nullptr);

		/*
		 * IOLog (and the -v console) gets whole lines only: qwz prints
		 * hex dumps one byte per printf, and one IOLog per byte is slow.
		 */
		if (gPendLen + len >= sizeof(gPend)) {
			IOLog("%s\n", gPend);
			gPendLen = 0;
		}
		memcpy(gPend + gPendLen, p, len);
		gPendLen += len;
		gPend[gPendLen] = '\0';
		if (nl != nullptr) {
			IOLog("%s", gPend);
			gPendLen = 0;
		}
		p += len;
	}
	IOLockUnlock(l);
}

void
qwzmac_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	qwzmac_vlog(fmt, ap);
	va_end(ap);
}

static void
log_publish(void)
{
	IOLock *l = log_lock();
	OSString *str = nullptr;
	IOService *owner;

	if (l == nullptr)
		return;
	IOLockLock(l);
	owner = gOwner;
	if (owner != nullptr && gDirty) {
		str = OSString::withCString(gBuf);
		gDirty = false;
	}
	IOLockUnlock(l);
	if (str != nullptr) {
		owner->setProperty("QwzLog", str);
		str->release();
	}
}

static void
log_publish_tc(thread_call_param_t p0, thread_call_param_t p1)
{
	uint64_t deadline;

	(void)p0; (void)p1;
	log_publish();
	clock_interval_to_deadline(1, kSecondScale, &deadline);
	thread_call_enter_delayed(gPublishTc, deadline);
}

void
qwzmac_log_start(IOService *owner)
{
	uint64_t deadline;

	IOLock *l = log_lock();

	owner->retain();
	if (l != nullptr) {
		IOLockLock(l);
		gOwner = owner;
		gDirty = true;
		IOLockUnlock(l);
	}
	log_publish();
	gPublishTc = thread_call_allocate(log_publish_tc, nullptr);
	if (gPublishTc != nullptr) {
		clock_interval_to_deadline(1, kSecondScale, &deadline);
		thread_call_enter_delayed(gPublishTc, deadline);
	}
}

void
qwzmac_log_publish(void)
{
	log_publish();
}

void
qwzmac_log_stop(void)
{
	IOLock *l = log_lock();

	if (gPublishTc != nullptr) {
		/* The callback re-arms itself; cancel until it is quiet. */
		qwzmac_thread_call_stop(gPublishTc);
		thread_call_free(gPublishTc);
		gPublishTc = nullptr;
	}
	log_publish();
	if (l != nullptr && gOwner != nullptr) {
		IOService *owner;

		IOLockLock(l);
		owner = gOwner;
		gOwner = nullptr;
		IOLockUnlock(l);
		owner->release();
	}
}
