/*
 * QwzWifi — Qualcomm WCN7850 (17cb:1107) driver for macOS, port of OpenBSD qwz(4).
 *
 * Stage 1: run qwz's attach unchanged (compat/autoconf.c) up to "firmware
 * ready" and the MAC address.
 * Stage 2: bring the interface up; net80211 logs the networks it finds.
 * Stage 3: join the configured network (WPA2-PSK) and appear to macOS as
 * an Ethernet interface (itlwm style): Ethernet frames are copied between
 * macOS mbufs and OpenBSD mbufs in compat/autoconf.c.
 * Stage 4: sleep/wake, packet counters, the link speed follows the TX rate.
 * HeliPort (itlwm's menu app) can pick the network: kext/QwzUserClient.cpp.
 * Native Wi-Fi: the separate QwzAirport.kext (airport/) connects through
 * callPlatformFunction() and kext/airport_api.h.
 *
 * Boot-args:
 *   -qwzon     required, otherwise the driver does not attach at all
 *   -qwzprobe  stage 0 only: log IDs/BAR/MSI and leave the device untouched
 *   qwzdebug=N qwz debug mask (QWZ_D_* in qwzvar.h), default 0 (errors only).
 *              0x7 = MISC|MHI|QMI; slows bring-up, qwz's 1 s QMI timeouts may fire
 *
 * The network to join at boot comes from the "QwzNetwork" dictionary (SSID,
 * Password) in the personality, which scripts/build.sh copies from
 * config/wifi.plist. Without it the driver only scans until HeliPort
 * picks a network.
 *
 * SPDX-License-Identifier: GPL-2.0-only (qwz parts: ISC)
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IONetworkStats.h>
#include <IOKit/network/IOOutputQueue.h>
#include <kern/thread_call.h>
#include <sys/kpi_mbuf.h>
#include <net/kpi_interface.h>
#include <pexpert/pexpert.h>

#include "os_bridge.h"
#include "../compat/include/qwzmac/os.h"
#include "../compat/include/qwzmac/attach.h"
#include "QwzWifi.h"

/* 802.11 MTU (2304) plus the Ethernet header, as in compat/autoconf.c. */
#define QWZ_FRAME_MAX	(2304 + 14)

/* How often the link speed shown by macOS follows the firmware's TX rate. */
#define QWZ_RATE_POLL_MS	2000

extern "C" uint32_t qwz_debug;	/* qwz/qwz.c, built with QWZ_DEBUG */

OSDefineMetaClassAndStructors(QwzWifi, IOEthernetController)

IOService *
QwzWifi::probe(IOService *provider, SInt32 *score)
{
	char tmp[16];

	if (!PE_parse_boot_argn("-qwzon", tmp, sizeof(tmp))) {
		QLOG("boot-arg -qwzon not set, not attaching");
		return nullptr;
	}
	return IOEthernetController::probe(provider, score);
}

/* Stage 0 diagnostics: config space reads only. */
void
QwzWifi::logDevice(IOPCIDevice *pci)
{
	UInt16 vid = pci->configRead16(kIOPCIConfigVendorID);
	UInt16 did = pci->configRead16(kIOPCIConfigDeviceID);
	UInt16 svid = pci->configRead16(kIOPCIConfigSubSystemVendorID);
	UInt16 sdid = pci->configRead16(kIOPCIConfigSubSystemID);
	UInt8  rev = pci->configRead8(kIOPCIConfigRevisionID);
	UInt16 cmd = pci->configRead16(kIOPCIConfigCommand);
	UInt32 bar0 = pci->configRead32(kIOPCIConfigBaseAddress0);
	UInt32 bar1 = pci->configRead32(kIOPCIConfigBaseAddress1);
	QLOG("found %04x:%04x sub %04x:%04x rev %02x cmd %04x bar0 %08x bar1 %08x",
	    vid, did, svid, sdid, rev, cmd, bar0, bar1);

	IODeviceMemory *mem = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (mem)
		QLOG("BAR0 phys 0x%llx len 0x%llx",
		    (unsigned long long)mem->getPhysicalAddress(),
		    (unsigned long long)mem->getLength());

	IOByteCount off = 0;
	if (pci->extendedFindPCICapability(kIOPCIMSICapability, &off) && off) {
		UInt16 ctl = pci->configRead16(off + 2);
		QLOG("MSI cap @0x%llx ctl %04x: %u vectors supported, 64bit=%d",
		    (unsigned long long)off, ctl, 1u << ((ctl >> 1) & 7),
		    (ctl >> 7) & 1);
	}
	off = 0;
	if (pci->extendedFindPCICapability(kIOPCIMSIXCapability, &off) && off)
		QLOG("MSI-X cap @0x%llx", (unsigned long long)off);
	off = 0;
	if (pci->extendedFindPCICapability(kIOPCIPowerManagementCapability, &off) && off)
		QLOG("PM cap @0x%llx pmcsr %04x", (unsigned long long)off,
		    pci->configRead16(off + 4));

	for (int i = 0; i < 40; i++) {
		int type = 0;
		if (pci->getInterruptType(i, &type) != kIOReturnSuccess)
			break;
		QLOG("interrupt index %d type 0x%x%s", i, type,
		    (type & kIOInterruptTypePCIMessaged) ? " (MSI)" : "");
	}
}

