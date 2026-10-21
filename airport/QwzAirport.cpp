/*
 * QwzAirport: the IO80211Controller (QwzAirport.hpp).
 *
 * Injected by OpenCore: it matches QwzAirportNub after asynchronous build
 * validation. The nub forwards callPlatformFunction() to the running QwzWifi
 * (kext/airport_api.h) and publishes a Wi-Fi interface. QwzWifi keeps its
 * Ethernet interface for HeliPort; native mode routes data through this front.
 *
 * Starts only on the macOS builds whose IO80211/Skywalk ABI was checked
 * with scripts/kc-abi.py --check (QwzAirportBuilds in Info.plist).
 *
 * Based on AirportItlwmV2 of laobamac/itlwm 738d4c6 (airport/CREDITS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "QwzAirport.hpp"
#include "QwzAirportInterface.hpp"
#include "QwzAirportEthernet.hpp"

#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <libkern/version.h>
#include <sys/sysctl.h>
#include <net/kpi_interface.h>

#define super IO80211Controller
OSDefineMetaClassAndStructors(QwzAirport, IO80211Controller);

bool
QwzAirport::init(OSDictionary *properties)
{
	ALOG("init: enter IO80211Controller::init");
	if (!super::init(properties)) {
		ALOG("init: IO80211Controller::init returned false");
		return false;
	}
	fQwz = nullptr;
	bzero(&fDrv, sizeof(fDrv));
	bzero(&fFront, sizeof(fFront));
	fConnected = false;
	fNativeData = false;
	fNativeLinkUp = 0;
	bzero(fMac, sizeof(fMac));
	fWorkQueue = nullptr;
	fGate = nullptr;
	fNetIf = nullptr;
	fBsdIf = nullptr;
	fPowerState = 0;
	fPowerWanted = 0;
	fPowerSerial = 0;
	fPowerTc = nullptr;
	fRadioTc = nullptr;
	fAsyncLock = nullptr;
	fLifecycleLock = nullptr;
	fLifecycle = 0;
	fRadioState = QWZMAC_RADIO_DOWN;
	fLogPipe = fDataPathPipe = fSnapshotsPipe = nullptr;
	fLogPipeStarted = fDataPathPipeStarted = fSnapshotsPipeStarted = false;
	fLogger = nullptr;
	fFaultStream = nullptr;
	fCoreFaultReporter = nullptr;
	fFaultReporter = nullptr;
	fAsyncLock = IOLockAlloc();
	if (!fAsyncLock) return false;
	fLifecycleLock = IOLockAlloc();
	if (fLifecycleLock == nullptr) {
		ALOG("init: IOLockAlloc failed");
		return false;
	}
	ALOG("init: complete");
	return true;
}

/*
 * The IO80211 ABI (slots and object sizes) is per macOS build. Refuse any
 * build not in QwzAirportBuilds, so a macOS update falls back to QwzWifi's
 * Ethernet interface instead of calling into moved slots.
 */
IOService *
QwzAirport::probe(IOService *provider, SInt32 *score)
{
	ALOG("probe: enter");
	char build[32] = "";
	size_t len = sizeof(build);
	OSArray *builds;
	bool ok = false;

	provider->setProperty("QwzAirportProbeResult", "reading kern.osversion");
	int error = sysctlbyname("kern.osversion", build, &len, nullptr, 0);
	if (error != 0)
		build[0] = '\0';
	builds = OSDynamicCast(OSArray, getProperty("QwzAirportBuilds"));
	ALOG("probe: kern.osversion='%s', errno=%d, length=%lu, allowlist=%u",
	    build, error, (unsigned long)len, builds ? builds->getCount() : 0);
	provider->setProperty("QwzAirportDetectedBuild", build);
	for (unsigned i = 0; builds != nullptr && i < builds->getCount(); i++) {
		OSString *b = OSDynamicCast(OSString, builds->getObject(i));
		if (b != nullptr && b->isEqualTo(build))
			ok = true;
	}
	if (!ok) {
		provider->setProperty("QwzAirportProbeResult", error ?
		    "kern.osversion failed" : "build not allowlisted");
		ALOG("macOS build %s is not in QwzAirportBuilds (its IO80211 ABI was "
		    "not checked with scripts/kc-abi.py --check): not starting", build);
		provider->callPlatformFunction(QWZ_AIRPORT_UNAVAILABLE, false,
		    (void *)"macOS build not allowlisted", nullptr, nullptr, nullptr);
		return nullptr;
	}
	IOService *result = super::probe(provider, score);
	provider->setProperty("QwzAirportProbeResult", result ? "accepted" : "super probe refused");
	ALOG("probe: %s", result ? "accepted" : "super probe refused");
	if (result == nullptr)
		provider->callPlatformFunction(QWZ_AIRPORT_UNAVAILABLE, false,
		    (void *)"native controller probe refused", nullptr, nullptr, nullptr);
	return result;
}

