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
 *
 * Boot-args:
 *   -qwzon     required, otherwise the driver does not attach at all
 *   -qwzprobe  stage 0 only: log IDs/BAR/MSI and leave the device untouched
 *   qwzdebug=N qwz debug mask (QWZ_D_* in qwzvar.h), default 0 (errors only).
 *              0x7 = MISC|MHI|QMI; slows bring-up, qwz's 1 s QMI timeouts may fire
 *
 * The network comes from the "QwzNetwork" dictionary (SSID, Password) in
 * the personality, which scripts/build.sh copies from config/wifi.plist.
 * Without it the driver only scans.
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

#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

/* 802.11 MTU (2304) plus the Ethernet header, as in compat/autoconf.c. */
#define QWZ_FRAME_MAX	(2304 + 14)

/* How often the link speed shown by macOS follows the firmware's TX rate. */
#define QWZ_RATE_POLL_MS	2000

extern "C" uint32_t qwz_debug;	/* qwz/qwz.c, built with QWZ_DEBUG */

class QwzWifi : public IOEthernetController {
	OSDeclareDefaultStructors(QwzWifi)
public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

	bool createWorkLoop() override;
	bool configureInterface(IONetworkInterface *netif) override;
	IOWorkLoop *getWorkLoop() const override;
	IOReturn enable(IONetworkInterface *netif) override;
	IOReturn disable(IONetworkInterface *netif) override;
	IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
	UInt32 outputPacket(mbuf_t m, void *param) override;
	IOReturn setPromiscuousMode(bool active) override;
	IOReturn setMulticastMode(bool active) override;
	IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
	IOReturn selectMedium(const IONetworkMedium *medium) override;
	const OSString *newVendorString() const override;
	const OSString *newModelString() const override;

private:
	void logDevice(IOPCIDevice *pci);
	bool readNetwork(qwzmac_network *net);
	bool createMedium();
	static IOReturn powerMessage(void *target, void *refCon,
	    UInt32 messageType, IOService *provider, void *arg,
	    vm_size_t argSize);
	static void netReady(void *cookie, const uint8_t mac[6]);
	static void netInput(void *cookie, const void *frame, size_t len);
	static void netLink(void *cookie, int up, uint64_t baudrate);
	static void linkTc(thread_call_param_t p0, thread_call_param_t p1);
	void countStat(UInt32 IONetworkStats::*field);

	IOPCIDevice	*fPci;
	void		*fQwz;
	bool		 fOsInit;
	IONotifier	*fPowerNotifier;
	IOWorkLoop	*fWorkLoop;

	IOLock		*fIfLock;	/* fNetIf vs. stop() */
	IOEthernetInterface *fNetIf;
	bool		 fStopping;
	IONetworkMedium	*fMedium;	/* retained */
	uint8_t		 fMac[6];

	thread_call_t	 fLinkTc;	/* setLinkStatus() outside the spl lock */
	volatile int	 fLinkUp;
	volatile uint64_t fLinkSpeed;	/* last speed given to setLinkStatus() */
	volatile uint64_t fLinkBaudrate; /* net80211's if_baudrate at link up */
	IONetworkStats	*fNetStats;	/* netstat's packet and error counters */
};

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
	fMedium = nullptr;
	fLinkTc = nullptr;
	fLinkUp = 0;
	fLinkSpeed = 0;
	fLinkBaudrate = 0;
	fNetStats = nullptr;
	bzero(fMac, sizeof(fMac));
	fIfLock = IOLockAlloc();
	if (fIfLock == nullptr)
		return false;

	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) {
		QLOG("provider is not IOPCIDevice");
		return false;
	}
	qwzmac_log_start(pci);
	QLOG("start, version 0.0.5 (built " __DATE__ " " __TIME__ ")");

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
	if (fLinkTc == nullptr) {
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

	bool have_net = readNetwork(&net);
	bzero(&ops, sizeof(ops));
	ops.cookie = this;
	ops.ready = netReady;
	ops.input = netInput;
	ops.link = netLink;

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

	/* The network interface is published by netReady(), once the MAC is known. */
	return true;
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

/* Bring-up thread, no spl lock: firmware is up, publish the interface. */
void
QwzWifi::netReady(void *cookie, const uint8_t mac[6])
{
	QwzWifi *me = (QwzWifi *)cookie;
	IOEthernetInterface *netif = nullptr;

	IOLockLock(me->fIfLock);
	if (me->fStopping || me->fNetIf != nullptr) {
		IOLockUnlock(me->fIfLock);
		return;
	}
	bcopy(mac, me->fMac, sizeof(me->fMac));
	if (!me->attachInterface((IONetworkInterface **)&netif, true)) {
		IOLockUnlock(me->fIfLock);
		QLOG("cannot attach the network interface");
		return;
	}
	me->fNetIf = netif;
	IOLockUnlock(me->fIfLock);
	me->setLinkStatus(kIONetworkLinkValid);
	QLOG("network interface %s%u published", netif->getNamePrefix(),
	    netif->getUnitNumber());
}

/* qwz context, spl lock held (serializes all calls). */
void
QwzWifi::netInput(void *cookie, const void *frame, size_t len)
{
	QwzWifi *me = (QwzWifi *)cookie;
	IOEthernetInterface *netif = me->fNetIf;
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
	netif = fNetIf;
	fNetIf = nullptr;
	IOLockUnlock(fIfLock);
	/* linkTc() calls into qwz: wait for it (fStopping stops re-arming). */
	if (fLinkTc != nullptr) {
		while (thread_call_isactive(fLinkTc)) {
			thread_call_cancel(fLinkTc);
			IOSleep(5);
		}
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
}
