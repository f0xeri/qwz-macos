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

#define QLOG(fmt, ...) qwzmac_log("QwzWifi: " fmt "\n", ##__VA_ARGS__)

#define QWZ_VERSION	"0.0.6"

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
	int		 fClientCalls;	/* fIfLock; QwzUserClient calls in progress */
};

#endif /* QWZ_QWZWIFI_H */