/*
 * The network to join, from the personality (config/wifi.plist at build
 * time). The property is removed right away so the password does not
 * show up in ioreg; it is never logged.
 */
bool
QwzWifi::readNetwork(qwzmac_network *net)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, getProperty("QwzNetwork"));
	OSString *ssid, *pass;
	bool ok = false;

	bzero(net, sizeof(*net));
	if (d == nullptr) {
		QLOG("no QwzNetwork in the config: scanning only");
		return false;
	}
	ssid = OSDynamicCast(OSString, d->getObject("SSID"));
	pass = OSDynamicCast(OSString, d->getObject("Password"));
	if (ssid == nullptr || pass == nullptr) {
		QLOG("QwzNetwork needs SSID and Password strings");
	} else if (ssid->getLength() < 1 || ssid->getLength() >= sizeof(net->ssid) ||
	    pass->getLength() >= sizeof(net->passphrase)) {
		QLOG("QwzNetwork: SSID or Password too long");
	} else {
		strlcpy(net->ssid, ssid->getCStringNoCopy(), sizeof(net->ssid));
		strlcpy(net->passphrase, pass->getCStringNoCopy(),
		    sizeof(net->passphrase));
		QLOG("network from config: \"%s\"", net->ssid);
		ok = true;
	}
	removeProperty("QwzNetwork");
	return ok;
}

bool
QwzWifi::createMedium()
{
	OSDictionary *dict = OSDictionary::withCapacity(1);
	bool ok;

	if (dict == nullptr)
		return false;
	fMedium = IONetworkMedium::medium(kIOMediumEthernetAuto, 0);
	if (fMedium == nullptr) {
		dict->release();
		return false;
	}
	IONetworkMedium::addMedium(dict, fMedium);
	ok = publishMediumDictionary(dict);
	dict->release();
	if (ok)
		ok = setCurrentMedium(fMedium) && setSelectedMedium(fMedium);
	return ok;
}

bool
QwzWifi::createWorkLoop()
{
	fWorkLoop = IOWorkLoop::workLoop();
	return fWorkLoop != nullptr;
}

IOWorkLoop *
QwzWifi::getWorkLoop() const
{
	return fWorkLoop;
}

/*
 * The interface keeps the packet counters netstat shows; the byte counters
 * the network stack counts by itself.
 */
bool
QwzWifi::configureInterface(IONetworkInterface *netif)
{
	IONetworkData *nd;

	if (!IOEthernetController::configureInterface(netif))
		return false;
	nd = netif->getParameter(kIONetworkStatsKey);
	if (nd != nullptr)
		fNetStats = (IONetworkStats *)nd->getBuffer();
	if (fNetStats == nullptr)
		QLOG("no network statistics buffer, netstat will show 0 packets");
	return true;
}