/* Also visible in ioreg if startup fails while the rest of macOS boots. */
void
QwzAirport::trace(const char *stage)
{
	setProperty("QwzAirportStage", stage);
	if (fQwz != nullptr)
		fQwz->setProperty("QwzAirportStage", stage);
	ALOG("stage: %s", stage);
}

/* Called without our gate held. No callback may use the interfaces after this. */
void
QwzAirport::stopAsync()
{
	if (fAsyncLock) IOLockLock(fAsyncLock);
	if (fNetIf != nullptr) {
		fNetIf->stopJoins();
		fNetIf->stopScans();
	}
	if (fLifecycleLock != nullptr)
		IOLockLock(fLifecycleLock);
	__atomic_fetch_or(&fLifecycle, (unsigned)Stopping, __ATOMIC_SEQ_CST);
	if (fLifecycleLock != nullptr)
		IOLockUnlock(fLifecycleLock);
	if (fConnected) {
		__atomic_store_n(&fNativeLinkUp, 0, __ATOMIC_SEQ_CST);
		if (fNativeData) (void)fDrv.leave(fDrv.dev);
		fQwz->callPlatformFunction(QWZ_AIRPORT_DISCONNECT, false,
		    fFront.cookie, nullptr, nullptr, nullptr);
		fConnected = false;
	}
	if (fRadioTc != nullptr) {
		qwz_airport_call_stop(fRadioTc);
		thread_call_free(fRadioTc);
		fRadioTc = nullptr;
	}
	if (fPowerTc != nullptr) {
		qwz_airport_call_stop(fPowerTc);
		thread_call_free(fPowerTc);
		fPowerTc = nullptr;
	}
	if (fAsyncLock) IOLockUnlock(fAsyncLock);
}

bool
QwzAirport::initCCLogs()
{
	CCPipeOptions o;

	/*
	 * IO80211Controller::start() fails without the driver's CoreCapture
	 * pipes and fault reporter. The values are laobamac's (taken from
	 * AppleBCMWLAN); the factories only initialize a pipe, startPipe()
	 * creates its event sources. CCPipe's vtable is not checked
	 * (kc-abi.py), so call it qualified, never virtually.
	 */
	bzero(&o, sizeof(o));
	o.pipe_type = 0;
	o.log_data_type = 1;
	o.pipe_size = 0x200000;
	o.min_log_size_notify = 0xccccc;
	o.notify_threshold = 1000;
	strlcpy(o.file_name, "QwzAirport_Logs", sizeof(o.file_name));
	snprintf(o.name, sizeof(o.name), "wlan%d", 0);
	strlcpy(o.directory_name, "WiFi", sizeof(o.directory_name));
	o.pad9 = 0x1000000;
	o.pad10 = 2;
	trace("CoreCapture log pipe factory");
	fLogPipe = CCPipe::withOwnerNameCapacity(this, "com.local.QwzAirport",
	    "DriverLogs", &o);
	if (fLogPipe == nullptr || getWorkLoop() == nullptr)
		return false;
	trace("CoreCapture log pipe start");
	fLogPipeStarted = fLogPipe->CCPipe::startPipe();
	if (!fLogPipeStarted)
		return false;
	CCStreamOptions so = {};
	CCStream *logger = CCStream::withPipeAndName(fLogPipe, "QwzAirport", &so);
	fLogger = OSDynamicCast(CCLogStream, logger);
	if (fLogger == nullptr) {
		OSSafeReleaseNULL(logger);
		return false;
	}

	bzero(&o, sizeof(o));
	o.pipe_type = 0;
	o.log_data_type = 0;
	o.pipe_size = 0x200000;
	o.min_log_size_notify = 0xccccc;
	o.notify_threshold = 1000;
	strlcpy(o.file_name, "QwzAirport_Datapath", sizeof(o.file_name));
	strlcpy(o.directory_name, "WiFi", sizeof(o.directory_name));
	o.pad9 = 0x2;
	o.pad10 = 0x2800000;
	fDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.local.QwzAirport",
	    "DatapathEvents", &o);
	if (fDataPathPipe == nullptr)
		return false;
	trace("CoreCapture data pipe start");
	fDataPathPipeStarted = fDataPathPipe->CCPipe::startPipe();
	if (!fDataPathPipeStarted)
		return false;

	bzero(&o, sizeof(o));
	o.pipe_type = 0x200000001;
	o.log_data_type = 2;
	strlcpy(o.file_name, "StateSnapshots", sizeof(o.file_name));
	strlcpy(o.name, "0", sizeof(o.name));
	strlcpy(o.directory_name, "WiFi", sizeof(o.directory_name));
	o.pipe_size = 128;
	fSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.local.QwzAirport",
	    "StateSnapshots", &o);
	if (fSnapshotsPipe == nullptr)
		return false;
	trace("CoreCapture snapshots pipe start");
	fSnapshotsPipeStarted = fSnapshotsPipe->CCPipe::startPipe();
	if (!fSnapshotsPipeStarted)
		return false;

	CCStreamOptions fo = {};
	fo.stream_type = 1;
	fo.console_level = 0xFFFFFFFFFFFFFFFF;
	fFaultStream = CCStream::withPipeAndName(fSnapshotsPipe, "FaultReporter", &fo);
	CCDataStream *ds = OSDynamicCast(CCDataStream, fFaultStream);
	if (ds == nullptr || fWorkQueue == nullptr)
		return false;
	trace("CoreCapture fault reporter");
	fCoreFaultReporter = CCFaultReporter::withStreamWorkloop(ds, fWorkQueue);
	if (fCoreFaultReporter == nullptr)
		return false;
	fFaultReporter = IO80211FaultReporter::allocWithParams(fCoreFaultReporter);
	return OSDynamicCast(CommonFaultReporter, fFaultReporter) != nullptr;
}

