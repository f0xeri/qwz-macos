/*
 * QwzWifi: implementation of qwzmac/os.h on IOKit and Mach.
 * This is the only place where the OpenBSD side touches macOS.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSDebug.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/random.h>
#include <sys/errno.h>

#include "../compat/include/qwzmac/os.h"
#include "os_bridge.h"

#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

static IORecursiveLock	*gSpl;
static IOService	*gOwner;
static IOPCIDevice	*gPci;
static IOWorkLoop	*gWorkLoop;
static IOMemoryMap	*gBarMap[6];

struct qwzmac_timer;
static IOLock		*gTimerLock;	/* protects gTimers */
static qwzmac_timer	*gTimers;
static volatile int	 gTimersDead;
static volatile int	 gTimersRunning;

#define QWZMAC_MAX_INTR	4
struct qwzmac_intr {
	IOFilterInterruptEventSource *src;
	int	(*fn)(void *);
	void	*arg;
	int	index;
	char	name[24];
	uint64_t count;
	volatile bool dying;	/* disestablished, removal may be deferred */
};
static qwzmac_intr gIntr[QWZMAC_MAX_INTR];

/* Lifecycle (called from QwzWifi::start/stop). */

bool
qwzmac_os_init(IOService *owner, IOPCIDevice *pci)
{
	gSpl = IORecursiveLockAlloc();
	if (gSpl == nullptr)
		return false;
	gTimerLock = IOLockAlloc();
	gWorkLoop = IOWorkLoop::workLoop();
	if (gWorkLoop == nullptr || gTimerLock == nullptr) {
		if (gWorkLoop != nullptr)
			gWorkLoop->release();
		gWorkLoop = nullptr;
		if (gTimerLock != nullptr)
			IOLockFree(gTimerLock);
		gTimerLock = nullptr;
		IORecursiveLockFree(gSpl);
		gSpl = nullptr;
		return false;
	}
	gTimers = nullptr;
	gTimersDead = 0;
	gOwner = owner;
	gPci = pci;
	bzero(gIntr, sizeof(gIntr));
	bzero(gBarMap, sizeof(gBarMap));
	return true;
}

static void qwzmac_timers_fini(void);

void
qwzmac_os_fini(void)
{
	qwzmac_timers_fini();
	/* Also removes sources whose removal had to be deferred. */
	for (int i = 0; i < QWZMAC_MAX_INTR; i++) {
		qwzmac_intr *ih = &gIntr[i];

		if (ih->src == nullptr)
			continue;
		ih->dying = true;
		ih->src->disable();
		gWorkLoop->removeEventSource(ih->src);
		ih->src->release();
		ih->src = nullptr;
	}
	for (int i = 0; i < 6; i++) {
		if (gBarMap[i] != nullptr) {
			gBarMap[i]->release();
			gBarMap[i] = nullptr;
		}
	}
	if (gWorkLoop != nullptr) {
		gWorkLoop->release();
		gWorkLoop = nullptr;
	}
	if (gSpl != nullptr) {
		IORecursiveLockFree(gSpl);
		gSpl = nullptr;
	}
	gOwner = nullptr;
	gPci = nullptr;
}

/* Logging: qwzmac_vlog/qwzmac_log live in qwzlog.cpp. */

void
qwzmac_backtrace(void)
{
	OSReportWithBacktrace("QwzWifi: backtrace");
}

/* Heap. IOMalloc may block; nothing here runs in primary interrupt context. */

void *
qwzmac_malloc(size_t size, int canwait)
{
	(void)canwait;
	return IOMalloc(size);
}

void
qwzmac_free(void *p, size_t size)
{
	IOFree(p, size);
}

/* Time. */

