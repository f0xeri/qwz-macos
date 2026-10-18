/*
 * QwzAirport: the IO80211Controller (QwzAirport.hpp).
 *
 * Loaded by hand during development (sudo kmutil load -p build/QwzAirport.kext):
 * it matches the running QwzWifi, connects through callPlatformFunction()
 * (kext/airport_api.h) and publishes a Wi-Fi interface. QwzWifi keeps its
 * Ethernet interface and HeliPort; the data path moves here in stage C4.
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

#define super IO80211Controller
OSDefineMetaClassAndStructors(QwzAirport, IO80211Controller);

bool
QwzAirport::init(OSDictionary *properties)
{
	if (!super::init(properties))
		return false;
	fQwz = nullptr;
	bzero(&fDrv, sizeof(fDrv));
	bzero(&fFront, sizeof(fFront));
	fConnected = false;
	bzero(fMac, sizeof(fMac));
	fWorkQueue = nullptr;
	fGate = nullptr;
	fNetIf = nullptr;
	fBsdIf = nullptr;
	fPowerState = 0;
	fPowerWanted = 0;
	fPowerTc = nullptr;
	fLogPipe = fDataPathPipe = fSnapshotsPipe = nullptr;
	fLogPipeStarted = fDataPathPipeStarted = fSnapshotsPipeStarted = false;
	fLogger = nullptr;
	fFaultStream = nullptr;
	fCoreFaultReporter = nullptr;
	fFaultReporter = nullptr;
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
	char build[32] = "";
	size_t len = sizeof(build);
	OSArray *builds;
	bool ok = false;

	if (sysctlbyname("kern.osversion", build, &len, nullptr, 0) != 0)
		build[0] = '\0';
	builds = OSDynamicCast(OSArray, getProperty("QwzAirportBuilds"));
	for (unsigned i = 0; builds != nullptr && i < builds->getCount(); i++) {
		OSString *b = OSDynamicCast(OSString, builds->getObject(i));
		if (b != nullptr && b->isEqualTo(build))
			ok = true;
	}
	if (!ok) {
		ALOG("macOS build %s is not in QwzAirportBuilds (its IO80211 ABI was "
		    "not checked with scripts/kc-abi.py --check): not starting", build);
		return nullptr;
	}
	return super::probe(provider, score);
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
	fLogPipe = CCPipe::withOwnerNameCapacity(this, "com.local.QwzAirport",
	    "DriverLogs", &o);
	if (fLogPipe == nullptr || getWorkLoop() == nullptr)
		return false;
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
	fCoreFaultReporter = CCFaultReporter::withStreamWorkloop(ds, fWorkQueue);
	if (fCoreFaultReporter == nullptr)
		return false;
	fFaultReporter = IO80211FaultReporter::allocWithParams(fCoreFaultReporter);
	return OSDynamicCast(CommonFaultReporter, fFaultReporter) != nullptr;
}

void
QwzAirport::releaseAll()
{
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
	if (fConnected) {
		/* No QwzWifi event runs or starts once this returns. */
		fQwz->callPlatformFunction(QWZ_AIRPORT_DISCONNECT, false,
		    fFront.cookie, nullptr, nullptr, nullptr);
		fConnected = false;
	}
	if (fPowerTc != nullptr) {
		while (thread_call_isactive(fPowerTc)) {
			thread_call_cancel(fPowerTc);
			IOSleep(5);
		}
		thread_call_free(fPowerTc);
		fPowerTc = nullptr;
	}
	if (fWorkQueue != nullptr) {
		if (fGate != nullptr) {
			fWorkQueue->removeEventSource(fGate);
			OSSafeReleaseNULL(fGate);
		}
		OSSafeReleaseNULL(fWorkQueue);
	}
}