void
QwzAirport::releaseAll()
{
	stopAsync();
	OSSafeReleaseNULL(fFaultReporter);
	OSSafeReleaseNULL(fCoreFaultReporter);
	OSSafeReleaseNULL(fFaultStream);
	OSSafeReleaseNULL(fLogger);
	/* Stop only started pipes, while their work loop exists. */
	if (fSnapshotsPipeStarted)
		fSnapshotsPipe->CCPipe::stopPipe();
	if (fDataPathPipeStarted)
		fDataPathPipe->CCPipe::stopPipe();
	if (fLogPipeStarted)
		fLogPipe->CCPipe::stopPipe();
	fSnapshotsPipeStarted = fDataPathPipeStarted = fLogPipeStarted = false;
	OSSafeReleaseNULL(fLogPipe);
	OSSafeReleaseNULL(fDataPathPipe);
	OSSafeReleaseNULL(fSnapshotsPipe);
	if (fWorkQueue != nullptr) {
		if (fGate != nullptr) {
			fWorkQueue->removeEventSource(fGate);
			OSSafeReleaseNULL(fGate);
		}
		OSSafeReleaseNULL(fWorkQueue);
	}
}

/* On failure QwzWifi falls back to Ethernet (kext/airport_api.h). */
bool
QwzAirport::start(IOService *provider)
{
	if (startController(provider))
		return true;
	provider->callPlatformFunction(QWZ_AIRPORT_UNAVAILABLE, false,
	    (void *)"native controller did not start", nullptr, nullptr, nullptr);
	return false;
}

