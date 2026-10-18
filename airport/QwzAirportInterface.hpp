/*
 * QwzAirportInterface: the Wi-Fi interface macOS's Wi-Fi stack (WCL) talks
 * to (an IO80211InfraProtocol). Stage C2: power, capabilities, channels,
 * state and the driver availability messages; scan (C3) and join (C4) are
 * not there yet.
 *
 * Based on AirportItlwmSkywalkInterface and WCL of laobamac/itlwm 738d4c6
 * (AirportItlwm/AirportItlwmSkywalkInterface.cpp, WCL/WCL.cpp): message
 * layouts (airport/WCL.hpp, unmodified) and the order of the lifecycle
 * messages. Their net80211 accesses are replaced by kext/airport_api.h
 * calls (airport/CREDITS). Every request this class does not implement
 * returns kIOReturnUnsupported from QwzAirportStubs.inc, which
 * scripts/airport-stubs.py generates from the pure virtuals of airport/abi.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef QWZ_AIRPORT_INTERFACE_HPP
#define QWZ_AIRPORT_INTERFACE_HPP

#include "QwzAirport.hpp"
#include "WCL.hpp"

class QwzAirportInterface : public IO80211InfraProtocol {
	OSDeclareDefaultStructors(QwzAirportInterface)
public:
	bool initWithController(QwzAirport *controller);
	void free() override;

	/* Driver (QwzWifi) radio state, QWZMAC_RADIO_*; in the gate. */
	void radioChanged(int state);

	/* Implemented requests (all others: QwzAirportStubs.inc) */
	IOReturn getOP_MODE(apple80211_opmode_data *) override;
	IOReturn getPOWERSAVE(apple80211_powersave_data *) override;
	IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data *) override;
	IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *) override;
	IOReturn getCHANNELS_INFO(apple80211_channels_info *) override;
	IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *) override;
	IOReturn getHW_ADDR(apple80211_hw_mac_address *) override;
	IOReturn getNSS(apple80211_nss_data *) override;
	IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *) override;
	IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane *) override;
	IOReturn setMacAddress(mloAddrArray &) override;

#include "QwzAirportStubs.inc"

private:
	void postDriverAvailable(bool available);

	QwzAirport	*fCtl;
	uint8_t		 fPermanentMAC[6];
	bool		 fAvailable;	/* last DriverAvailable we posted */
};

#endif /* QWZ_AIRPORT_INTERFACE_HPP */
