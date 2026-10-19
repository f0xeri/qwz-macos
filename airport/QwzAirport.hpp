/*
 * QwzAirport: the native Wi-Fi front of QwzWifi (docs/native-wifi-research.md
 * §8.5, PLAN.md "Этап C2"). An IO80211Controller on top of the QwzWifi
 * service: macOS's Wi-Fi stack (WCL) talks to it, and it talks to QwzWifi
 * only through kext/airport_api.h. No OpenBSD code and no hardware access
 * here: everything that touches the chip or net80211 is in QwzWifi.
 *
 * Based on AirportItlwmV2 of laobamac/itlwm 738d4c6 (AirportItlwm/
 * AirportItlwmV2.cpp, .hpp; itself AirportItlwm by zxystd, OpenIntelWireless):
 * work queue, CoreCapture pipes and fault reporter, interface setup. The
 * PCI, power management and itlwm HAL parts are gone (QwzWifi owns the
 * device); requests go to QwzWifi instead of net80211 (airport/CREDITS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_AIRPORT_HPP
#define QWZ_AIRPORT_HPP

#include <Airport/Apple80211.h>
#include <Airport/macOS26/FaultReporter.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/network/IOEthernetController.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <stdarg.h>

#include "../kext/airport_api.h"

/* IOLog reaches the boot console; os_log preserves the same line for log show. */
static inline void qwz_airport_log(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
static inline void qwz_airport_log(const char *format, ...)
{
	char text[384];
	va_list ap;
	va_start(ap, format);
	vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	IOLog("QwzAirport: %s\n", text);
	os_log(OS_LOG_DEFAULT, "QwzAirport: %{public}s", text);
}
#define ALOG(...) qwz_airport_log(__VA_ARGS__)

#define QWZ_AIRPORT_VERSION	"0.3.6"

class QwzAirportInterface;
class QwzAirportEthernet;

class QwzAirport : public IO80211Controller {
	OSDeclareDefaultStructors(QwzAirport)
public:
	/* IOService */
	bool init(OSDictionary *properties) override;
	void free() override;
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

	/* IONetworkController / IOEthernetController */
	IOWorkLoop *getWorkLoop() const override;
	bool createWorkLoop() override;
	IONetworkInterface *createInterface() override;
	bool configureInterface(IONetworkInterface *netif) override;
	UInt32 outputPacket(mbuf_t m, void *param) override;
	IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
	IOReturn setHardwareAddress(const void *addr, UInt32 len) override;
	IOReturn setPromiscuousMode(bool active) override;
	IOReturn setMulticastMode(bool active) override;
	IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
	IOReturn selectMedium(const IONetworkMedium *medium) override;
	UInt32 getFeatures() const override;
	const OSString *newVendorString() const override;
	const OSString *newModelString() const override;

	/* IO80211Controller */
	bool createWorkQueue() override;
	IO80211WorkQueue *getWorkQueue() const override;
	CCLogStream *getLogger() const override { return fLogger; }
	bool getLogPipes(CCPipe **log, CCPipe **events, CCPipe **snapshots) override;
	CommonFaultReporter *getFaultReporterFromDriver() override;
	bool requiresExplicitMBufRelease() override { return false; }
	bool flowIdSupported() override { return false; }
	SInt32 monitorModeSetEnabled(bool, unsigned int) override { return kIOReturnSuccess; }
	IOReturn requestQueueSizeAndTimeout(unsigned short *, unsigned short *) override
	    { return kIOReturnSuccess; }
	SInt32 enableFeature(IO80211FeatureCode code, void *data) override;
	bool isCommandProhibited(int) override { return false; }
	SInt32 handleCardSpecific(IO80211SkywalkInterface *, unsigned long, void *, bool) override
	    { return 0; }
	IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *) override;
	IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *) override;
	IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *) override;
	IOReturn getPOWER(IO80211SkywalkInterface *, apple80211_power_data *) override;
	IOReturn setPOWER(IO80211SkywalkInterface *, apple80211_power_data *) override;
	IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *) override;
	IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *) override;
	IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *, apple80211_debug_command *) override
	    { return kIOReturnSuccess; }

	/* For QwzAirportInterface */
	const qwz_airport_driver *driver() const { return &fDrv; }
	IOCommandGate *gate() const { return fGate; }
	bool radioOn() const { return fPowerState != 0; }
	void bsdAttached();
	bool nativeDataMode() const { return fNativeData; }
	void nativeLink(bool up, uint64_t rate);
	static void frontInput(void *, const void *, size_t);
	void bsdDetached();
	void macAddress(uint8_t mac[6]) const { memcpy(mac, fMac, sizeof(fMac)); }

private:
	void trace(const char *stage);
	void stopAsync();
	static int park(void *);
	static IOReturn parkGated(OSObject *, void *, void *, void *, void *);
	IOLock *fAsyncLock;
	void scheduleRadio();
	static void radioTc(thread_call_param_t, thread_call_param_t);
	bool initCCLogs();
	void releaseAll();
	bool createMediumTables();
	static void frontEvent(void *cookie, int event, const void *data, size_t len);
	static IOReturn radioGated(OSObject *owner, void *a0, void *, void *, void *);
	static void powerTc(thread_call_param_t p0, thread_call_param_t p1);

	IOService		*fQwz;		/* provider: QwzWifi */
	qwz_airport_driver	 fDrv;
	qwz_airport_front	 fFront;
	bool			 fConnected;
	bool fNativeData;
	volatile unsigned fNativeLinkUp;
	uint8_t			 fMac[6];

	IO80211WorkQueue	*fWorkQueue;
	IOCommandGate		*fGate;
	QwzAirportInterface	*fNetIf;	/* the Wi-Fi interface WCL uses */
	QwzAirportEthernet	*fBsdIf;	/* its BSD (data) interface */

	uint8_t			 fPowerState;	/* what WCL set (setPOWER) */
	uint32_t fPowerSerial;
	volatile int		 fPowerWanted;	/* for powerTc() */
	thread_call_t		 fPowerTc;
	thread_call_t		 fRadioTc;
	/* Atomic rendezvous between start(), BSD attachment and radio events. */
	enum { Started = 1, BSDAttached = 2, Stopping = 4 };
	IOLock			*fLifecycleLock;
	unsigned		 fLifecycle;
	int			 fRadioState;

	CCPipe			*fLogPipe;
	CCPipe			*fDataPathPipe;
	CCPipe			*fSnapshotsPipe;
	bool			 fLogPipeStarted;
	bool			 fDataPathPipeStarted;
	bool			 fSnapshotsPipeStarted;
	CCLogStream		*fLogger;
	CCStream		*fFaultStream;
	CCFaultReporter		*fCoreFaultReporter;
	IO80211FaultReporter	*fFaultReporter;
};

#endif /* QWZ_AIRPORT_HPP */
