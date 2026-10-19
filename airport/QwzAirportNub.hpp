/*
 * Asynchronous readiness nub between QwzWifi and the native controller.
 * Unlike IOResourceMatch, waiting here never holds an IOKit matching thread.
 * Lifecycle pattern: laobamac's IOPCIEDeviceWrapper; no PCI or HAL access.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_AIRPORT_NUB_HPP
#define QWZ_AIRPORT_NUB_HPP
#include <IOKit/IOService.h>
#include <kern/thread_call.h>

class IO80211WorkQueue;

class QwzAirportNub : public IOService {
	OSDeclareDefaultStructors(QwzAirportNub)
public:
	bool init(OSDictionary *) override;
	bool start(IOService *) override;
	IOWorkLoop *getWorkLoop() const override;
	void stop(IOService *) override;
	void free() override;
	using IOService::callPlatformFunction;
	IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) override;
private:
	static void readyTc(thread_call_param_t, thread_call_param_t);
	void cancelReady();
	void status(const char *text);
	IOService *fQwz;
	IO80211WorkQueue *fWorkQueue;
	thread_call_t fReadyTc;
	bool fStopping;
	unsigned fAttempts;
};
#endif