uint64_t
qwzmac_nsecuptime(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

void
qwzmac_delay_us(unsigned int us)
{
	IODelay(us);
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

/* Threads. */

struct qwzmac_thread_arg {
	void	(*fn)(void *);
	void	*arg;
};

static void
qwzmac_thread_main(void *p, wait_result_t wr)
{
	qwzmac_thread_arg a = *(qwzmac_thread_arg *)p;

	(void)wr;
	IOFree(p, sizeof(qwzmac_thread_arg));
	a.fn(a.arg);
	thread_terminate(current_thread());
}

unsigned long
qwzmac_thread_self(void)
{
	return (unsigned long)current_thread();
}

int
qwzmac_thread_start(void (*fn)(void *), void *arg)
{
	qwzmac_thread_arg *a;
	thread_t thread;

	a = (qwzmac_thread_arg *)IOMalloc(sizeof(*a));
	if (a == nullptr)
		return ENOMEM;
	a->fn = fn;
	a->arg = arg;
	if (kernel_thread_start(qwzmac_thread_main, a, &thread) !=
	    KERN_SUCCESS) {
		IOFree(a, sizeof(*a));
		return ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

/* Timers. */

/*
 * thread_call_cancel_wait() is private KPI, so running callbacks are
 * counted here and waited for by polling.
 *
 * Every timer is also kept on a list: a timeout(9) lives inside a softc,
 * and nothing in qwz/net80211 frees its thread_call. qwzmac_timers_quiesce()
 * stops them all before the softc is freed, os_fini frees them.
 */
struct qwzmac_timer {
	thread_call_t	tc;
	void		(*fn)(void *);
	void		*arg;
	volatile int	running;
	qwzmac_timer	*next, **prevp;
};


static void
qwzmac_timer_main(thread_call_param_t p0, thread_call_param_t p1)
{
	qwzmac_timer *t = (qwzmac_timer *)p0;

	(void)p1;
	__atomic_add_fetch(&gTimersRunning, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&t->running, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&gTimersDead, __ATOMIC_SEQ_CST))
		t->fn(t->arg);
	__atomic_sub_fetch(&t->running, 1, __ATOMIC_SEQ_CST);
	__atomic_sub_fetch(&gTimersRunning, 1, __ATOMIC_SEQ_CST);
}

void
qwzmac_timers_quiesce(void)
{
	__atomic_store_n(&gTimersDead, 1, __ATOMIC_SEQ_CST);
	if (gTimerLock != nullptr) {
		IOLockLock(gTimerLock);
		for (qwzmac_timer *t = gTimers; t != nullptr; t = t->next)
			thread_call_cancel(t->tc);
		IOLockUnlock(gTimerLock);
	}
	while (__atomic_load_n(&gTimersRunning, __ATOMIC_SEQ_CST) > 0)
		IOSleep(1);
}

static void
qwzmac_timers_fini(void)
{
	qwzmac_timers_quiesce();
	if (gTimerLock == nullptr)
		return;
	for (;;) {
		IOLockLock(gTimerLock);
		qwzmac_timer *t = gTimers;
		IOLockUnlock(gTimerLock);
		if (t == nullptr)
			break;
		qwzmac_timer_free(t);
	}
	IOLockFree(gTimerLock);
	gTimerLock = nullptr;
}

void *
qwzmac_timer_alloc(void (*fn)(void *), void *arg)
{
	qwzmac_timer *t;

	t = (qwzmac_timer *)IOMalloc(sizeof(*t));
	if (t == nullptr)
		return nullptr;
	t->fn = fn;
	t->arg = arg;
	t->running = 0;
	t->tc = thread_call_allocate(qwzmac_timer_main, t);
	if (t->tc == nullptr) {
		IOFree(t, sizeof(*t));
		return nullptr;
	}
	IOLockLock(gTimerLock);
	t->next = gTimers;
	if (gTimers != nullptr)
		gTimers->prevp = &t->next;
	t->prevp = &gTimers;
	gTimers = t;
	IOLockUnlock(gTimerLock);
	return t;
}

void
qwzmac_timer_free(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	if (t == nullptr)
		return;
	qwzmac_timer_cancel_wait(t);
	IOLockLock(gTimerLock);
	*t->prevp = t->next;
	if (t->next != nullptr)
		t->next->prevp = t->prevp;
	IOLockUnlock(gTimerLock);
	if (!thread_call_free(t->tc)) {
		QLOG("timer still pending at free, leaking it");
		return;
	}
	IOFree(t, sizeof(*t));
}

int
qwzmac_timer_enter(void *p, uint64_t nsecs)
{
	qwzmac_timer *t = (qwzmac_timer *)p;
	uint64_t abs, deadline;

	nanoseconds_to_absolutetime(nsecs, &abs);
	clock_get_uptime(&deadline);
	deadline += abs;
	return thread_call_enter_delayed(t->tc, deadline) ? 1 : 0;
}

int
qwzmac_timer_cancel(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	return thread_call_cancel(t->tc) ? 1 : 0;
}

int
qwzmac_timer_cancel_wait(void *p)
{
	qwzmac_timer *t = (qwzmac_timer *)p;

	/*
	 * A running handler may be waiting for the spl lock; waiting for it
	 * while holding that lock would deadlock. Only cancel in that case.
	 */
	int cancelled;

	cancelled = thread_call_cancel(t->tc) ? 1 : 0;
	if (IORecursiveLockHaveLock(gSpl))
		return cancelled;
	while (__atomic_load_n(&t->running, __ATOMIC_SEQ_CST) > 0)
		IOSleep(1);
	return cancelled;
}

/* DMA memory. */

int
qwzmac_dma_alloc(size_t size, size_t align, uint64_t lo, uint64_t hi,
    int canwait, void **cookie, void **va, uint64_t *pa)
{
	IOBufferMemoryDescriptor *md;
	mach_vm_address_t mask;
	IOByteCount seglen, off;
	addr64_t base, seg;

	(void)canwait;
	if (align < PAGE_SIZE || (align & (align - 1)) != 0 || lo != 0 ||
	    hi < size)
		return EINVAL;

	/* Physical mask: the address range and the alignment in one. */
	mask = (mach_vm_address_t)hi & ~((mach_vm_address_t)align - 1);
	md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, mask);
	if (md == nullptr)
		return ENOMEM;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return ENOMEM;
	}

	base = md->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
	/* Walk the whole buffer: it must really be one physical run. */
	for (off = 0; off < size; off += seglen) {
		seg = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
		if (seg == 0 || seglen == 0 || seg != base + off) {
			QLOG("DMA buffer not contiguous at +0x%llx "
			    "(0x%llx != 0x%llx)", (unsigned long long)off,
			    (unsigned long long)seg,
			    (unsigned long long)(base + off));
			md->complete();
			md->release();
			return ENOMEM;
		}
	}
	if (base < lo || base + size - 1 > hi || (base & (align - 1)) != 0) {
		QLOG("DMA buffer 0x%llx+0x%zx outside mask 0x%llx",
		    (unsigned long long)base, size, (unsigned long long)mask);
		md->complete();
		md->release();
		return ENOMEM;
	}

	*cookie = md;
	*va = md->getBytesNoCopy();
	*pa = base;
	return 0;
}

void
qwzmac_dma_free(void *cookie)
{
	IOBufferMemoryDescriptor *md = (IOBufferMemoryDescriptor *)cookie;

	if (md == nullptr)
		return;
	md->complete();
	md->release();
}

/* PCI. */

uint32_t
qwzmac_pci_conf_read(void *dev, int reg)
{
	IOPCIDevice *pci = (IOPCIDevice *)dev;

	return pci->extendedConfigRead32((IOByteCount)reg);
}

void
qwzmac_pci_conf_write(void *dev, int reg, uint32_t val)
{
	IOPCIDevice *pci = (IOPCIDevice *)dev;

	pci->extendedConfigWrite32((IOByteCount)reg, val);
}

int
qwzmac_pci_map_bar(void *dev, int reg, uintptr_t *va, uint64_t *pa,
    uint64_t *size)
{
	IOPCIDevice *pci = (IOPCIDevice *)dev;
	IOMemoryMap *map;
	int idx = (reg - kIOPCIConfigBaseAddress0) / 4;

	if (idx < 0 || idx >= 6)
		return EINVAL;
	if (gBarMap[idx] == nullptr) {
		pci->setMemoryEnable(true);
		map = pci->mapDeviceMemoryWithRegister((UInt8)reg,
		    kIOMapInhibitCache);
		if (map == nullptr) {
			QLOG("cannot map BAR at 0x%x", reg);
			return ENXIO;
		}
		gBarMap[idx] = map;
	}
	map = gBarMap[idx];
	*va = (uintptr_t)map->getVirtualAddress();
	*pa = map->getPhysicalAddress();
	*size = map->getLength();
	QLOG("BAR 0x%x: phys 0x%llx size 0x%llx va 0x%lx", reg,
	    (unsigned long long)*pa, (unsigned long long)*size,
	    (unsigned long)*va);
	return 0;
}

void
qwzmac_pci_unmap_bar(void *dev, int reg)
{
	int idx = (reg - kIOPCIConfigBaseAddress0) / 4;

	(void)dev;
	if (idx >= 0 && idx < 6 && gBarMap[idx] != nullptr) {
		gBarMap[idx]->release();
		gBarMap[idx] = nullptr;
	}
}

int
qwzmac_pci_msi_index(void *dev)
{
	IOPCIDevice *pci = (IOPCIDevice *)dev;
	int type;

	for (int i = 0; i < 16; i++) {
		if (pci->getInterruptType(i, &type) != kIOReturnSuccess)
			break;
		if (type & kIOInterruptTypePCIMessaged)
			return i;
	}
	QLOG("no MSI interrupt source");
	return -1;
}

static bool
qwzmac_intr_filter(OSObject *owner, IOFilterInterruptEventSource *src)
{
	(void)owner; (void)src;
	return true;	/* MSI: nothing to acknowledge at this level */
}

static void
qwzmac_intr_action(OSObject *owner, IOInterruptEventSource *src, int count)
{
	(void)owner;
	for (int i = 0; i < QWZMAC_MAX_INTR; i++) {
		qwzmac_intr *ih = &gIntr[i];

		if (ih->src != src)
			continue;
		ih->count++;
		if (ih->count <= 3 || (ih->count % 10000) == 0)
			QLOG("interrupt %s #%llu (%d coalesced)", ih->name,
			    (unsigned long long)ih->count, count);
		IORecursiveLockLock(gSpl);
		if (!ih->dying)
			ih->fn(ih->arg);
		IORecursiveLockUnlock(gSpl);
		return;
	}
}

void *
qwzmac_pci_intr_establish(void *dev, int index, int (*fn)(void *),
    void *arg, const char *name)
{
	IOPCIDevice *pci = (IOPCIDevice *)dev;
	qwzmac_intr *ih = nullptr;

	for (int i = 0; i < QWZMAC_MAX_INTR; i++) {
		if (gIntr[i].src == nullptr) {
			ih = &gIntr[i];
			break;
		}
	}
	if (ih == nullptr)
		return nullptr;

	ih->fn = fn;
	ih->arg = arg;
	ih->index = index;
	ih->count = 0;
	ih->dying = false;
	strlcpy(ih->name, name ? name : "?", sizeof(ih->name));
	ih->src = IOFilterInterruptEventSource::filterInterruptEventSource(
	    gOwner, qwzmac_intr_action, qwzmac_intr_filter, pci, index);
	if (ih->src == nullptr) {
		QLOG("cannot create interrupt source %d", index);
		return nullptr;
	}
	if (gWorkLoop->addEventSource(ih->src) != kIOReturnSuccess) {
		QLOG("cannot add interrupt source %d", index);
		ih->src->release();
		ih->src = nullptr;
		return nullptr;
	}
	ih->src->enable();
	QLOG("interrupt %s established on index %d", ih->name, index);

	/* What IOPCIFamily programmed; qwz reads the same registers. */
	IOByteCount msi = 0;
	if (pci->extendedFindPCICapability(kIOPCIMSICapability, &msi) && msi) {
		UInt16 ctl = pci->configRead16(msi + 2);
		bool is64 = (ctl & 0x80) != 0;
		UInt32 lo = pci->configRead32(msi + 4);
		UInt32 hi = is64 ? pci->configRead32(msi + 8) : 0;
		UInt16 data = pci->configRead16(msi + (is64 ? 12 : 8));
		QLOG("MSI ctl 0x%04x (enabled=%d, %d of %d vectors) addr 0x%08x%08x"
		    " data 0x%04x", ctl, ctl & 1, 1 << ((ctl >> 4) & 7),
		    1 << ((ctl >> 1) & 7), hi, lo, data);
	}
	return ih;
}

void
qwzmac_pci_intr_disestablish(void *dev, void *cookie)
{
	qwzmac_intr *ih = (qwzmac_intr *)cookie;

	(void)dev;
	if (ih == nullptr || ih->src == nullptr || ih->dying)
		return;
	ih->dying = true;
	ih->src->disable();
	/*
	 * removeEventSource() waits for a running action, and the action may
	 * be waiting for the spl lock we hold: leave removal to os_fini.
	 */
	if (IORecursiveLockHaveLock(gSpl)) {
		QLOG("interrupt %s disabled; removal deferred", ih->name);
		return;
	}
	gWorkLoop->removeEventSource(ih->src);
	ih->src->release();
	ih->src = nullptr;
	QLOG("interrupt %s removed after %llu interrupts", ih->name,
	    (unsigned long long)ih->count);
}

/* Embedded firmware (kext/firmware.S). */

extern "C" {
extern const uint8_t qwz_fw_amss[], qwz_fw_amss_end[];
extern const uint8_t qwz_fw_m3[], qwz_fw_m3_end[];
extern const uint8_t qwz_fw_board2[], qwz_fw_board2_end[];
}

static const struct {
	const char	*name;
	const uint8_t	*start;
	const uint8_t	*end;
} gFirmware[] = {
	{ "qwz-wcn7850-hw2.0-amss",	qwz_fw_amss,	qwz_fw_amss_end },
	{ "qwz-wcn7850-hw2.0-m3",	qwz_fw_m3,	qwz_fw_m3_end },
	{ "qwz-wcn7850-hw2.0-board-2",	qwz_fw_board2,	qwz_fw_board2_end },
};

int
qwzmac_firmware_get(const char *name, const uint8_t **data, size_t *len)
{
	for (size_t i = 0; i < sizeof(gFirmware) / sizeof(gFirmware[0]); i++) {
		if (strcmp(name, gFirmware[i].name) == 0) {
			*data = gFirmware[i].start;
			*len = (size_t)(gFirmware[i].end - gFirmware[i].start);
			return 0;
		}
	}
	return ENOENT;
}

/* Randomness. */

void
qwzmac_random_buf(void *buf, size_t len)
{
	read_random(buf, (u_int)len);
}
