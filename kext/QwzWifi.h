/*
 * QwzWifi: the IOEthernetController (kext/QwzWifi.cpp).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_QWZWIFI_H
#define QWZ_QWZWIFI_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IONetworkStats.h>
#include <kern/thread_call.h>

#include "../compat/include/qwzmac/os.h"
#include "../compat/include/qwzmac/attach.h"
#include "airport_api.h"

#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

#define QWZ_VERSION	"0.0.20"

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
	/* QwzAirport connects through this (kext/airport_api.h). */
	using IOService::callPlatformFunction;
	IOReturn callPlatformFunction(const OSSymbol *name, bool wait,
	    void *p1, void *p2, void *p3, void *p4) override;

	/*
	 * For QwzUserClient: calls into qwz go between clientEnter() and
	 * clientExit(); stop() waits for them. clientEnter() fails once
	 * stopping.
	 */
	bool clientEnter();
	void clientExit();
	void *qwzDev() const { return fQwz; }
	/* "en4", or "" before the interface is published. */
	void bsdName(char *buf, size_t len);

	IOReturn requestHeliportMode();
	static void modeTc(thread_call_param_t, thread_call_param_t);
	bool nativeDataMode() const {
		IOLockLock(fIfLock);
		bool native = (fNativePreferred || fHeliportRequested) && (fNetIf == nullptr || fNetIf->getIfnet() == nullptr);
		IOLockUnlock(fIfLock);
		return native;
	}

private:
	void logDevice(IOPCIDevice *pci);
	void nameInterface(IONetworkInterface *netif);
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
	/* QwzAirport (kext/airport_api.h) */
	IOReturn frontConnect(const qwz_airport_front *front,
	    qwz_airport_driver *driver);
	IOReturn frontDisconnect(void *cookie);
	void frontQueue(int state);
	static void netRadio(void *cookie, int state);
	static void netBeacon(void *cookie);
	static void frontTc(thread_call_param_t p0, thread_call_param_t p1);
	static int dvPower(void *dev);
	static int dvSetPower(void *dev, int on);
	static int dvHwAddress(void *dev, uint8_t mac[6]);
	static void dvFwVersion(void *dev, char *buf, size_t len);
	static int dvCountry(void *dev, char cc[3]);
	static int dvChannels(void *dev, qwzmac_chan *out, int max);
	static int dvStaInfo(void *dev, qwzmac_sta_info *si);
	static int dvScanStart(void *, const qwzmac_scan_request *, uint32_t *);
	static int dvScanPoll(void *, uint32_t);
	static int dvScanSnapshot(void *, uint32_t, qwzmac_scan_bss *, int);
	static int dvSetMac(void *, const uint8_t[6]);
	static int dvDataOwner(void *);
	static int dvJoin(void *, qwzmac_native_join *);
	static int dvLeave(void *);
	static int dvJoinStatus(void *, qwzmac_native_status *);
	static int dvOutput(void *, const void *, size_t);
	void publishEthernet();

	IOPCIDevice	*fPci;
	void		*fQwz;
	bool		 fOsInit;
	IONotifier	*fPowerNotifier;
	IOWorkLoop	*fWorkLoop;

	IOLock		*fIfLock;	/* fNetIf vs. stop() */
	IOEthernetInterface *fNetIf;
	bool		 fStopping;
	bool fNativePreferred;
	bool fHeliportRequested;
	thread_call_t fModeTc;
	IONetworkMedium	*fMedium;	/* retained */
	uint8_t		 fMac[6];

	thread_call_t	 fLinkTc;	/* setLinkStatus() outside the spl lock */
	volatile int	 fLinkUp;
	volatile uint64_t fLinkSpeed;	/* last speed given to setLinkStatus() */
	volatile uint64_t fLinkBaudrate; /* net80211's if_baudrate at link up */
	IONetworkStats	*fNetStats;	/* netstat's packet and error counters */
	int		 fClientCalls;	/* fIfLock; QwzUserClient calls in progress */

	/* QwzAirport: the front, under fIfLock; events go out in frontTc(). */
	qwz_airport_front fFront;
	bool		 fFrontOn;
	int		 fFrontCalls;	/* fIfLock; front->event() calls running */
	thread_call_t	 fFrontTc;
	IOSimpleLock	*fRadioLock;	/* the queue below; taken under qwz's lock */
#define QWZ_RADIO_QLEN	8
	int		 fRadioQ[QWZ_RADIO_QLEN];
	unsigned	 fRadioHead, fRadioTail;
	bool fBeaconPending; /* fRadioLock; coalesce RX signals, never radio states */
};

#endif /* QWZ_QWZWIFI_H */