bool
QwzWifi::start(IOService *provider)
{
	qwzmac_network net;
	qwzmac_netif_ops ops;
	char tmp[16];
	int error;

	fPci = nullptr;
	fQwz = nullptr;
	fOsInit = false;
	fPowerNotifier = nullptr;
	fNetIf = nullptr;
	fStopping = false;
	fNativePreferred = false;
	fHeliportRequested = false;
	fModeTc = nullptr;
	fMedium = nullptr;
	fLinkTc = nullptr;
	fLinkUp = 0;
	fLinkSpeed = 0;
	fLinkBaudrate = 0;
	fNetStats = nullptr;
	fClientCalls = 0;
	bzero(fMac, sizeof(fMac));
	bzero(&fFront, sizeof(fFront));
	fFrontOn = false;
	fFrontCalls = 0;
	fFrontTc = nullptr;
	fRadioHead = fRadioTail = 0;
	fIfLock = IOLockAlloc();
	if (fIfLock == nullptr)
		return false;
	fRadioLock = IOSimpleLockAlloc();
	if (fRadioLock == nullptr) {
		IOLockFree(fIfLock);
		fIfLock = nullptr;
		return false;
	}

	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) {
		QLOG("provider is not IOPCIDevice");
		return false;
	}
	qwzmac_log_start(pci);
	QLOG("start, version " QWZ_VERSION " (built " __DATE__ " " __TIME__ ")");

	if (!IOEthernetController::start(provider)) {
		QLOG("IOEthernetController::start failed");
		qwzmac_log_stop();
		return false;
	}

	logDevice(pci);

	if (PE_parse_boot_argn("-qwzprobe", tmp, sizeof(tmp))) {
		QLOG("-qwzprobe: stage 0 only, device left untouched");
		qwzmac_log_publish();
		return true;
	}

	if (!createMedium()) {
		QLOG("cannot publish the medium");
		qwzmac_log_stop();
		return false;
	}
	fLinkTc = thread_call_allocate(linkTc, this);
	// One dispatcher preserves FIFO order across the unlocked front callback.
	fFrontTc = thread_call_allocate_with_options(frontTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	fModeTc = thread_call_allocate_with_options(modeTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (fLinkTc == nullptr || fFrontTc == nullptr || fModeTc == nullptr) {
		qwzmac_log_stop();
		return false;
	}

	if (!pci->open(this)) {
		QLOG("cannot open PCI device");
		qwzmac_log_stop();
		return false;
	}
	fPci = pci;

	if (!qwzmac_os_init(this, pci)) {
		QLOG("os bridge init failed");
		pci->close(this);
		fPci = nullptr;
		qwzmac_log_stop();
		return false;
	}
	fOsInit = true;

	/*
	 * Default 0, as in a stock OpenBSD kernel. The QMI/MHI debug output
	 * prints hex dumps byte by byte and made qwz miss its 1 s QMI
	 * timeouts (run 3); enable it only when chasing something specific.
	 */
	UInt32 mask = 0;
	PE_parse_boot_argn("qwzdebug", &mask, sizeof(mask));
	qwz_debug = mask;

	fNativePreferred = getProperty("QwzNativeData") == kOSBooleanTrue;
	bool have_net = fNativePreferred ? false : readNetwork(&net);
	bzero(&ops, sizeof(ops));
	ops.cookie = this;
	ops.ready = netReady;
	ops.input = netInput;
	ops.link = netLink;
	ops.radio = netRadio;

	QLOG("stage 1: attaching qwz (debug mask 0x%x)", (unsigned)mask);
	error = qwzmac_qwz_attach(pci, &ops, have_net ? &net : nullptr, &fQwz);
	bzero(&net, sizeof(net));	/* compat wiped it too */
	if (error) {
		QLOG("qwz attach failed: %d", error);
		qwzmac_os_fini();
		fOsInit = false;
		pci->close(this);
		fPci = nullptr;
		qwzmac_log_stop();
		return false;
	}

	/*
	 * At shutdown/restart power the chip down, as Linux ath12k does
	 * (compat/autoconf.c, qwzmac_qwz_shutdown()); before sleep power it
	 * down too and bring it up after wake. A priority sleep/wake
	 * interest gets these messages before the drivers' own power
	 * changes, without joining the power plane.
	 */
	fPowerNotifier = registerPrioritySleepWakeInterest(powerMessage, this);
	if (fPowerNotifier == nullptr)
		QLOG("no shutdown notification, DMA stays on at restart");

	/*
	 * HeliPort talks only to an IOEthernetController whose IOClass
	 * property is "itlwm" (ClientKit/Api.c, isSupportService()); it
	 * then opens our IOUserClientClass, QwzUserClient (kext/heliport.h).
	 * The property is informational once matching is done: IOKit
	 * identifies us by our meta class (ioreg still shows QwzWifi) and
	 * the bundle identifier.
	 */
	setProperty("IOClass", "itlwm");
	/*
	 * The network interface is published by netReady(), once the MAC is
	 * known, and the controller is registered there too.
	 */
	return true;
}

bool
QwzWifi::clientEnter()
{
	bool ok;

	IOLockLock(fIfLock);
	ok = !fStopping && fQwz != nullptr;
	if (ok)
		fClientCalls++;
	IOLockUnlock(fIfLock);
	return ok;
}

void
QwzWifi::clientExit()
{
	IOLockLock(fIfLock);
	if (--fClientCalls == 0)
		IOLockWakeup(fIfLock, &fClientCalls, false);
	IOLockUnlock(fIfLock);
}

void
QwzWifi::bsdName(char *buf, size_t len)
{
	IOEthernetInterface *netif;

	buf[0] = '\0';
	IOLockLock(fIfLock);
	netif = fNetIf;
	if (netif != nullptr && netif->getIfnet() != nullptr)
		snprintf(buf, len, "%s%u", ifnet_name(netif->getIfnet()),
		    ifnet_unit(netif->getIfnet()));
	IOLockUnlock(fIfLock);
}

IOReturn
QwzWifi::powerMessage(void *target, void *refCon, UInt32 messageType,
    IOService *provider, void *arg, vm_size_t argSize)
{
	QwzWifi *me = (QwzWifi *)target;

	(void)refCon; (void)provider; (void)arg; (void)argSize;
	switch (messageType) {
	case kIOMessageSystemWillPowerOff:
	case kIOMessageSystemWillRestart:
		QLOG("system %s: powering the chip down",
		    messageType == kIOMessageSystemWillRestart ?
		    "restart" : "power off");
		qwzmac_qwz_shutdown(me->fQwz);
		break;
	case kIOMessageSystemWillSleep:
		/* Blocks up to 10 s; priority clients get this before drivers sleep. */
		QLOG("system sleep: powering the chip down");
		qwzmac_qwz_sleep(me->fQwz);
		break;
	case kIOMessageSystemHasPoweredOn:
		QLOG("system wake: bringing the chip up");
		qwzmac_qwz_wake(me->fQwz);
		break;
	default:
		QLOG("power message 0x%x", (unsigned)messageType);
		break;
	}
	/* kIOReturnSuccess without a wait time acknowledges at once. */
	return kIOReturnSuccess;
}

/*
 * The name macOS gives the interface's network service ("Ethernet 2"
 * otherwise). configd (SCNetworkInterface.c) names a built-in Ethernet
 * interface "Ethernet"/"Ethernet N" whatever the driver says; for one that
 * is not built in it takes the "Product Name" property of the interface or
 * its providers, as "iPhone USB" gets its name from USB. So mark the
 * interface not built in and give it a product name, from QwzInterfaceName
 * in the personality (kext/Info.plist). Only a newly created service gets
 * it; an existing one keeps the name it was saved with.
 */
void
QwzWifi::nameInterface(IONetworkInterface *netif)
{
	OSString *name = OSDynamicCast(OSString, getProperty("QwzInterfaceName"));

	if (name == nullptr || name->getLength() == 0)
		return;
	netif->setProperty(kIOBuiltin, false);
	netif->setProperty("Product Name", name);
}

/* Bring-up thread, no spl lock: firmware is up, publish the interface. */
void
QwzWifi::netReady(void *cookie, const uint8_t mac[6])
{
	auto me = static_cast<QwzWifi *>(cookie);
	bcopy(mac, me->fMac, sizeof(me->fMac));
	if (!me->fNativePreferred) me->publishEthernet();
	else {
		me->setProperty("QwzDataOwner", "waiting for native front");
	}
	// Publish only after firmware and the MAC are ready; the nub matches here.
	me->registerService();
}

void QwzWifi::publishEthernet()
{
	IOEthernetInterface *netif = nullptr;

	IOLockLock(this->fIfLock);
	if (this->fStopping || this->fNetIf != nullptr ||
	    (fNativePreferred && fFrontOn)) {
		IOLockUnlock(this->fIfLock);
		return;
	}
	if (!this->attachInterface((IONetworkInterface **)&netif, false)) {
		IOLockUnlock(this->fIfLock);
		QLOG("cannot attach the network interface");
		return;
	}
	this->nameInterface(netif);
	netif->registerService();
	this->fNetIf = netif;
	IOLockUnlock(this->fIfLock);
	this->setLinkStatus(kIONetworkLinkValid);
	QLOG("network interface %s%u published", netif->getNamePrefix(),
	    netif->getUnitNumber());
	setProperty("QwzDataOwner", "Ethernet fallback");
	if (fNativePreferred) {
		qwzmac_network network = {};
		if (readNetwork(&network)) (void)qwzmac_qwz_join(fQwz, &network);
		bzero(&network, sizeof(network));
	}
}

/* Explicit one-way handover only. No timer and no automatic fallback. */
IOReturn QwzWifi::requestHeliportMode()
{
	IOLockLock(fIfLock);
	if (fStopping || fModeTc == nullptr) {
		IOLockUnlock(fIfLock); return kIOReturnNotReady;
	}
	if (!fNativePreferred && fNetIf != nullptr) {
		setProperty("QwzModeSwitch", "HeliPort");
		IOLockUnlock(fIfLock); return kIOReturnSuccess;
	}
	fHeliportRequested = true;
	setProperty("QwzModeSwitch", "requested");
	thread_call_enter(fModeTc);
	IOLockUnlock(fIfLock);
	return kIOReturnSuccess;
}

void QwzWifi::modeTc(thread_call_param_t p0, thread_call_param_t)
{
	auto me = static_cast<QwzWifi *>(p0);
	if (!me->clientEnter()) return;
	// Terminate the native provider, including its controller and interfaces.
	// No driver lock is held: stop() drains WCL workers and disconnects RX.
	auto children = me->getChildIterator(gIOServicePlane);
	bool ok = true;
	while (children) {
		auto child = OSDynamicCast(IOService, children->getNextObject());
		if (!child) break;
		if (!child->metaCast("QwzAirportNub")) continue;
		if (!child->terminate(kIOServiceRequired | kIOServiceSynchronous)) ok = false;
	}
	OSSafeReleaseNULL(children);
	IOLockLock(me->fIfLock);
	ok = ok && !me->fFrontOn && !me->fStopping;
	IOLockUnlock(me->fIfLock);
	if (ok) {
		(void)qwzmac_qwz_native_leave(me->fQwz);
		int error = qwzmac_qwz_set_mac(me->fQwz, me->fMac);
		if (!error) {
			IOLockLock(me->fIfLock);
			me->fNativePreferred = false;
			IOLockUnlock(me->fIfLock);
			me->publishEthernet();
		}
		else { me->setProperty("QwzModeSwitch", "MAC restore failed"); me->clientExit(); return; }
		IOLockLock(me->fIfLock);
		bool published = me->fNetIf != nullptr;
		IOLockUnlock(me->fIfLock);
		me->setProperty("QwzModeSwitch", published ? "HeliPort" : "Ethernet publication failed");
		if (published) QLOG("manual switch to HeliPort mode completed");
	} else me->setProperty("QwzModeSwitch", "native teardown failed; Ethernet withheld");
	me->clientExit();
}

/* qwz context, spl lock held (serializes all calls). */
void
QwzWifi::netInput(void *cookie, const void *frame, size_t len)
{
	QwzWifi *me = (QwzWifi *)cookie;
	IOLockLock(me->fIfLock);
	if (me->fNativePreferred) {
		if (!me->fStopping && me->fFrontOn && me->fFront.input != nullptr) {
			auto front = me->fFront;
			++me->fFrontCalls;
			IOLockUnlock(me->fIfLock);
			front.input(front.cookie, frame, len);
			IOLockLock(me->fIfLock);
			if (--me->fFrontCalls == 0)
				IOLockWakeup(me->fIfLock, &me->fFrontCalls, false);
			IOLockUnlock(me->fIfLock);
			return;
		}
	}
	IOEthernetInterface *netif = me->fNetIf;
	IOLockUnlock(me->fIfLock);
	mbuf_t m;

	if (netif == nullptr || len == 0 || len > QWZ_FRAME_MAX)
		return;
	m = me->allocatePacket((UInt32)len);
	if (m != nullptr && mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT) != 0) {
		me->freePacket(m);
		m = nullptr;
	}
	/*
	 * netstat's Ipkts/Ibytes are the ifnet's own counters, and
	 * inputPacket() without the input queue does not update them (Opkts
	 * the stack counts on output by itself). Seen with 0.0.4: ioreg
	 * showed inputPackets 42557, netstat Ipkts 0, Ibytes 0.
	 */
	if (m == nullptr) {
		me->countStat(&IONetworkStats::inputErrors);
		ifnet_stat_increment_in(netif->getIfnet(), 0, 0, 1);
		return;
	}
	me->countStat(&IONetworkStats::inputPackets);
	ifnet_stat_increment_in(netif->getIfnet(), 1, (u_int32_t)len, 0);
	netif->inputPacket(m, (UInt32)len);
}

