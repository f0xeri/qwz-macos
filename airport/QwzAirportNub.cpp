/* SPDX-License-Identifier: GPL-2.0-only */
#include "QwzAirportNub.hpp"
#include "QwzAirport.hpp"
#include <kern/clock.h>
#include <libkern/sysctl.h>

#define super IOService
OSDefineMetaClassAndStructors(QwzAirportNub, IOService);

bool QwzAirportNub::init(OSDictionary *properties)
{
	if (!super::init(properties))
		return false;
	fQwz = nullptr;
	fWorkQueue = nullptr;
	fReadyTc = nullptr;
	fStopping = false;
	fAttempts = 0;
	return true;
}

void QwzAirportNub::status(const char *text)
{
	setProperty("QwzAirportReadiness", text);
	if (fQwz != nullptr)
		fQwz->setProperty("QwzAirportReadiness", text);
	ALOG("readiness: %s", text);
}

// No native controller will be published: QwzWifi falls back to Ethernet.
void QwzAirportNub::unavailable(IOService *qwz, const char *why)
{
	ALOG("native front unavailable: %s", why);
	qwz->callPlatformFunction(QWZ_AIRPORT_UNAVAILABLE, false, (void *)why,
	    nullptr, nullptr, nullptr);
}

bool QwzAirportNub::start(IOService *provider)
{
	if (!super::start(provider)) {
		unavailable(provider, "nub did not start");
		return false;
	}
	fQwz = provider;
	fWorkQueue = IO80211WorkQueue::workQueue();
	if (fWorkQueue == nullptr) {
		super::stop(provider);
		unavailable(provider, "no IO80211 work queue");
		return false;
	}
	// ONCE prevents overlap if registerService causes further work to run.
	fReadyTc = thread_call_allocate_with_options(readyTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (fReadyTc == nullptr) {
		OSSafeReleaseNULL(fWorkQueue);
		super::stop(provider);
		unavailable(provider, "nub did not start");
		return false;
	}
	status("waiting asynchronously for kern.osversion");
	uint64_t deadline;
	clock_interval_to_deadline(1, kSecondScale, &deadline);
	thread_call_enter_delayed(fReadyTc, deadline);
	// Do not registerService yet: the IO80211Controller must not match here.
	return true;
}

void QwzAirportNub::readyTc(thread_call_param_t p0, thread_call_param_t)
{
	auto self = static_cast<QwzAirportNub *>(p0);
	if (__atomic_load_n(&self->fStopping, __ATOMIC_SEQ_CST))
		return;
	char build[32] = "";
	size_t len = sizeof(build);
	int error = sysctlbyname("kern.osversion", build, &len, nullptr, 0);
	++self->fAttempts;
	if (error != 0 || build[0] == '\0') {
		if (self->fAttempts >= 120) {
			self->status("build unavailable after 120 polls; native controller not published");
			unavailable(self->fQwz, "macOS build unknown");
			return;
		}
		uint64_t deadline;
		clock_interval_to_deadline(1, kSecondScale, &deadline);
		if (!__atomic_load_n(&self->fStopping, __ATOMIC_SEQ_CST))
			thread_call_enter_delayed(self->fReadyTc, deadline);
		return;
	}
	self->setProperty("QwzAirportDetectedBuild", build);
	self->fQwz->setProperty("QwzAirportDetectedBuild", build);
	auto builds = OSDynamicCast(OSArray, self->getProperty("QwzAirportBuilds"));
	bool allowed = false;
	for (unsigned i = 0; builds != nullptr && i < builds->getCount(); ++i) {
		auto b = OSDynamicCast(OSString, builds->getObject(i));
		if (b != nullptr && b->isEqualTo(build))
			allowed = true;
	}
	if (!allowed) {
		self->status("build not allowlisted; native controller not published");
		unavailable(self->fQwz, "macOS build not allowlisted");
		return;
	}
	if (__atomic_load_n(&self->fStopping, __ATOMIC_SEQ_CST))
		return;
	self->status("build allowed; publishing native controller provider");
	self->registerService(kIOServiceAsynchronous);
}

IOReturn QwzAirportNub::callPlatformFunction(const OSSymbol *name, bool wait,
    void *p1, void *p2, void *p3, void *p4)
{
	if (name != nullptr && (name->isEqualTo(QWZ_AIRPORT_CONNECT) ||
	    name->isEqualTo(QWZ_AIRPORT_DISCONNECT) ||
	    name->isEqualTo(QWZ_AIRPORT_UNAVAILABLE))) {
		if (fQwz == nullptr)
			return kIOReturnNotReady;
		return fQwz->callPlatformFunction(name, wait, p1, p2, p3, p4);
	}
	return super::callPlatformFunction(name, wait, p1, p2, p3, p4);
}

void QwzAirportNub::cancelReady()
{
	__atomic_store_n(&fStopping, true, __ATOMIC_SEQ_CST);
	if (fReadyTc != nullptr) {
		while (thread_call_isactive(fReadyTc)) {
			thread_call_cancel(fReadyTc);
			IOSleep(5);
		}
		thread_call_free(fReadyTc);
		fReadyTc = nullptr;
	}
}

void QwzAirportNub::stop(IOService *provider)
{
	cancelReady();
	super::stop(provider);
	fQwz = nullptr;
}

IOWorkLoop *QwzAirportNub::getWorkLoop() const
{
	return fWorkQueue;
}

void QwzAirportNub::free()
{
	cancelReady();
	OSSafeReleaseNULL(fWorkQueue);
	super::free();
}