bool
QwzAirport::start(IOService *provider)
{
	IOSkywalkEthernetInterface::RegistrationInfo reg;
	const IONetworkMedium *medium;
	IOReturn r;

	ALOG("start, version " QWZ_AIRPORT_VERSION " (built " __DATE__ " " __TIME__ ")");
	fQwz = provider;

	fFront.version = QWZ_AIRPORT_API_VERSION;
	fFront.size = sizeof(fFront);
	fFront.cookie = this;
	fFront.event = frontEvent;
	fDrv.version = QWZ_AIRPORT_API_VERSION;
	fDrv.size = sizeof(fDrv);
	r = provider->callPlatformFunction(QWZ_AIRPORT_CONNECT, false, &fFront, &fDrv,
	    nullptr, nullptr);
	if (r != kIOReturnSuccess) {
		ALOG("QwzWifi refused the connection: 0x%x", r);
		return false;
	}
	fConnected = true;
	if (fDrv.hw_address(fDrv.dev, fMac) != 0) {
		ALOG("QwzWifi has no MAC address yet (firmware not up)");
		releaseAll();
		return false;
	}
	fPowerTc = thread_call_allocate(powerTc, this);
	fWorkQueue = IO80211WorkQueue::workQueue();
	if (fPowerTc == nullptr || fWorkQueue == nullptr) {
		releaseAll();
		return false;
	}

	UInt8 builtIn = 0;
	OSData *bi = OSData::withBytes(&builtIn, sizeof(builtIn));
	setProperty("built-in", bi);
	OSSafeReleaseNULL(bi);
	setProperty("DriverKitDriver", kOSBooleanFalse);
	if (!initCCLogs()) {
		ALOG("CoreCapture pipes or fault reporter failed");
		releaseAll();
		return false;
	}
	if (!super::start(provider)) {
		ALOG("IO80211Controller::start failed");
		releaseAll();
		return false;
	}
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

	fNetIf = new QwzAirportInterface;
	if (fNetIf == nullptr || !fNetIf->initWithController(this)) {
		ALOG("Wi-Fi interface init failed");
		OSSafeReleaseNULL(fNetIf);
		super::stop(provider);
		releaseAll();
		return false;
	}
	fNetIf->setInterfaceRole(1);
	fNetIf->setInterfaceId(1);
	if (!fNetIf->attach(this) || !attachInterface(fNetIf, this)) {
		ALOG("cannot attach the Wi-Fi interface");
		goto fail;
	}
	/* The BSD interface: createInterface() below; registered at the end. */
	if (!IONetworkController::attachInterface((IONetworkInterface **)&fBsdIf, false)) {
		ALOG("cannot attach the BSD interface");
		goto fail;
	}
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
	if (!fNetIf->start(this)) {
		ALOG("Wi-Fi interface start failed");
		goto fail;
	}
	setLinkStatus(kIONetworkLinkValid);
	fBsdIf->registerService();
	registerService();

	fPowerState = fDrv.power(fDrv.dev) ? 1 : 0;
	/* Tell WCL whether the driver is available (later: frontEvent()). */
	fGate->runAction(radioGated, (void *)(intptr_t)(fPowerState ?
	    QWZMAC_RADIO_UP : QWZMAC_RADIO_DOWN));
	ALOG("started, radio %s", fPowerState ? "on" : "off");
	return true;

fail:
	super::stop(provider);
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
	/* First no more QwzWifi events into the interface. */
	if (fConnected) {
		fQwz->callPlatformFunction(QWZ_AIRPORT_DISCONNECT, false,
		    fFront.cookie, nullptr, nullptr, nullptr);
		fConnected = false;
	}
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
	super::free();
}

/* QwzWifi's event thread (kext/airport_api.h): never our work loop. */
void
QwzAirport::frontEvent(void *cookie, int event, const void *data, size_t len)
{
	QwzAirport *me = (QwzAirport *)cookie;
	int state;

	if (event != QWZ_AIRPORT_EV_RADIO || len != sizeof(state) || me->fGate == nullptr)
		return;
	memcpy(&state, data, sizeof(state));
	me->fGate->runAction(radioGated, (void *)(intptr_t)state);
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
	if (me->fNetIf != nullptr)
		me->fNetIf->radioChanged(state);
	return kIOReturnSuccess;
}

/* Radio on/off takes seconds: done here, outside the gate. */
void
QwzAirport::powerTc(thread_call_param_t p0, thread_call_param_t)
{
	QwzAirport *me = (QwzAirport *)p0;
	int on = __atomic_load_n(&me->fPowerWanted, __ATOMIC_SEQ_CST);
	int error;

	if (!me->fConnected || me->fDrv.power(me->fDrv.dev) == on)
		return;
	error = me->fDrv.set_power(me->fDrv.dev, on);
	ALOG("radio %s: %d", on ? "on" : "off", error);
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

/* Stage C2: no data path here yet (QwzWifi's en4 carries it). */
UInt32
QwzAirport::outputPacket(mbuf_t m, void *param)
{
	(void)param;
	if (m != nullptr)
		freePacket(m);
	return kIOReturnOutputDropped;
}

IOReturn
QwzAirport::getHardwareAddress(IOEthernetAddress *addr)
{
	memcpy(addr->bytes, fMac, sizeof(fMac));
	return kIOReturnSuccess;
}

/*
 * WCL sets the (private) address itself and replays it after every
 * DriverAvailable. qwz creates its vdev with the factory address, and
 * changing it needs at least a new vdev (docs/native-wifi-research.md §8.4),
 * so for now only the current address is accepted. How WCL reacts to the
 * refusal is a C2 test question.
 */
IOReturn
QwzAirport::setHardwareAddress(const void *addr, UInt32 len)
{
	switch (AirportWCL::planMacUpdate(fMac, (const uint8_t *)addr, len, false)) {
	case AirportWCL::MacUpdate::Unchanged:
		return kIOReturnSuccess;
	case AirportWCL::MacUpdate::Invalid:
		return kIOReturnBadArgument;
	default:
		ALOG("MAC address change refused (not supported yet)");
		return kIOReturnUnsupported;
	}
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
	ALOG("setPOWER %u", pd->power_state[0]);
	fPowerState = pd->power_state[0] ? 1 : 0;
	__atomic_store_n(&fPowerWanted, (int)fPowerState, __ATOMIC_SEQ_CST);
	thread_call_enter(fPowerTc);
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