/*
 * qwz context, spl lock held. setLinkStatus() talks to the network stack,
 * which may be calling outputPacket() and waiting for the spl lock, so
 * defer it to a thread.
 */
void
QwzWifi::netLink(void *cookie, int up, uint64_t baudrate)
{
	QwzWifi *me = (QwzWifi *)cookie;

	me->fLinkBaudrate = baudrate;
	me->fLinkSpeed = 0;	/* report the link again */
	__atomic_store_n(&me->fLinkUp, up ? 1 : 0, __ATOMIC_SEQ_CST);
	thread_call_enter(me->fLinkTc);
}

/*
 * Thread call: report link changes, and while the link is up, re-check the
 * TX rate every QWZ_RATE_POLL_MS so macOS shows the current speed (net80211
 * sets if_baudrate only once, from the first rate it picks).
 */
void
QwzWifi::linkTc(thread_call_param_t p0, thread_call_param_t p1)
{
	QwzWifi *me = (QwzWifi *)p0;
	uint64_t speed, deadline;

	(void)p1;
	if (!__atomic_load_n(&me->fLinkUp, __ATOMIC_SEQ_CST)) {
		me->fLinkSpeed = 0;
		me->setLinkStatus(kIONetworkLinkValid);
		return;
	}
	speed = qwzmac_qwz_tx_rate(me->fQwz);
	if (speed != 0 && speed != me->fLinkSpeed) {
		me->fLinkSpeed = speed;
		me->setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive,
		    me->fMedium, speed);
	} else if (me->fLinkSpeed == 0) {
		me->fLinkSpeed = me->fLinkBaudrate;
		me->setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive,
		    me->fMedium, me->fLinkBaudrate);
	}
	IOLockLock(me->fIfLock);
	if (!me->fStopping) {
		clock_interval_to_deadline(QWZ_RATE_POLL_MS, kMillisecondScale,
		    &deadline);
		thread_call_enter_delayed(me->fLinkTc, deadline);
	}
	IOLockUnlock(me->fIfLock);
}

