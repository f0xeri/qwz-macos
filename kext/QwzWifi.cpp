/*
 * QwzWifi — Qualcomm WCN7850 (17cb:1107) driver for macOS, port of OpenBSD qwz(4).
 *
 * Stage 1: run qwz's attach unchanged (compat/autoconf.c) up to "firmware
 * ready" and the MAC address.
 * Stage 2: bring the interface up in scan-only mode; net80211 logs the
 * networks it finds. No macOS network interface is published yet.
 *
 * Boot-args:
 *   -qwzon     required, otherwise the driver does not attach at all
 *   -qwzprobe  stage 0 only: log IDs/BAR/MSI and leave the device untouched
 *   qwzdebug=N qwz debug mask (QWZ_D_* in qwzvar.h), default 0 (errors only).
 *              0x7 = MISC|MHI|QMI; slows bring-up, qwz's 1 s QMI timeouts may fire
 *
 * SPDX-License-Identifier: GPL-2.0-only (qwz parts: ISC)
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <pexpert/pexpert.h>

#include "os_bridge.h"
#include "../compat/include/qwzmac/os.h"
#include "../compat/include/qwzmac/attach.h"

#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

extern "C" uint32_t qwz_debug;	/* qwz/qwz.c, built with QWZ_DEBUG */

class QwzWifi : public IOService {
	OSDeclareDefaultStructors(QwzWifi)
public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

private:
	void logDevice(IOPCIDevice *pci);
	static IOReturn powerMessage(void *target, void *refCon,
	    UInt32 messageType, IOService *provider, void *arg,
	    vm_size_t argSize);

	IOPCIDevice	*fPci;
	void		*fQwz;
	bool		 fOsInit;
	IONotifier	*fPowerNotifier;
};

OSDefineMetaClassAndStructors(QwzWifi, IOService)

IOService *
QwzWifi::probe(IOService *provider, SInt32 *score)
{
	char tmp[16];

	if (!PE_parse_boot_argn("-qwzon", tmp, sizeof(tmp))) {
		QLOG("boot-arg -qwzon not set, not attaching");
		return nullptr;
	}
	return IOService::probe(provider, score);
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

bool
QwzWifi::start(IOService *provider)
{
	char tmp[16];
	int error;

	fPci = nullptr;
	fQwz = nullptr;
	fOsInit = false;
	fPowerNotifier = nullptr;

	if (!IOService::start(provider))
		return false;

	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) {
		QLOG("provider is not IOPCIDevice");
		return false;
	}
	qwzmac_log_start(pci);
	QLOG("start, version 0.0.1 (built " __DATE__ " " __TIME__ ")");

	logDevice(pci);

	if (PE_parse_boot_argn("-qwzprobe", tmp, sizeof(tmp))) {
		QLOG("-qwzprobe: stage 0 only, device left untouched");
		qwzmac_log_publish();
		registerService();
		return true;
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
	QLOG("stage 1: attaching qwz (debug mask 0x%x)", (unsigned)mask);
	error = qwzmac_qwz_attach(pci, &fQwz);
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
	 * From stage 2 on the chip is running and writes into host RAM.
	 * At shutdown/restart turn its bus mastering off. A priority
	 * sleep/wake interest gets these messages without joining the power
	 * plane (which would also make us handle sleep).
	 */
	fPowerNotifier = registerPrioritySleepWakeInterest(powerMessage, this);
	if (fPowerNotifier == nullptr)
		QLOG("no shutdown notification, DMA stays on at restart");

	registerService();
	return true;
}

IOReturn
QwzWifi::powerMessage(void *target, void *refCon, UInt32 messageType,
    IOService *provider, void *arg, vm_size_t argSize)
{
	QwzWifi *me = (QwzWifi *)target;

	(void)refCon; (void)provider; (void)arg; (void)argSize;
	if (messageType == kIOMessageSystemWillPowerOff ||
	    messageType == kIOMessageSystemWillRestart) {
		QLOG("system %s: stopping device DMA",
		    messageType == kIOMessageSystemWillRestart ?
		    "restart" : "power off");
		qwzmac_qwz_shutdown(me->fQwz);
	}
	/* kIOReturnSuccess without a wait time acknowledges at once. */
	return kIOReturnSuccess;
}

void
QwzWifi::stop(IOService *provider)
{
	QLOG("stop");
	if (fPowerNotifier != nullptr) {
		fPowerNotifier->remove();
		fPowerNotifier = nullptr;
	}
	if (fQwz != nullptr) {
		qwzmac_qwz_detach(fQwz);
		fQwz = nullptr;
	}
	if (fOsInit) {
		qwzmac_os_fini();
		fOsInit = false;
	}
	if (fPci != nullptr) {
		fPci->close(this);
		fPci = nullptr;
	}
	qwzmac_log_stop();
	IOService::stop(provider);
}
