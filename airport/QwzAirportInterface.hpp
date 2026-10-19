/*
 * QwzAirportInterface: the Wi-Fi interface macOS's Wi-Fi stack (WCL) talks
 * to (an IO80211InfraProtocol). Stage C2: power, capabilities, channels,
 * state, availability and full scan (C3); join (C4) is not there yet.
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
#include "RadioLifecycle.hpp"

class QwzAirportInterface : public IO80211InfraProtocol {
	OSDeclareDefaultStructors(QwzAirportInterface)
public:
	bool initWithController(QwzAirport *controller);
	void free() override;
	void stopScans();
	void stopJoins();
	bool macBusy();
	void queueMacAddress(const uint8_t[6]);
	IOReturn setWCL_ASSOCIATE(apple80211AssocCandidates *) override;
	IOReturn setWCL_JOIN_ABORT(apple80211_wcl_abort_join *) override;
	IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *) override;
	IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *) override;
	IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *) override;
	IOReturn getBEACON_INFO(apple80211_beacon_info_t *) override;
	IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *) override;
	IOReturn setWCL_SCAN_ABORT(void *) override;

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
	static void joinTc(thread_call_param_t, thread_call_param_t);
	static IOReturn joinCommand(OSObject *, void *, void *, void *, void *);
	void finishJoin(IOReturn);
	void disconnectJoin();
	void scheduleJoin(unsigned, uint32_t);
	struct JoinBatch { uint32_t serial; int operation, error; qwzmac_native_status *status; };
	enum { JoinIdle, JoinApply, JoinPoll, JoinLeave };
	IOLock *fJoinLock;
	thread_call_t fJoinTc;
	bool fMacPending, fMacApplying;
	int fMacError;
	uint8_t fMacAddress[6];
	bool fJoinStopping, fJoinPending, fJoinAssociated, fJoined;
	int fJoinOperation;
	uint32_t fJoinSerial, fJoinPolls;
	uint8_t fJoinBSSID[6];
	qwzmac_native_join fJoinRequest;
	uint8_t fJoinBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800];
	uint8_t fJoinBeaconInfo[0x808];
	bool fJoinBssValid, fJoinExtendedValid;
	uint8_t fJoinExtendedBss[0x214];
	static void scanTc(thread_call_param_t, thread_call_param_t);
	static IOReturn scanCommand(OSObject *, void *, void *, void *, void *);
	void finishScan(IOReturn);
	void scheduleScan(unsigned milliseconds, uint32_t serial);
	struct ScanBatch {
		uint32_t serial;
		qwzmac_scan_bss *networks;
		int count;
		IOReturn status;
	};
	IOLock *fScanLock;
	thread_call_t fScanTc;
	bool fScanStopping, fScanPending, fScanStarted;
	uint32_t fScanSerial, fScanGeneration, fScanPolls;
	uint32_t fScanAccepted, fScanCompleted;
	AirportWCL::ScanRequest fScanRequest;

	QwzAirport	*fCtl;
	uint8_t		 fPermanentMAC[6];
	QwzRadio::State fRadioLifecycle;
};

#endif /* QWZ_AIRPORT_INTERFACE_HPP */