/*
 * Counters are 32-bit and wrap, as in every IONetworking driver. Input runs
 * under the spl lock and output in the network stack's thread, so add
 * atomically.
 */
void
QwzWifi::countStat(UInt32 IONetworkStats::*field)
{
	if (fNetStats != nullptr)
		__atomic_fetch_add(&(fNetStats->*field), 1, __ATOMIC_RELAXED);
}

UInt32
QwzWifi::outputPacket(mbuf_t m, void *param)
{
	uint8_t frame[QWZ_FRAME_MAX];
	size_t len;
	int error;

	(void)param;
	if (m == nullptr)
		return kIOReturnOutputDropped;
	len = mbuf_pkthdr_len(m);
	if (len < 14 || len > sizeof(frame) ||
	    mbuf_copydata(m, 0, len, frame) != 0) {
		freePacket(m);
		countStat(&IONetworkStats::outputErrors);
		return kIOReturnOutputDropped;
	}
	freePacket(m);
	error = qwzmac_qwz_output(fQwz, frame, len);
	countStat(error == 0 ? &IONetworkStats::outputPackets :
	    &IONetworkStats::outputErrors);
	return error == 0 ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

/*
 * The interface was brought up/down by macOS. The radio follows its own
 * life: it joins the network at boot and stays associated, except across
 * sleep (powerMessage()).
 */
IOReturn
QwzWifi::enable(IONetworkInterface *netif)
{
	(void)netif;
	QLOG("interface enabled by macOS");
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::disable(IONetworkInterface *netif)
{
	(void)netif;
	QLOG("interface disabled by macOS");
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::getHardwareAddress(IOEthernetAddress *addr)
{
	bcopy(fMac, addr->bytes, sizeof(fMac));
	return kIOReturnSuccess;
}

/* qwz has no receive filter to program: it passes all multicast. */
IOReturn
QwzWifi::setPromiscuousMode(bool active)
{
	(void)active;
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::setMulticastMode(bool active)
{
	(void)active;
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
	(void)addrs; (void)count;
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::selectMedium(const IONetworkMedium *medium)
{
	setSelectedMedium(medium);
	return kIOReturnSuccess;
}

const OSString *
QwzWifi::newVendorString() const
{
	return OSString::withCString("Qualcomm");
}

const OSString *
QwzWifi::newModelString() const
{
	return OSString::withCString("WCN7850 Wi-Fi (qwz)");
}

/*
 * QwzAirport (kext/airport_api.h). It connects once its controller is
 * started and disconnects in its stop(). Its calls into qwz go through
 * clientEnter()/clientExit(), like QwzUserClient's, so stop() waits for
 * them; events go out from frontTc(), so qwz's lock is never held while the
 * front runs.
 */
IOReturn
QwzWifi::callPlatformFunction(const OSSymbol *name, bool wait, void *p1,
    void *p2, void *p3, void *p4)
{
	if (name != nullptr && name->isEqualTo(QWZ_AIRPORT_CONNECT))
		return frontConnect((const qwz_airport_front *)p1,
		    (qwz_airport_driver *)p2);
	if (name != nullptr && name->isEqualTo(QWZ_AIRPORT_DISCONNECT))
		return frontDisconnect(p1);
	return IOEthernetController::callPlatformFunction(name, wait, p1, p2,
	    p3, p4);
}

IOReturn
QwzWifi::frontConnect(const qwz_airport_front *front, qwz_airport_driver *driver)
{
	if (front == nullptr || driver == nullptr || front->event == nullptr ||
	    front->version != QWZ_AIRPORT_API_VERSION ||
	    front->size != sizeof(*front) ||
	    driver->version != QWZ_AIRPORT_API_VERSION ||
	    driver->size != sizeof(*driver)) {
		QLOG("airport: connect refused: API version %u, ours %u",
		    front != nullptr ? (unsigned)front->version : 0,
		    (unsigned)QWZ_AIRPORT_API_VERSION);
		return kIOReturnBadArgument;
	}
	if (fQwz == nullptr || fFrontTc == nullptr)
		return kIOReturnNotReady;	/* -qwzprobe */
	IOLockLock(fIfLock);
	if (fStopping || fHeliportRequested || fFrontOn || (fNativePreferred && fNetIf != nullptr)) {
		IOLockUnlock(fIfLock);
		return fStopping ? kIOReturnNotReady : kIOReturnBusy;
	}
	fFront = *front;
	fFrontOn = true;
	IOLockUnlock(fIfLock);

	driver->dev = this;
	driver->power = dvPower;
	driver->set_power = dvSetPower;
	driver->hw_address = dvHwAddress;
	driver->fw_version = dvFwVersion;
	driver->country = dvCountry;
	driver->channels = dvChannels;
	driver->sta_info = dvStaInfo;
	driver->scan_start = dvScanStart;
	driver->scan_poll = dvScanPoll;
	driver->scan_snapshot = dvScanSnapshot;
	driver->data_owner = dvDataOwner;
	driver->set_mac = dvSetMac;
	driver->join = dvJoin;
	driver->leave = dvLeave;
	driver->join_status = dvJoinStatus;
	driver->output = dvOutput;
	setProperty("QwzDataOwner", fNativePreferred ? "native front" : "Ethernet");
	QLOG("airport: native Wi-Fi front connected");
	/* Tell it where the radio is now; later changes come from netRadio(). */
	frontQueue(qwzmac_qwz_power(fQwz) ? QWZMAC_RADIO_UP : QWZMAC_RADIO_DOWN);
	return kIOReturnSuccess;
}

IOReturn
QwzWifi::frontDisconnect(void *cookie)
{
	IOLockLock(fIfLock);
	if (!fFrontOn || fFront.cookie != cookie) {
		IOLockUnlock(fIfLock);
		return kIOReturnNotOpen;
	}
	fFrontOn = false;
	while (fFrontCalls > 0)
		IOLockSleep(fIfLock, &fFrontCalls, THREAD_UNINT);
	bzero(&fFront, sizeof(fFront));
	IOLockUnlock(fIfLock);
	QLOG("airport: native Wi-Fi front disconnected");
	return kIOReturnSuccess;
}

/* Any context, also under qwz's lock: queue a radio state for the front. */
void
QwzWifi::frontQueue(int state)
{
	IOSimpleLockLock(fRadioLock);
	if (fRadioTail - fRadioHead == QWZ_RADIO_QLEN)
		fRadioHead++;	/* full: drop the oldest */
	fRadioQ[fRadioTail++ % QWZ_RADIO_QLEN] = state;
	IOSimpleLockUnlock(fRadioLock);
	thread_call_enter(fFrontTc);
}

/* qwz context, spl lock held. */
void
QwzWifi::netRadio(void *cookie, int state)
{
	QwzWifi *me = (QwzWifi *)cookie;

	me->frontQueue(state);
}

/* Thread call: hand queued radio states to the front, in order. */
void
QwzWifi::frontTc(thread_call_param_t p0, thread_call_param_t p1)
{
	QwzWifi *me = (QwzWifi *)p0;
	qwz_airport_front front;
	int state;

	(void)p1;
	for (;;) {
		IOSimpleLockLock(me->fRadioLock);
		if (me->fRadioHead == me->fRadioTail) {
			IOSimpleLockUnlock(me->fRadioLock);
			return;
		}
		state = me->fRadioQ[me->fRadioHead++ % QWZ_RADIO_QLEN];
		IOSimpleLockUnlock(me->fRadioLock);

		IOLockLock(me->fIfLock);
		if (!me->fFrontOn || me->fStopping) {
			IOLockUnlock(me->fIfLock);
			continue;
		}
		front = me->fFront;
		me->fFrontCalls++;
		IOLockUnlock(me->fIfLock);
		front.event(front.cookie, QWZ_AIRPORT_EV_RADIO, &state,
		    sizeof(state));
		IOLockLock(me->fIfLock);
		if (--me->fFrontCalls == 0)
			IOLockWakeup(me->fIfLock, &me->fFrontCalls, false);
		IOLockUnlock(me->fIfLock);
	}
}

int
QwzWifi::dvPower(void *dev)
{
	QwzWifi *me = (QwzWifi *)dev;
	int on;

	if (!me->clientEnter())
		return 0;
	on = qwzmac_qwz_power(me->fQwz);
	me->clientExit();
	return on;
}

int
QwzWifi::dvSetPower(void *dev, int on)
{
	QwzWifi *me = (QwzWifi *)dev;
	int error;

	if (!me->clientEnter())
		return ENXIO;
	if (!on && dvDataOwner(dev)) (void)qwzmac_qwz_native_leave(me->fQwz);
	error = qwzmac_qwz_set_power(me->fQwz, on);
	me->clientExit();
	return error;
}

int
QwzWifi::dvHwAddress(void *dev, uint8_t mac[6])
{
	QwzWifi *me = (QwzWifi *)dev;
	uint8_t any = 0;

	for (unsigned i = 0; i < sizeof(me->fMac); i++)
		any |= me->fMac[i];
	if (!any)
		return ENXIO;	/* before netReady() */
	bcopy(me->fMac, mac, sizeof(me->fMac));
	return 0;
}

void
QwzWifi::dvFwVersion(void *dev, char *buf, size_t len)
{
	QwzWifi *me = (QwzWifi *)dev;

	if (len > 0)
		buf[0] = '\0';
	if (!me->clientEnter())
		return;
	qwzmac_qwz_fw_version(me->fQwz, buf, len);
	me->clientExit();
}

int
QwzWifi::dvCountry(void *dev, char cc[3])
{
	QwzWifi *me = (QwzWifi *)dev;
	int error;

	cc[0] = '\0';
	if (!me->clientEnter())
		return ENXIO;
	error = qwzmac_qwz_country(me->fQwz, cc);
	me->clientExit();
	return error;
}

int
QwzWifi::dvChannels(void *dev, qwzmac_chan *out, int max)
{
	QwzWifi *me = (QwzWifi *)dev;
	int n;

	if (!me->clientEnter())
		return 0;
	n = qwzmac_qwz_channels(me->fQwz, out, max);
	me->clientExit();
	return n;
}

int
QwzWifi::dvStaInfo(void *dev, qwzmac_sta_info *si)
{
	QwzWifi *me = (QwzWifi *)dev;
	int error;

	if (!me->clientEnter()) {
		bzero(si, sizeof(*si));
		return ENXIO;
	}
	error = qwzmac_qwz_sta_info(me->fQwz, si);
	me->clientExit();
	return error;
}

void
QwzWifi::stop(IOService *provider)
{
	IOEthernetInterface *netif;

	QLOG("stop");
	if (fPowerNotifier != nullptr) {
		fPowerNotifier->remove();
		fPowerNotifier = nullptr;
	}
	/* No more netInput()/netLink(), then no more outputPacket(). */
	qwzmac_qwz_disconnect(fQwz);
	IOLockLock(fIfLock);
	fStopping = true;
	while (fClientCalls > 0)	/* QwzUserClient calls into qwz */
		IOLockSleep(fIfLock, &fClientCalls, THREAD_UNINT);
	netif = fNetIf;
	fNetIf = nullptr;
	IOLockUnlock(fIfLock);
	if (fModeTc != nullptr) {
		while (thread_call_isactive(fModeTc)) {
			thread_call_cancel(fModeTc);
			IOSleep(5);
		}
		thread_call_free(fModeTc);
		fModeTc = nullptr;
	}
	/* linkTc() calls into qwz: wait for it (fStopping stops re-arming). */
	if (fLinkTc != nullptr) {
		while (thread_call_isactive(fLinkTc)) {
			thread_call_cancel(fLinkTc);
			IOSleep(5);
		}
	}
	/*
	 * QwzAirport is our client, so IOKit stopped it (and it disconnected)
	 * before this; drop it anyway, then wait for frontTc(). No netRadio()
	 * any more since qwzmac_qwz_disconnect().
	 */
	IOLockLock(fIfLock);
	fFrontOn = false;
	while (fFrontCalls > 0)
		IOLockSleep(fIfLock, &fFrontCalls, THREAD_UNINT);
	IOLockUnlock(fIfLock);
	if (fFrontTc != nullptr) {
		while (thread_call_isactive(fFrontTc)) {
			thread_call_cancel(fFrontTc);
			IOSleep(5);
		}
		thread_call_free(fFrontTc);
		fFrontTc = nullptr;
	}
	if (netif != nullptr) {
		detachInterface(netif, true);
		netif->release();
	}
	if (fQwz != nullptr) {
		qwzmac_qwz_detach(fQwz);
		fQwz = nullptr;
	}
	if (fLinkTc != nullptr) {
		thread_call_free(fLinkTc);
		fLinkTc = nullptr;
	}
	if (fOsInit) {
		qwzmac_os_fini();
		fOsInit = false;
	}
	if (fPci != nullptr) {
		fPci->close(this);
		fPci = nullptr;
	}
	if (fMedium != nullptr) {
		fMedium->release();
		fMedium = nullptr;
	}
	qwzmac_log_stop();
	IOEthernetController::stop(provider);
	if (fWorkLoop != nullptr) {
		fWorkLoop->release();
		fWorkLoop = nullptr;
	}
	IOLockFree(fIfLock);
	fIfLock = nullptr;
	IOSimpleLockFree(fRadioLock);
	fRadioLock = nullptr;
}

int QwzWifi::dvScanStart(void *dev, const qwzmac_scan_request *request, uint32_t *generation)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_scan_start(me->fQwz, request, generation);
	me->clientExit();
	return error;
}
int QwzWifi::dvScanPoll(void *dev, uint32_t generation)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_scan_poll(me->fQwz, generation);
	me->clientExit();
	return error;
}
int QwzWifi::dvScanSnapshot(void *dev, uint32_t ticket, qwzmac_scan_bss *out, int max)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!me->clientEnter()) return 0;
	int n = qwzmac_qwz_scan_snapshot(me->fQwz, ticket, out, max);
	me->clientExit();
	return n;
}

int QwzWifi::dvSetMac(void *dev, const uint8_t mac[6])
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!dvDataOwner(dev) || !me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_native_leave(me->fQwz);
	if (!error) error = qwzmac_qwz_set_mac(me->fQwz, mac);
	me->clientExit();
	return error;
}
int QwzWifi::dvDataOwner(void *dev)
{
	auto me = static_cast<QwzWifi *>(dev);
	IOLockLock(me->fIfLock);
	int owner = me->fNativePreferred && me->fFrontOn && me->fNetIf == nullptr;
	IOLockUnlock(me->fIfLock);
	return owner;
}
int QwzWifi::dvJoin(void *dev, qwzmac_native_join *request)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!dvDataOwner(dev) || !me->clientEnter()) {
		bzero(request->pmk, sizeof(request->pmk)); return ENXIO;
	}
	int error = qwzmac_qwz_native_join(me->fQwz, request);
	me->clientExit(); return error;
}
int QwzWifi::dvLeave(void *dev)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!dvDataOwner(dev) || !me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_native_leave(me->fQwz);
	me->clientExit(); return error;
}
int QwzWifi::dvJoinStatus(void *dev, qwzmac_native_status *status)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_native_status(me->fQwz, status);
	me->clientExit(); return error;
}
int QwzWifi::dvOutput(void *dev, const void *frame, size_t len)
{
	auto me = static_cast<QwzWifi *>(dev);
	if (!dvDataOwner(dev) || !me->clientEnter()) return ENXIO;
	int error = qwzmac_qwz_output(me->fQwz, frame, len);
	me->clientExit(); return error;
}