bool
QwzAirport::startController(IOService *provider)
{
	IOSkywalkEthernetInterface::RegistrationInfo reg;
	const IONetworkMedium *medium;
	IOReturn r;

	ALOG("start, version " QWZ_AIRPORT_VERSION " (built " __DATE__ " " __TIME__ ")");
	fQwz = provider;
	trace("connect QwzWifi");

	fFront.version = QWZ_AIRPORT_API_VERSION;
	fFront.size = sizeof(fFront);
	fFront.cookie = this;
	fFront.event = frontEvent;
	fFront.input = frontInput;
	fFront.park = park;
	fDrv.version = QWZ_AIRPORT_API_VERSION;
	fDrv.size = sizeof(fDrv);
	r = provider->callPlatformFunction(QWZ_AIRPORT_CONNECT, false, &fFront, &fDrv,
	    nullptr, nullptr);
	if (r != kIOReturnSuccess) {
		ALOG("QwzWifi refused the connection: 0x%x", r);
		return false;
	}
	fConnected = true;
	fNativeData = fDrv.data_owner(fDrv.dev) != 0;
	setProperty("QwzAirportDataMode", fNativeData ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("QwzAirportMACPolicy", "WCL runtime MAC");
	if (fDrv.hw_address(fDrv.dev, fMac) != 0) {
		ALOG("QwzWifi has no MAC address yet (firmware not up)");
		releaseAll();
		return false;
	}
	/* QWZMAC: C2 keeps QwzWifi's real en interface alive. A second
	 * interface with its MAC is assigned the same persisted en unit by
	 * configd (en4 here), and naming fails with kIOReturnNoSpace. Give
	 * this control-only front a distinct locally administered unicast
	 * identity. No firmware/vdev MAC is changed; replace this arrangement
	 * when C4 transfers ownership of the real data interface.
	 */
	if (!fNativeData) {
		fMac[0] = (fMac[0] | 0x02) & 0xfe;
		fMac[5] ^= 0x01;
	} // distinct even if the hardware MAC is already local
	trace("create work queue");
	// Preserve order when coalescing radio state notifications.
	fRadioTc = thread_call_allocate_with_options(radioTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	// Slow off/on operations must never overlap. Re-entry queues one
	// follow-up invocation while the running worker drains the latest target.
	fPowerTc = thread_call_allocate_with_options(powerTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	// QWZMAC: share the provider queue, as in laobamac's wrapper. On
	// 25G241 IO80211WorkQueue::getThread() returns NULL; a different
	// provider loop makes IONetworkController::start pass it to
	// ml_thread_policy and panic. Each owner holds its own reference.
	fWorkQueue = OSDynamicCast(IO80211WorkQueue, provider->getWorkLoop());
	if (fWorkQueue != nullptr)
		fWorkQueue->retain();
	if (fRadioTc == nullptr || fPowerTc == nullptr || fWorkQueue == nullptr) {
		releaseAll();
		return false;
	}

	UInt8 builtIn = 0;
	OSData *bi = OSData::withBytes(&builtIn, sizeof(builtIn));
	setProperty("built-in", bi);
	OSSafeReleaseNULL(bi);
	setProperty("DriverKitDriver", kOSBooleanFalse);
	trace("CoreCapture init");
	if (!initCCLogs()) {
		ALOG("CoreCapture pipes or fault reporter failed");
		releaseAll();
		return false;
	}
	trace("IO80211Controller::start enter");
	if (!super::start(provider)) {
		ALOG("IO80211Controller::start failed");
		releaseAll();
		return false;
	}
	trace("IO80211Controller::start complete");
	fGate = IOCommandGate::commandGate(this);
	if (fGate == nullptr || fWorkQueue->addEventSource(fGate) != kIOReturnSuccess) {
		OSSafeReleaseNULL(fGate);
		super::stop(provider);
		releaseAll();
		return false;
	}
	if (!createMediumTables() || (medium = getSelectedMedium()) == nullptr ||
	    !setCurrentMedium(medium)) {
		ALOG("cannot publish the medium");
		super::stop(provider);
		releaseAll();
		return false;
	}

	trace("Wi-Fi interface init");
	fNetIf = new QwzAirportInterface;
	if (fNetIf == nullptr || !fNetIf->initWithController(this)) {
		ALOG("Wi-Fi interface init failed");
		OSSafeReleaseNULL(fNetIf);
		super::stop(provider);
		releaseAll();
		return false;
	}
	trace("Wi-Fi interface attach");
	fNetIf->setInterfaceRole(1);
	fNetIf->setInterfaceId(1);
	if (!fNetIf->attach(this) || !attachInterface(fNetIf, this)) {
		ALOG("cannot attach the Wi-Fi interface");
		goto fail;
	}
	trace("BSD interface create");
	/* The BSD interface: createInterface() below; registered at the end. */
	if (!IONetworkController::attachInterface((IONetworkInterface **)&fBsdIf, false)) {
		ALOG("cannot attach the BSD interface");
		goto fail;
	}
	trace("registration info init");
	bzero(&reg, sizeof(reg));
	if (!fNetIf->initRegistrationInfo(&reg, 1, sizeof(reg))) {
		ALOG("initRegistrationInfo failed");
		goto fail;
	}
	/*
	 * IOSkywalkNetworkInterface and IOSkywalkEthernetInterface each keep
	 * their own copy of the registration info (laobamac's finding).
	 */
	fNetIf->mExpansionData->fRegistrationInfo =
	    (IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(reg));
	fNetIf->mExpansionData2->fRegistrationInfo =
	    (IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(reg));
	if (fNetIf->mExpansionData->fRegistrationInfo == nullptr ||
	    fNetIf->mExpansionData2->fRegistrationInfo == nullptr) {
		goto fail;
	}
	memcpy(fNetIf->mExpansionData->fRegistrationInfo, &reg, sizeof(reg));
	memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &reg, sizeof(reg));
	/* Frames go through fBsdIf, not through Skywalk. */
	fNetIf->deferBSDAttach(true);
	trace("Wi-Fi interface start enter");
	if (!fNetIf->start(this)) {
		ALOG("Wi-Fi interface start failed");
		goto fail;
	}
	setLinkStatus(kIONetworkLinkValid);
	trace("Wi-Fi interface started; publish BSD");
	fBsdIf->registerService();
	registerService();

	/*
	 * registerService() schedules BSD attachment; it does not complete it.
	 * WCL's synchronous ready messages must wait for prepareBSDInterface()
	 * AND for startup to finish. Do not enter WCL from this matching thread.
	 */
	fPowerState = fDrv.power(fDrv.dev) ? 1 : 0;
	__atomic_store_n(&fRadioState, fPowerState ? QWZMAC_RADIO_UP :
	    QWZMAC_RADIO_DOWN, __ATOMIC_SEQ_CST);
	trace("controller started; waiting for BSD attachment");
	__atomic_fetch_or(&fLifecycle, (unsigned)Started, __ATOMIC_SEQ_CST);
	scheduleRadio();
	ALOG("started, radio %s", fPowerState ? "on" : "off");
	return true;

fail:
	stopAsync();
	super::stop(provider);
	/* BSD keeps a reference to the Skywalk interface: detach it first. */
	if (fBsdIf != nullptr) {
		IONetworkController::detachInterface(fBsdIf, true);
		OSSafeReleaseNULL(fBsdIf);
	}
	if (fNetIf != nullptr) {
		detachInterface(fNetIf, true);
		OSSafeReleaseNULL(fNetIf);
	}
	releaseAll();
	return false;
}

void
QwzAirport::stop(IOService *provider)
{
	QwzAirportInterface *netif = fNetIf;

	ALOG("stop");
	stopAsync();
	if (fGate != nullptr)
		fGate->runAction(radioGated, (void *)(intptr_t)QWZMAC_RADIO_DOWN);
	fNetIf = nullptr;
	setLinkStatus(kIONetworkLinkValid);
	super::stop(provider);
	if (fBsdIf != nullptr) {
		IONetworkController::detachInterface(fBsdIf, true);
		OSSafeReleaseNULL(fBsdIf);
	}
	if (netif != nullptr) {
		detachInterface(netif, true);
		netif->release();
	}
	releaseAll();
}

void
QwzAirport::free()
{
	releaseAll();
	if (fAsyncLock) { IOLockFree(fAsyncLock); fAsyncLock = nullptr; }
	if (fLifecycleLock != nullptr) {
		IOLockFree(fLifecycleLock);
		fLifecycleLock = nullptr;
	}
	super::free();
}

/* Park without detach/terminate and without disconnecting our own callback.
 * Try-lock is essential: stopAsync may already be waiting for this callback. */
int QwzAirport::park(void *cookie)
{
	auto self = static_cast<QwzAirport *>(cookie);
	if (!self->fAsyncLock || !IOLockTryLock(self->fAsyncLock)) return EBUSY;
	IOLockLock(self->fLifecycleLock);
	bool stopping = (__atomic_load_n(&self->fLifecycle, __ATOMIC_SEQ_CST) & Stopping) != 0;
	__atomic_fetch_or(&self->fLifecycle, (unsigned)Stopping, __ATOMIC_SEQ_CST);
	IOLockUnlock(self->fLifecycleLock);
	if (stopping) { IOLockUnlock(self->fAsyncLock); return EBUSY; }
	__atomic_store_n(&self->fNativeLinkUp, 0, __ATOMIC_SEQ_CST);
	thread_call_t calls[] = {self->fPowerTc, self->fRadioTc};
	for (auto call : calls) {
		if (!call) continue;
		qwz_airport_call_stop(call);
	}
	if (self->fNetIf) { self->fNetIf->stopJoins(); self->fNetIf->stopScans(); }
	IOReturn result = self->fGate->runAction(parkGated);
	IOLockUnlock(self->fAsyncLock);
	return result == kIOReturnSuccess ? 0 : EIO;
}
IOReturn QwzAirport::parkGated(OSObject *owner, void *, void *, void *, void *)
{
	auto self = static_cast<QwzAirport *>(owner);
	self->fPowerState = 0;
	self->nativeLink(false, 0);
	self->fNativeData = false;
	if (self->fNetIf) self->fNetIf->radioChanged(QWZMAC_RADIO_DOWN);
	// Keep the parked endpoint distinct from the real factory-MAC Ethernet
	// endpoint. No packet rewrite and no firmware MAC request here.
	uint8_t parked[6];
	if (self->fDrv.hw_address(self->fDrv.dev, parked)) return kIOReturnNotReady;
	parked[0] = (parked[0] | 2) & 0xfe;
	parked[5] ^= 0x80;
	if (self->fBsdIf && self->fBsdIf->getIfnet()) {
		if (ifnet_set_lladdr(self->fBsdIf->getIfnet(), parked, 6)) return kIOReturnError;
		self->fBsdIf->setProperty(kIOMACAddress, parked, 6);
	}
	memcpy(self->fMac, parked, 6);
	self->setProperty(kIOMACAddress, parked, 6);
	if (self->fNetIf) self->fNetIf->setProperty(kIOMACAddress, parked, 6);
	self->setProperty("QwzAirportParked", true);
	return kIOReturnSuccess;
}

/* The provider callback only records state; it never waits on our gate. */
void
QwzAirport::frontEvent(void *cookie, int event, const void *data, size_t len)
{
	QwzAirport *me = (QwzAirport *)cookie;
	int state;

	if (event == QWZ_AIRPORT_EV_BEACON) {
		if ((__atomic_load_n(&me->fLifecycle, __ATOMIC_SEQ_CST) &
		    (Started | BSDAttached | Stopping)) == (Started | BSDAttached) && me->fNetIf)
			me->fNetIf->beaconReceived();
		return;
	}
	if (event != QWZ_AIRPORT_EV_RADIO || data == nullptr || len != sizeof(state))
		return;
	memcpy(&state, data, sizeof(state));
	__atomic_store_n(&me->fRadioState, state, __ATOMIC_SEQ_CST);
	me->scheduleRadio();
}

void
QwzAirport::bsdAttached()
{
	__atomic_fetch_or(&fLifecycle, (unsigned)BSDAttached, __ATOMIC_SEQ_CST);
	scheduleRadio();
}

/* Serialize checking readiness + enqueue against stopAsync() freeing the call. */
void
QwzAirport::scheduleRadio()
{
	IOLockLock(fLifecycleLock);
	if ((__atomic_load_n(&fLifecycle, __ATOMIC_SEQ_CST) &
	    (Started | BSDAttached | Stopping)) == (Started | BSDAttached))
		thread_call_enter(fRadioTc);
	IOLockUnlock(fLifecycleLock);
}

void
QwzAirport::bsdDetached()
{
	__atomic_fetch_and(&fLifecycle, ~(unsigned)BSDAttached, __ATOMIC_SEQ_CST);
}

void
QwzAirport::radioTc(thread_call_param_t p0, thread_call_param_t)
{
	QwzAirport *me = (QwzAirport *)p0;
	if ((__atomic_load_n(&me->fLifecycle, __ATOMIC_SEQ_CST) &
	    (Started | BSDAttached | Stopping)) != (Started | BSDAttached))
		return;
	me->fGate->runAction(radioGated,
	    (void *)(intptr_t)__atomic_load_n(&me->fRadioState, __ATOMIC_SEQ_CST));
}

IOReturn
QwzAirport::radioGated(OSObject *owner, void *a0, void *, void *, void *)
{
	QwzAirport *me = (QwzAirport *)owner;
	int state = (int)(intptr_t)a0;

	if (state == QWZMAC_RADIO_UP)
		me->fPowerState = 1;
	else if (state == QWZMAC_RADIO_DOWN)
		me->fPowerState = 0;
	if (me->fNetIf != nullptr &&
	    (__atomic_load_n(&me->fLifecycle, __ATOMIC_SEQ_CST) & BSDAttached))
		me->fNetIf->radioChanged(state);
	me->trace(state == QWZMAC_RADIO_UP ? "native interface ready" :
	    state == QWZMAC_RADIO_DOWN ? "radio off" : "radio stopping");
	return kIOReturnSuccess;
}

/* Snapshot actual user intent, not fPowerState (which also tracks hardware).
 * Never hold this lock while calling back into WCL/setPOWER. */
QwzPower::Request
QwzAirport::powerRequest()
{
	IOLockLock(fLifecycleLock);
	QwzPower::Request request = {0, 0};
	if (!(__atomic_load_n(&fLifecycle, __ATOMIC_SEQ_CST) & Stopping)) {
		request.wanted = __atomic_load_n(&fPowerWanted, __ATOMIC_SEQ_CST);
		request.serial = fPowerSerial;
	}
	IOLockUnlock(fLifecycleLock);
	return request;
}

/* Radio on/off takes seconds: done here, outside the gate. */
void
QwzAirport::powerTc(thread_call_param_t p0, thread_call_param_t)
{
	auto me = static_cast<QwzAirport *>(p0);
	QwzPower::drain(
	    [me]() {
		IOLockLock(me->fLifecycleLock);
		QwzPower::Request request = {
		    __atomic_load_n(&me->fPowerWanted, __ATOMIC_SEQ_CST), me->fPowerSerial};
		IOLockUnlock(me->fLifecycleLock);
		return request;
	    },
	    [me]() { return me->fDrv.power(me->fDrv.dev) ? 1 : 0; },
	    [me](int on) { return me->fDrv.set_power(me->fDrv.dev, on); },
	    [me](QwzPower::Request request, int actual, int error) {
		me->setProperty("QwzAirportPowerCompleted", request.serial, 32);
		me->setProperty("QwzAirportPowerActual", actual, 32);
		me->setProperty("QwzAirportPowerError", static_cast<uint32_t>(error), 32);
		ALOG("power request %u: wanted=%d actual=%d errno=%d",
		    request.serial, request.wanted, actual, error);
	    },
	    [me]() { return
		(__atomic_load_n(&me->fLifecycle, __ATOMIC_SEQ_CST) & Stopping) != 0; });
}

IOWorkLoop *
QwzAirport::getWorkLoop() const
{
	return fWorkQueue;
}

bool
QwzAirport::createWorkLoop()
{
	return fWorkQueue != nullptr;
}

bool
QwzAirport::createWorkQueue()
{
	return fWorkQueue != nullptr;
}

IO80211WorkQueue *
QwzAirport::getWorkQueue() const
{
	return fWorkQueue;
}

CommonFaultReporter *
QwzAirport::getFaultReporterFromDriver()
{
	return fFaultReporter;
}

bool
QwzAirport::getLogPipes(CCPipe **log, CCPipe **events, CCPipe **snapshots)
{
	bool ret = false;

	if (log != nullptr) {
		*log = fLogPipe;
		ret = true;
	}
	if (events != nullptr) {
		*events = fDataPathPipe;
		ret = true;
	}
	if (snapshots != nullptr) {
		*snapshots = fSnapshotsPipe;
		ret = true;
	}
	return ret;
}

IONetworkInterface *
QwzAirport::createInterface()
{
	QwzAirportEthernet *netif = new QwzAirportEthernet;

	if (netif == nullptr)
		return nullptr;
	if (!netif->initWithSkywalkInterface(this, fNetIf)) {
		netif->release();
		return nullptr;
	}
	return netif;
}

bool
QwzAirport::configureInterface(IONetworkInterface *netif)
{
	return super::configureInterface(netif);
}

bool
QwzAirport::createMediumTables()
{
	OSDictionary *dict = OSDictionary::withCapacity(2);
	IONetworkMedium *m;
	bool ok;

	if (dict == nullptr)
		return false;
	m = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
	IONetworkMedium::addMedium(dict, m);
	OSSafeReleaseNULL(m);
	m = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
	IONetworkMedium::addMedium(dict, m);
	OSSafeReleaseNULL(m);
	ok = publishMediumDictionary(dict);
	dict->release();
	if (ok) {
		const IONetworkMedium *sel = IONetworkMedium::getMediumWithType(
		    getMediumDictionary(), kIOMediumIEEE80211);
		ok = sel != nullptr && setSelectedMedium(sel);
	}
	return ok;
}

IOReturn
QwzAirport::selectMedium(const IONetworkMedium *medium)
{
	setSelectedMedium(medium);
	return kIOReturnSuccess;
}

/* The native BSD interface is the only IP data path in C4 mode. */
UInt32
QwzAirport::outputPacket(mbuf_t packet, void *)
{
	if (packet == nullptr) return kIOReturnOutputDropped;
	size_t length = mbuf_pkthdr_len(packet);
	int error = ENETDOWN;
	if (fNativeData && __atomic_load_n(&fNativeLinkUp, __ATOMIC_SEQ_CST) && length >= 14 && length <= 2318) {
		void *bytes = IOMalloc(length);
		if (bytes != nullptr) {
			error = mbuf_copydata(packet, 0, length, bytes);
			if (!error) error = fDrv.output(fDrv.dev, bytes, length);
			IOFree(bytes, length);
		}
	}
	freePacket(packet);
	return error == 0 ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

void QwzAirport::frontInput(void *cookie, const void *bytes, size_t length)
{
	auto self = static_cast<QwzAirport *>(cookie);
	// Called under the driver's spl lock. Never enter the WCL gate here.
	if (!self->fNativeData || !__atomic_load_n(&self->fNativeLinkUp, __ATOMIC_SEQ_CST) ||
	    self->fBsdIf == nullptr || length < 14 || length > 2318)
		return;
	mbuf_t packet = self->allocatePacket(static_cast<UInt32>(length));
	if (packet == nullptr) return;
	if (mbuf_copyback(packet, 0, length, bytes, MBUF_DONTWAIT)) {
		self->freePacket(packet); return;
	}
	ifnet_stat_increment_in(self->fBsdIf->getIfnet(), 1, static_cast<uint32_t>(length), 0);
	self->fBsdIf->inputPacket(packet, static_cast<UInt32>(length));
}

void QwzAirport::nativeLink(bool up, uint64_t rate)
{
	__atomic_store_n(&fNativeLinkUp, up ? 1U : 0U, __ATOMIC_SEQ_CST);
	if (fBsdIf != nullptr) fBsdIf->setLinkState(up ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown);
	setLinkStatus(kIONetworkLinkValid | (up ? kIONetworkLinkActive : 0),
	    getSelectedMedium(), up ? rate : 0);
}

IOReturn
QwzAirport::getHardwareAddress(IOEthernetAddress *addr)
{
	memcpy(addr->bytes, fMac, sizeof(fMac));
	return kIOReturnSuccess;
}

/* As AirportMtkWlan: synchronize BSD, start firmware reconfiguration, and
 * retain an arriving join until it completes. The slow work stays outside
 * WCL's gate; internal MAC changes do not replay DriverAvailable/reset. */
IOReturn
QwzAirport::setHardwareAddress(const void *addr, UInt32 len)
{
	auto plan = AirportWCL::planMacUpdate(fMac, (const uint8_t *)addr, len, false);
	if (plan == AirportWCL::MacUpdate::Invalid) return kIOReturnBadArgument;
	if (plan == AirportWCL::MacUpdate::Unchanged) return kIOReturnSuccess;
	if (fNetIf && fNetIf->macBusy()) return kIOReturnBusy;
	if (!fNativeData) return kIOReturnUnsupported;
	if (!fNetIf || !fBsdIf || !fBsdIf->getIfnet()) return kIOReturnNotReady;
	nativeLink(false, 0);
	if (fBsdIf && fBsdIf->getIfnet()) {
		int error = ifnet_set_lladdr(fBsdIf->getIfnet(), addr, 6);
		if (error) return kIOReturnError;
	}
	memcpy(fMac, addr, 6);
	setProperty("IOMACAddress", const_cast<void *>(addr), 6);
	fBsdIf->setProperty(kIOMACAddress, const_cast<void *>(addr), 6);
	setProperty("QwzAirportMACPolicy", "WCL runtime MAC; applied before join");
	fNetIf->queueMacAddress(fMac);
	ALOG("WCL runtime MAC accepted; reconfiguration started asynchronously");
	return kIOReturnSuccess;
}

/* qwz has no receive filter: it passes all multicast. */
IOReturn
QwzAirport::setPromiscuousMode(bool)
{
	return kIOReturnSuccess;
}

IOReturn
QwzAirport::setMulticastMode(bool)
{
	return kIOReturnSuccess;
}

IOReturn
QwzAirport::setMulticastList(IOEthernetAddress *, UInt32)
{
	return kIOReturnSuccess;
}

UInt32
QwzAirport::getFeatures() const
{
	return 0;
}

const OSString *
QwzAirport::newVendorString() const
{
	return OSString::withCString("Qualcomm");
}

const OSString *
QwzAirport::newModelString() const
{
	return OSString::withCString("WCN7850 Wi-Fi (qwz)");
}

/* As laobamac: 802.11n is "enabled", nothing else is supported (102). */
SInt32
QwzAirport::enableFeature(IO80211FeatureCode code, void *)
{
	return code == kIO80211Feature80211n ? 0 : 102;
}

IOReturn
QwzAirport::getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *hv)
{
	char fw[64];

	if (hv == nullptr)
		return kIOReturnError;
	fDrv.fw_version(fDrv.dev, fw, sizeof(fw));
	hv->version = APPLE80211_VERSION;
	snprintf(hv->string, sizeof(hv->string), "QwzAirport " QWZ_AIRPORT_VERSION " fw %s", fw);
	hv->string_len = (u_int16_t)strlen(hv->string);
	return kIOReturnSuccess;
}

IOReturn
QwzAirport::getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *hv)
{
	if (hv == nullptr)
		return kIOReturnError;
	hv->version = APPLE80211_VERSION;
	fDrv.fw_version(fDrv.dev, hv->string, sizeof(hv->string));
	hv->string_len = (u_int16_t)strlen(hv->string);
	return kIOReturnSuccess;
}

/*
 * What qwz does: WPA2 (RSN) with CCMP, TKIP group keys in software, short
 * slot and preamble, WME, HT with short GI. The bits WCL looks at are
 * laobamac's (its WCL branch): no WPA1, scan completion events (byte 5),
 * no HE (byte 17).
 */
IOReturn
QwzAirport::getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *cd)
{
	if (cd == nullptr)
		return kIOReturnError;
	bzero(cd, sizeof(*cd));
	cd->version = APPLE80211_VERSION;
	cd->capabilities[0] = (1 << APPLE80211_CAP_AES_CCM) | (1 << APPLE80211_CAP_TKIP);
	cd->capabilities[1] = (1 << (APPLE80211_CAP_SHSLOT - 8)) |
	    (1 << (APPLE80211_CAP_SHPREAMBLE - 8)) | (1 << (APPLE80211_CAP_WPA2 - 8)) |
	    (1 << (APPLE80211_CAP_TKIPMIC - 8));
	cd->capabilities[2] = (1 << (APPLE80211_CAP_WME - 16)) |
	    (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
	    (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
	cd->capabilities[5] = 0x40;
	return kIOReturnSuccess;
}

IOReturn
QwzAirport::getPOWER(IO80211SkywalkInterface *, apple80211_power_data *pd)
{
	if (pd == nullptr)
		return kIOReturnError;
	/* What AppleBCMWLANCore::getPOWER() fills on 25G241: one radio. */
	pd->version = APPLE80211_VERSION;
	pd->num_radios = 1;
	pd->power_state[0] = fPowerState;
	return kIOReturnSuccess;
}

IOReturn
QwzAirport::setPOWER(IO80211SkywalkInterface *, apple80211_power_data *pd)
{
	if (pd == nullptr)
		return kIOReturnError;
	if (pd->num_radios == 0)
		return kIOReturnSuccess;
	IOLockLock(fLifecycleLock);
	if ((__atomic_load_n(&fLifecycle, __ATOMIC_SEQ_CST) & Stopping) ||
	    fPowerTc == nullptr) {
		IOLockUnlock(fLifecycleLock);
		return kIOReturnNotReady;
	}
	ALOG("setPOWER %u", pd->power_state[0]);
	fPowerState = pd->power_state[0] ? 1 : 0;
	__atomic_store_n(&fPowerWanted, (int)fPowerState, __ATOMIC_SEQ_CST);
	setProperty("QwzAirportPowerRequested", ++fPowerSerial, 32);
	setProperty("QwzAirportPowerTarget", fPowerState, 32);
	thread_call_enter(fPowerTc);
	IOLockUnlock(fLifecycleLock);
	return kIOReturnSuccess;
}

/*
 * The country of the firmware's regulatory domain, only reported
 * (CLAUDE.md: no regulatory changes): "ZZ" until the firmware sent one.
 */
IOReturn
QwzAirport::getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *cd)
{
	char cc[3];

	if (cd == nullptr)
		return kIOReturnError;
	bzero(cd, sizeof(*cd));
	cd->version = APPLE80211_VERSION;
	if (fDrv.country(fDrv.dev, cc) != 0)
		strlcpy(cc, "ZZ", sizeof(cc));
	memcpy(cd->cc, cc, 2);
	return kIOReturnSuccess;
}

/* macOS's country (from location) is never applied to the chip. */
IOReturn
QwzAirport::setCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *cd)
{
	if (cd == nullptr)
		return kIOReturnBadArgument;
	ALOG("macOS country %c%c ignored (the firmware's own is used)",
	    cd->cc[0] ? cd->cc[0] : '-', cd->cc[1] ? cd->cc[1] : '-');
	return kIOReturnSuccess;
}
