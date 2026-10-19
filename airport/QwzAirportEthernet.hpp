/*
 * QwzAirportEthernet: the BSD interface (enN) that carries QwzAirport's
 * data. WCL drives the Skywalk interface (QwzAirportInterface), which is
 * created with deferBSDAttach(); the frames go through this plain
 * IOEthernetInterface of the same controller.
 *
 * Based on AirportItlwmEthernetInterface of laobamac/itlwm 738d4c6
 * (AirportItlwm/AirportItlwmEthernetInterface.cpp), largely unchanged
 * (airport/CREDITS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_AIRPORT_ETHERNET_HPP
#define QWZ_AIRPORT_ETHERNET_HPP

#include <Airport/Apple80211.h>
#include <IOKit/network/IOEthernetInterface.h>

class QwzAirportEthernet : public IOEthernetInterface {
	OSDeclareDefaultStructors(QwzAirportEthernet)
public:
	bool initWithSkywalkInterface(IONetworkController *controller,
	    IO80211SkywalkInterface *interface);
	IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) override;
	void detachFromDataLinkLayer(IOOptionBits options, void *parameter) override;
	UInt32 inputPacket(mbuf_t packet, UInt32 length = 0, IOOptionBits options = 0,
	    void *param = 0) override;
	/*
	 * Once attached, report the Skywalk interface as the provider:
	 * IOSkywalkFamily casts this interface's provider to
	 * IOSkywalkNetworkInterface and panics otherwise (laobamac's finding).
	 */
	IOService *getProvider() const override;
	bool setLinkState(IO80211LinkState state);
	bool initIfnetParams(struct ifnet_init_params *) override;

private:
	static IOReturn prepareBSDGated(OSObject *, void *, void *, void *, void *);

	IO80211SkywalkInterface	*fInterface;
	bool			 fAttached;
	uint8_t fUniqueId[16];
};

#endif /* QWZ_AIRPORT_ETHERNET_HPP */
