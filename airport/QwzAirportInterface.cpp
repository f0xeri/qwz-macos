#include "ExtendedBss.hpp"
/*
 * Native Wi-Fi requests, scans and joins over kext/airport_api.h.
 * Lifecycle messages follow laobamac/itlwm 738d4c6 (airport/CREDITS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "QwzAirportInterface.hpp"
#include "Scan.hpp"
#include "Join.hpp"
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <sys/errno.h>

#define super IO80211InfraProtocol
static_assert(sizeof(apple80211_rate_set_data) == 0xbc, "extended BSS rate-set layout");
static_assert(sizeof(apple80211_mcs_index_set_data) == 0x10, "extended BSS HT MCS layout");
static_assert(sizeof(apple80211_vht_mcs_index_set_data) == 6, "extended BSS VHT MCS layout");
OSDefineMetaClassAndStructors(QwzAirportInterface, IO80211InfraProtocol);
static_assert(sizeof(QwzCurrentLink::Channel) == sizeof(apple80211_channel_data), "current channel wire size");
static_assert(offsetof(apple80211_channel_data, channel) == 4, "current channel wire offset");
static_assert(APPLE80211_VERSION == 1 && APPLE80211_C_FLAG_20MHZ == QwzCurrentLink::Width20 &&
    APPLE80211_C_FLAG_40MHZ == QwzCurrentLink::Width40 && APPLE80211_C_FLAG_2GHZ == QwzCurrentLink::Band2 &&
    APPLE80211_C_FLAG_5GHZ == QwzCurrentLink::Band5 && APPLE80211_C_FLAG_ACTIVE == QwzCurrentLink::Active &&
    APPLE80211_C_FLAG_DFS == QwzCurrentLink::DFS && APPLE80211_C_FLAG_EXT_ABV == QwzCurrentLink::Above &&
    APPLE80211_C_FLAG_80MHZ == QwzCurrentLink::Width80 && APPLE80211_C_FLAG_160MHZ == QwzCurrentLink::Width160,
    "current channel flags agree with Apple headers");

/* Enough for 2.4 and 5 GHz; qwz has no 6 GHz channels. */
#define QWZ_AIRPORT_MAX_CHAN	APPLE80211_MAX_CHANNELS

bool
QwzAirportInterface::initWithController(QwzAirport *controller)
{
	ether_addr addr;
	fCtl = nullptr;
	fScanLock = fJoinLock = nullptr;
	fScanTc = fJoinTc = nullptr;

	if (!IO80211InfraInterface::init())
		return false;
	fCtl = controller;
	fJoinLock = IOLockAlloc();
	fJoinTc = nullptr;
	fJoinStopping = fJoinPending = fJoinAssociated = fJoined = fJoinBssValid = false;
	fMacPending = fMacApplying = false;
	fMacError = 0;
	bzero(fMacAddress, sizeof(fMacAddress));
	fJoinExtendedValid = false;
	fJoinBeacons.reset();
	fLqmUpdates = 0;
	fReportedLqm = -2;
	fJoinStation = {};
	fJoinChannel = {};
	bzero(fJoinExtendedBss, sizeof(fJoinExtendedBss));
	fJoinOperation = JoinIdle;
	fJoinSerial = 0;
	fJoinDeadline = 0;
	bzero(&fJoinRequest, sizeof(fJoinRequest));
	bzero(fJoinBSSID, sizeof(fJoinBSSID));
	bzero(fJoinBssInfo, sizeof(fJoinBssInfo));
	bzero(fJoinBeaconInfo, sizeof(fJoinBeaconInfo));
	if (fJoinLock == nullptr) return false;
	fJoinTc = thread_call_allocate_with_options(joinTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (fJoinTc == nullptr) return false;
	fRadioLifecycle = {};
	fPowerReplays = 0;
	fPowerRecovery.cancel();
	fScanLock = IOLockAlloc();
	fScanTc = nullptr;
	fScanStopping = fScanPending = fScanStarted = false;
	fScanSerial = fScanGeneration = fScanPolls = 0;
	fScanAccepted = fScanCompleted = 0;
	fScanProgress.reset();
	fScanPublished = 0;
	fScanStartedAt = 0;
	fScanHadResult = false;
	if (fScanLock == nullptr)
		return false;
	fScanTc = thread_call_allocate_with_options(scanTc, this,
	    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (fScanTc == nullptr)
		return false;
	controller->macAddress(fPermanentMAC);
	memcpy(addr.octet, fPermanentMAC, sizeof(addr.octet));
	return IO80211SkywalkInterface::init(controller, &addr);
}

void
QwzAirportInterface::free()
{
	stopJoins();
	stopScans();
	if (fJoinLock != nullptr) { IOLockFree(fJoinLock); fJoinLock = nullptr; }
	if (fScanLock != nullptr) {
		IOLockFree(fScanLock);
		fScanLock = nullptr;
	}
	super::free();
}

void
QwzAirportInterface::postDriverAvailable(bool available)
{
	AirportWCL::DriverAvailability ev = {};

	ev.version = 3;
	ev.available = available;
	/* laobamac's reasons: ready / stopped. */
	ev.reason = available ? 0xe0821803 : 0xe0821804;
	fCtl->postMessage(this, AirportWCL::DriverAvailable, &ev, sizeof(ev), true);
	fCtl->setProperty("QwzAirportDriverAvailable", available ? kOSBooleanTrue : kOSBooleanFalse);
	ALOG("driver %savailable", available ? "" : "not ");
}

IOReturn
QwzAirportInterface::setINFRA_ENUMERATED(apple80211_infra_enumerated *data)
{
	if (data == nullptr) return kIOReturnBadArgument;
	/* QWZMAC: airportd can subscribe after the initial ready event. Replay
	 * current availability after enumeration so auto-join sees it. Send
	 * only to user space: another WCL reset/ready would rotate the MAC.
	 * The diagnostic property records a send attempt, not delivery. */
	if (fRadioLifecycle.available && !fRadioLifecycle.resetPending) {
		AirportWCL::DriverAvailability ev = {};
		ev.version = 3;
		ev.available = 1;
		ev.reason = 0xe0821803;
		IO80211SkywalkInterface::postMessage(AirportWCL::DriverAvailable,
		    &ev, sizeof(ev), false);
		fCtl->setProperty("QwzAirportAvailabilityReplayAttempted", kOSBooleanTrue);
	}
	return kIOReturnSuccess;
}

void
QwzAirportInterface::radioChanged(int state)
{
	if (state == QWZMAC_RADIO_GOING_DOWN || state == QWZMAC_RADIO_DOWN) {
		fPowerRecovery.cancel();
		finishScan(kIOReturnAborted);
		finishJoin(kIOReturnOffline);
		disconnectJoin();
		IOLockLock(fJoinLock);
		++fJoinSerial;
		fJoinOperation = JoinIdle;
		QwzJoin::wipe(&fJoinRequest, sizeof(fJoinRequest));
		IOLockUnlock(fJoinLock);
	}
	if (state == QWZMAC_RADIO_UP) {
		IOLockLock(fJoinLock);
		if (!fMacApplying && !fMacPending) fMacError = 0;
		IOLockUnlock(fJoinLock);
	}
	// GOING_DOWN and DOWN may arrive separately or be coalesced. Pair
	// exactly one reset begin with the ready completion in either case.
	bool afterReset = fRadioLifecycle.resetPending;
	unsigned actions = QwzRadio::transition(fRadioLifecycle, state);
	if (actions & QwzRadio::BeginReset) {
		uint32_t reset = 0;
		fCtl->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
	}
	if (actions & QwzRadio::EndReset) {
		uint32_t reset = 1;
		fCtl->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
	}
	if (actions & QwzRadio::Unavailable) postDriverAvailable(false);
	if (actions & QwzRadio::Available) {
		if (afterReset) fPowerRecovery.arm();
		postDriverAvailable(true);
	}
	if (state == QWZMAC_RADIO_UP) restorePower();
	fCtl->setProperty("QwzAirportResetPending", fRadioLifecycle.resetPending ?
	    kOSBooleanTrue : kOSBooleanFalse);
}

void
QwzAirportInterface::restorePower()
{
	if (!fPowerRecovery.take()) return;
	// WCL can merge sleep and wake while unavailable, leaving scans stopped.
	// Reapply the user's ON request once WCL has processed firmware-ready.
	static_assert(sizeof(QwzPower::Data) == sizeof(apple80211_power_data),
	    "POWER wire ABI");
	static_assert(offsetof(QwzPower::Data, state) ==
	    offsetof(apple80211_power_data, power_state), "POWER state offset");
	QwzPower::recover(true, fCtl->radioOn(),
	    [this]() { return fCtl->powerRequest(); },
	    [this](unsigned request, unsigned command, QwzPower::Data *data, size_t size) {
		// radioTc enters the shared gate from a thread_call. Apple's
		// synchronous IOUC router requires inGate() && !onThread().
		// Never move this to a work-queue/timer callback.
		int result = routeIoctlToWcl(request, command, data, size);
		fCtl->setProperty("QwzAirportPowerReplayRouteStatus",
		    static_cast<uint32_t>(result), 32);
		return result;
	    },
	    [this](QwzPower::Data *data) {
		return static_cast<int>(fCtl->setPOWER(this,
		    reinterpret_cast<apple80211_power_data *>(data)));
	    },
	    [this]() { postMessage(APPLE80211_M_POWER_CHANGED, nullptr, 0, true); },
	    [this](QwzPower::Request intent, int result) {
		fCtl->setProperty("QwzAirportPowerReplayCount", ++fPowerReplays, 32);
		fCtl->setProperty("QwzAirportPowerReplayStatus", static_cast<uint32_t>(result), 32);
		ALOG("restore power intent %u after firmware ready: 0x%x", intent.serial, result);
	    });
}

IOReturn
QwzAirportInterface::getOP_MODE(apple80211_opmode_data *od)
{
	if (od == nullptr)
		return kIOReturnBadArgument;
	od->version = APPLE80211_VERSION;
	od->op_mode = APPLE80211_M_STA;
	return kIOReturnSuccess;
}

IOReturn
QwzAirportInterface::getPOWERSAVE(apple80211_powersave_data *pd)
{
	if (pd == nullptr)
		return kIOReturnBadArgument;
	pd->version = APPLE80211_VERSION;
	pd->powersave_level = APPLE80211_POWERSAVE_MODE_DISABLED;
	return kIOReturnSuccess;
}

IOReturn
QwzAirportInterface::getSUPPORTED_CHANNELS(apple80211_sup_channel_data *ad)
{
	qwzmac_chan *ch;
	int n;

	if (ad == nullptr)
		return kIOReturnBadArgument;
	ch = (qwzmac_chan *)IOMalloc(sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	if (ch == nullptr)
		return kIOReturnNoMemory;
	n = fCtl->driver()->channels(fCtl->driver()->dev, ch, QWZ_AIRPORT_MAX_CHAN);
	ad->version = APPLE80211_VERSION;
	ad->num_channels = 0;
	for (int i = 0; i < n; i++) {
		apple80211_channel *c = &ad->supported_channels[ad->num_channels++];
		c->version = APPLE80211_VERSION;
		c->channel = ch[i].channel;
		/* The flags laobamac reports here (Ventura's convention). */
		c->flags = APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ |
		    ((ch[i].flags & QWZMAC_CHAN_5GHZ) ? APPLE80211_C_FLAG_5GHZ :
		    APPLE80211_C_FLAG_2GHZ);
	}
	IOFree(ch, sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	return n > 0 ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn
QwzAirportInterface::getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *ad)
{
	return getSUPPORTED_CHANNELS(ad);
}

/* The CHANNELS_INFO layout (0x2720 bytes) is laobamac's, airport/WCL.hpp. */
IOReturn
QwzAirportInterface::getCHANNELS_INFO(apple80211_channels_info *data)
{
	AirportWCL::NativeChannelInfo *info = (AirportWCL::NativeChannelInfo *)data;
	apple80211_country_code_data cc;
	qwzmac_chan *ch;
	int n;

	if (data == nullptr)
		return kIOReturnBadArgument;
	ch = (qwzmac_chan *)IOMalloc(sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	if (ch == nullptr)
		return kIOReturnNoMemory;
	n = fCtl->driver()->channels(fCtl->driver()->dev, ch, QWZ_AIRPORT_MAX_CHAN);
	bzero(info, sizeof(*info));
	for (int i = 0; i < n; i++)
		AirportWCL::appendChannel(*info, ch[i].channel,
		    (ch[i].flags & QWZMAC_CHAN_5GHZ) != 0, false,
		    (ch[i].flags & QWZMAC_CHAN_PASSIVE) != 0,
		    (ch[i].flags & QWZMAC_CHAN_40MHZ) != 0,
		    (ch[i].flags & QWZMAC_CHAN_80MHZ) != 0);
	IOFree(ch, sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	if (fCtl->getCOUNTRY_CODE(this, &cc) == kIOReturnSuccess)
		memcpy(info->country, cc.cc, sizeof(cc.cc));
	return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

/* The WCL_CHANNELS_INFO layout (0xc86 bytes) is laobamac's, airport/WCL.hpp. */
IOReturn
QwzAirportInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
	AirportWCL::ChannelInfo *info = (AirportWCL::ChannelInfo *)data;
	apple80211_country_code_data cc;
	qwzmac_chan *ch;
	int n;

	if (data == nullptr)
		return kIOReturnBadArgument;
	ch = (qwzmac_chan *)IOMalloc(sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	if (ch == nullptr)
		return kIOReturnNoMemory;
	n = fCtl->driver()->channels(fCtl->driver()->dev, ch, QWZ_AIRPORT_MAX_CHAN);
	bzero(info, sizeof(*info));
	for (int i = 0; i < n && info->count < 400; i++) {
		AirportWCL::ChannelEntry &e = info->channels[info->count++];
		bool five = (ch[i].flags & QWZMAC_CHAN_5GHZ) != 0;

		e.channel = (uint8_t)ch[i].channel;
		e.channelSpec = (uint16_t)(ch[i].channel | (five ? 0xd000 : 0x1000));
		if (ch[i].flags & QWZMAC_CHAN_PASSIVE)
			e.flags |= 4;
	}
	IOFree(ch, sizeof(*ch) * QWZ_AIRPORT_MAX_CHAN);
	if (fCtl->getCOUNTRY_CODE(this, &cc) == kIOReturnSuccess)
		memcpy(info->country, cc.cc, 3);
	return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn
QwzAirportInterface::getHW_ADDR(apple80211_hw_mac_address *data)
{
	AirportWCL::HardwareAddress v;

	if (data == nullptr)
		return kIOReturnBadArgument;
	if (!AirportWCL::copyHardwareAddress(fPermanentMAC, v))
		return kIOReturnNotReady;
	/*
	 * Version and address only (bytes 0..9): AppleBCMWLANCore::getHW_ADDR()
	 * writes no further on 25G241, and the header has no size for it.
	 */
	memcpy(data, &v, offsetof(AirportWCL::HardwareAddress, padding));
	return kIOReturnSuccess;
}

/* WCN7850 is 2x2; qwz uses both chains for HT. */
IOReturn
QwzAirportInterface::getNSS(apple80211_nss_data *data)
{
	if (data == nullptr)
		return kIOReturnBadArgument;
	bzero(data, sizeof(*data));
	data->version = APPLE80211_VERSION;
	data->nss = 2;
	return kIOReturnSuccess;
}

/* No low-latency or P2P session. */
IOReturn
QwzAirportInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
	if (data == nullptr)
		return kIOReturnBadArgument;
	bzero(data, 4);
	return kIOReturnSuccess;
}

/* Fast lane (QoS marking) off is fine; on is not supported. */
IOReturn
QwzAirportInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data)
{
	const uint8_t *b = (const uint8_t *)data;

	if (data == nullptr)
		return kIOReturnBadArgument;
	return (b[0] || b[1]) ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn
QwzAirportInterface::setMacAddress(mloAddrArray &addresses)
{
	uint8_t current[6];
	fCtl->macAddress(current);
	if (fJoinPending && memcmp(current, &addresses, 6)) return kIOReturnBusy;
	return fCtl->setHardwareAddress(&addresses, 6);
}

/* All WCL messages and request state transitions run in the front gate.
 * Firmware commands and cache copying run on an ONCE thread call outside it. */
IOReturn QwzAirportInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request)
{
	if (request == nullptr) return kIOReturnBadArgument;
	return fCtl->gate()->runAction(scanCommand, this, request);
}
IOReturn QwzAirportInterface::setWCL_SCAN_ABORT(void *)
{
	return fCtl->gate()->runAction(scanCommand, this, nullptr);
}

/* Keep WCL's advisory, as the reference drivers do. Firmware scan timing
 * and passive/DFS rules remain with qwz. */
IOReturn
QwzAirportInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *data)
{
	if (data == nullptr) return kIOReturnBadArgument;
	fCtl->setProperty("QwzAirportScanHomeAwayHint", data, 8);
	// WCL has reached FW_LOADED. Continue on radioTc: synchronous IOUC
	// calls are forbidden on this work-queue thread.
	if (fPowerRecovery.acknowledge()) fCtl->wclFirmwareReady();
	return kIOReturnSuccess;
}

void QwzAirportInterface::finishScan(IOReturn status)
{
	IOLockLock(fScanLock);
	bool pending = fScanPending;
	fScanPending = false;
	++fScanSerial; // invalidate an in-flight poll/snapshot
	IOLockUnlock(fScanLock);
	if (pending) {
		fCtl->setProperty("QwzAirportScanCompleted", ++fScanCompleted, 32);
		fCtl->setProperty("QwzAirportScanStatus", static_cast<uint32_t>(status), 32);
		ALOG("scan done: 0x%x", status);
		fCtl->postMessage(this, AirportWCL::ScanDone, &status, sizeof(status), true);
	}
}

IOReturn QwzAirportInterface::scanCommand(OSObject *, void *a0, void *a1,
    void *a2, void *)
{
	auto self = static_cast<QwzAirportInterface *>(a0);
	if (a2 != nullptr) {
		auto batch = static_cast<ScanBatch *>(a2);
		IOLockLock(self->fScanLock);
		bool valid = !self->fScanStopping && self->fScanPending &&
		    self->fScanSerial == batch->serial;
		AirportWCL::ScanRequest request = self->fScanRequest;
		IOLockUnlock(self->fScanLock);
		if (!valid) return kIOReturnAborted;
		unsigned published = 0;
		for (int i = 0; i < batch->count; ++i) {
			const auto &b = batch->networks[i];
			if (request.ssidLength && (b.ssid_len != request.ssidLength ||
			    memcmp(b.ssid, request.ssid, b.ssid_len))) continue;
			if (!AirportWCL::scanChannelSelected(request.channels,
			    request.channelCount != 0, b.channel)) continue;
			QwzScan::Message message;
			size_t length = QwzScan::encode(b, message);
			if (length && self->fScanProgress.publish(b, batch->complete)) {
				if (!self->fScanHadResult) {
					uint64_t ns;
					absolutetime_to_nanoseconds(mach_absolute_time() - self->fScanStartedAt, &ns);
					self->fScanHadResult = true;
					self->fCtl->setProperty("QwzAirportScanFirstResultMs", ns / 1000000, 64);
				}
				++self->fScanPublished;
				self->fCtl->postMessage(self, AirportWCL::ScanResult,
				    &message, length, true);
				++published;
				// A newly discovered AP can make WCL abort this scan and join
				// synchronously. Never deliver its remaining old batch afterwards.
				IOLockLock(self->fScanLock);
				valid = !self->fScanStopping && self->fScanPending && self->fScanSerial == batch->serial;
				IOLockUnlock(self->fScanLock);
				if (!valid) return kIOReturnAborted;
			}
		}
		self->fCtl->setProperty("QwzAirportScanPublished", self->fScanPublished, 32);
		if (batch->complete) {
			self->fCtl->setProperty("QwzAirportScanResults", published, 32);
			self->finishScan(batch->status);
		}
		return kIOReturnSuccess;
	}
	if (a1 == nullptr) {
		self->finishScan(kIOReturnAborted);
		return kIOReturnSuccess;
	}
	AirportWCL::ScanRequest request;
	// 0x1550 is the native 0x58 header plus 400 channel entries (laobamac).
	if (!AirportWCL::decodeScan(a1, 0x1550, request))
		return kIOReturnBadArgument;
	self->fCtl->setProperty("QwzAirportScanType", request.type, 32);
	self->fCtl->setProperty("QwzAirportScanChannelCount", request.channelCount, 32);
	// Channel mask and passive mode are applied by the hardware scan owner.
	if (!QwzScan::supported(request)) {
		ALOG("scan request unsupported: id=%u type=%u ssidLength=%u channels=%u",
		    request.identifier, request.type, request.ssidLength, request.channelCount);
		return kIOReturnUnsupported;
	}
	if (!self->fCtl->radioOn()) return kIOReturnNotReady;
	if (self->fJoinPending) return kIOReturnBusy;
	IOLockLock(self->fScanLock);
	if (self->fScanStopping || self->fScanPending) {
		IOLockUnlock(self->fScanLock);
		return kIOReturnBusy;
	}
	self->fScanRequest = request;
	self->fScanPending = true;
	self->fScanStarted = false;
	self->fScanPolls = 0;
	self->fScanProgress.reset();
	self->fScanPublished = 0;
	self->fScanStartedAt = mach_absolute_time();
	self->fScanHadResult = false;
	uint32_t serial = ++self->fScanSerial;
	IOLockUnlock(self->fScanLock);
	self->fCtl->setProperty("QwzAirportScanAccepted", ++self->fScanAccepted, 32);
	self->fCtl->setProperty("QwzAirportScanIdentifier", request.identifier, 32);
	self->fCtl->setProperty("QwzAirportScanFirstResultMs", 0ULL, 64);
	ALOG("scan request %u type %u", request.identifier, request.type);
	self->scheduleScan(0, serial);
	return kIOReturnSuccess;
}

void QwzAirportInterface::scheduleScan(unsigned milliseconds, uint32_t serial)
{
	IOLockLock(fScanLock);
	if (!fScanStopping && fScanPending && fScanSerial == serial) {
		if (milliseconds) {
			uint64_t deadline;
			clock_interval_to_deadline(milliseconds, kMillisecondScale, &deadline);
			thread_call_enter_delayed(fScanTc, deadline);
		} else thread_call_enter(fScanTc);
	}
	IOLockUnlock(fScanLock);
}

void QwzAirportInterface::scanTc(thread_call_param_t p0, thread_call_param_t)
{
	auto self = static_cast<QwzAirportInterface *>(p0);
	IOLockLock(self->fScanLock);
	if (self->fScanStopping || !self->fScanPending) {
		IOLockUnlock(self->fScanLock);
		return;
	}
	uint32_t serial = self->fScanSerial, generation = self->fScanGeneration;
	bool started = self->fScanStarted;
	qwzmac_scan_request request = QwzScan::driverRequest(self->fScanRequest);
	unsigned polls = ++self->fScanPolls;
	IOLockUnlock(self->fScanLock);
	const auto drv = self->fCtl->driver();
	int error = started ? drv->scan_poll(drv->dev, generation) :
	    drv->scan_start(drv->dev, &request, &generation);
	if (!started && error == 0) {
		IOLockLock(self->fScanLock);
		if (self->fScanSerial == serial) {
			self->fScanGeneration = generation;
			self->fScanStarted = true;
		}
		IOLockUnlock(self->fScanLock);
		self->scheduleScan(250, serial);
		return;
	}
	bool progress = error == EINPROGRESS && polls < 80;
	ScanBatch batch = {serial, nullptr, 0, (error == 0 || progress) ? kIOReturnSuccess :
	    error == EINPROGRESS ? kIOReturnTimeout : kIOReturnError, !progress};
	const size_t bytes = 128 * sizeof(qwzmac_scan_bss);
	if (error == 0 || progress) {
		batch.networks = static_cast<qwzmac_scan_bss *>(IOMalloc(bytes));
		if (batch.networks == nullptr) { batch.status = kIOReturnNoMemory; batch.complete = true; }
		else batch.count = progress ? drv->scan_progress(drv->dev, generation, batch.networks, 128) :
		    drv->scan_snapshot(drv->dev, generation, batch.networks, 128);
	}
	if (batch.complete) ALOG("scan worker: errno=%d results=%d", error, batch.count);
	self->fCtl->gate()->runAction(scanCommand, self, nullptr, &batch);
	if (batch.networks != nullptr) IOFree(batch.networks, bytes);
	if (!batch.complete) self->scheduleScan(250, serial);
}

void QwzAirportInterface::stopScans()
{
	if (fScanLock == nullptr) return;
	IOLockLock(fScanLock);
	fScanStopping = true;
	fScanPending = false;
	++fScanSerial;
	IOLockUnlock(fScanLock);
	if (fScanTc != nullptr) {
		qwz_airport_call_stop(fScanTc);
		thread_call_free(fScanTc);
		fScanTc = nullptr;
	}
}

IOReturn QwzAirportInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)1, request);
}
IOReturn QwzAirportInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)2);
}
IOReturn QwzAirportInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)3);
}
IOReturn QwzAirportInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)7, out);
}
IOReturn QwzAirportInterface::getWCL_BSS_INFO(apple80211_beacon_msg *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)5, out);
}
IOReturn QwzAirportInterface::getBEACON_INFO(apple80211_beacon_info_t *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)6, out);
}
IOReturn QwzAirportInterface::getCHANNEL(apple80211_channel_data *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)9, out);
}
IOReturn QwzAirportInterface::getRSSI(apple80211_rssi_data *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)10, out);
}
IOReturn QwzAirportInterface::getRATE(apple80211_rate_data *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)11, out);
}
IOReturn QwzAirportInterface::getMCS(apple80211_mcs_data *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)13, out);
}
IOReturn QwzAirportInterface::getGUARD_INTERVAL(apple80211_guard_interval_data *out)
{
	return fCtl->gate()->runAction(joinCommand, this, (void *)14, out);
}

void QwzAirportInterface::finishJoin(IOReturn error)
{
	if (!fJoinPending) return;
	fJoinPending = false;
	AirportWCL::ConnectionEvent event = {};
	event.status = error ? 1 : 0;
	event.reason = error == kIOReturnTimeout ? 1004 : 0;
	memcpy(event.peers[0].bssid, fJoinBSSID, 6);
	event.peers[0].driverStatus = error;
	fCtl->setProperty("QwzAirportJoinStatus", static_cast<uint32_t>(error), 32);
	fCtl->postMessage(this, AirportWCL::ConnectComplete, &event, sizeof(event), true);
}
void QwzAirportInterface::disconnectJoin()
{
	fJoinExtendedValid = false;
	fJoinBeacons.reset();
	fJoinStation = {};
	fJoinChannel = {};
	if (fJoined) {
		if (fReportedLqm != -2) setLQM(static_cast<unsigned long long>(-2LL));
		fReportedLqm = -2;
		AirportWCL::LinkStatusEvent event = {};
		memcpy(event.bssid, fJoinBSSID, 6);
		event.reason = 255;
		fCtl->postMessage(this, AirportWCL::LinkStatus, &event, sizeof(event), true);
	}
	fJoined = fJoinAssociated = fJoinBssValid = false;
	fCtl->nativeLink(false, 0);
}

IOReturn QwzAirportInterface::joinCommand(OSObject *, void *a0, void *a1,
    void *a2, void *)
{
	auto self = static_cast<QwzAirportInterface *>(a0);
	unsigned operation = static_cast<unsigned>(reinterpret_cast<uintptr_t>(a1));
	if (operation >= 9 && operation <= 14 && operation != 12) {
		if (!a2) return kIOReturnBadArgument;
		if (self->fJoinStation.state != 4) return kIOReturnNotReady;
		/* The station snapshot is taken once, at join. airportd asks for the
		 * signal, rate, MCS and guard interval all along the connection: refresh it
		 * from the driver (a quick read under its lock, no firmware call), but
		 * only while still on the same BSS. */
		if (operation != 9) {
			qwzmac_sta_info fresh;
			auto drv = self->fCtl->driver();
			if (drv->sta_info(drv->dev, &fresh) == 0 && fresh.state == 4 &&
			    !memcmp(fresh.bssid, self->fJoinStation.bssid, 6))
				self->fJoinStation = fresh;
		}
		if (operation == 13) {
			auto out = static_cast<apple80211_mcs_data *>(a2);
			bzero(out, sizeof(*out));
			out->version = APPLE80211_VERSION;
			out->index = self->fJoinStation.mcs > 0 ? static_cast<uint32_t>(self->fJoinStation.mcs) : 0;
			return kIOReturnSuccess;
		}
		if (operation == 14) {
			auto out = static_cast<apple80211_guard_interval_data *>(a2);
			bzero(out, sizeof(*out));
			out->version = APPLE80211_VERSION;
			out->interval = self->fJoinStation.gi_ns && self->fJoinStation.gi_ns < 800 ?
			    APPLE80211_GI_SHORT : APPLE80211_GI_LONG;
			return kIOReturnSuccess;
		}
		if (operation == 9) {
			if (!self->fJoinChannel.number) return kIOReturnNotReady;
			memcpy(a2, &self->fJoinChannel, sizeof(self->fJoinChannel));
		} else if (operation == 10) {
			auto out = static_cast<apple80211_rssi_data *>(a2);
			bzero(out, sizeof(*out));
			out->version = APPLE80211_VERSION; out->num_radios = 1;
			out->rssi_unit = APPLE80211_UNIT_DBM;
			out->rssi[0] = out->aggregate_rssi = out->rssi_ext[0] =
			    out->aggregate_rssi_ext = self->fJoinStation.rssi_dbm;
		} else {
			if (!self->fJoinStation.rate_bps) return kIOReturnNotReady;
			auto out = static_cast<apple80211_rate_data *>(a2);
			bzero(out, sizeof(*out));
			out->version = APPLE80211_VERSION; out->num_radios = 1;
			out->rate[0] = static_cast<uint32_t>(self->fJoinStation.rate_bps / 1000000);
		}
		return kIOReturnSuccess;
	}
	if (operation == 7) {
		if (!a2 || !self->fJoinExtendedValid) return kIOReturnNotReady;
		memcpy(a2, self->fJoinExtendedBss, sizeof(self->fJoinExtendedBss));
		return kIOReturnSuccess;
	}
	if (operation == 8) {
		int error = *static_cast<int *>(a2);
		IOLockLock(self->fJoinLock);
		self->fMacApplying = false;
		self->fMacError = error;
		bool stopped = self->fJoinStopping;
		uint32_t serial = self->fJoinSerial;
		IOLockUnlock(self->fJoinLock);
		self->fCtl->setProperty("QwzAirportMACStatus", static_cast<uint32_t>(error), 32);
		if (stopped) return kIOReturnAborted;
		if (error && self->fJoinPending) {
			self->finishJoin(kIOReturnError);
			self->disconnectJoin();
			IOLockLock(self->fJoinLock);
			self->fJoinOperation = JoinLeave;
			IOLockUnlock(self->fJoinLock);
		}
		self->scheduleJoin(0, serial);
		return error ? kIOReturnError : kIOReturnSuccess;
	}
	if (operation == 5 || operation == 6) {
		if (!a2 || !self->fJoinBssValid) return kIOReturnNotReady;
		if (operation == 5) memcpy(a2, self->fJoinBssInfo, sizeof(self->fJoinBssInfo));
		else memcpy(a2, self->fJoinBeaconInfo, sizeof(self->fJoinBeaconInfo));
		return kIOReturnSuccess;
	}
	if (!self->fCtl->nativeDataMode()) return kIOReturnNotReady;
	if (operation == 4) {
		auto batch = static_cast<JoinBatch *>(a2);
		IOLockLock(self->fJoinLock);
		bool valid = !self->fJoinStopping && self->fJoinSerial == batch->serial;
		IOLockUnlock(self->fJoinLock);
		if (!valid) return kIOReturnAborted;
		if (batch->operation == JoinLeave) {
			IOLockLock(self->fJoinLock);
			self->fJoinOperation = JoinIdle;
			IOLockUnlock(self->fJoinLock);
			return kIOReturnSuccess;
		}
		if (batch->error) {
			self->finishJoin(kIOReturnError);
			self->disconnectJoin();
			IOLockLock(self->fJoinLock);
			self->fJoinOperation = JoinLeave;
			IOLockUnlock(self->fJoinLock);
			self->scheduleJoin(0, batch->serial);
			return kIOReturnSuccess;
		}
		if (!batch->status) {
			self->scheduleJoin(250, batch->serial);
			return kIOReturnSuccess;
		}
		const auto &status = *batch->status;
		bool samePeer = status.station.state == 4 &&
		    !memcmp(status.station.bssid, self->fJoinBSSID, 6);
		if (samePeer) {
			// Cache before AssocComplete/LinkStatus: airportd synchronously
			// queries these, just as it does in the reference native fronts.
			self->fJoinStation = status.station;
			QwzCurrentLink::channel(status, self->fJoinChannel);
			self->fJoinExtendedValid = QwzExtendedBss::encode(status,
			    self->fJoinExtendedBss, sizeof(self->fJoinExtendedBss));
			self->fCtl->setProperty("QwzAirportExtendedBssReady", self->fJoinExtendedValid);
			QwzScan::Message message;
			size_t size = QwzScan::encode(status.bss, message);
			if (size) {
				bzero(self->fJoinBssInfo, sizeof(self->fJoinBssInfo));
				memcpy(self->fJoinBssInfo, &message, size);
			}
			if (status.beacon_length >= 36 && status.beacon_length <= 2048) {
				bzero(self->fJoinBeaconInfo, sizeof(self->fJoinBeaconInfo));
				memcpy(self->fJoinBeaconInfo, &status.beacon_length, 4);
				memcpy(self->fJoinBeaconInfo + 4, status.beacon, status.beacon_length);
				self->fJoinBssValid = size != 0;
			}
			if (!self->fJoinAssociated) {
				self->fJoinAssociated = true;
				AirportWCL::AssociationEvent event = {};
				memcpy(event.bssid, self->fJoinBSSID, 6);
				self->fCtl->postMessage(self, AirportWCL::AssocComplete, &event, sizeof(event), true);
			}
		}
		if (!self->fJoined && self->fJoinPending && self->fJoinBssValid && self->fJoinExtendedValid &&
		    QwzJoin::connected(status, self->fJoinBSSID)) {
			self->fJoined = true;
			self->fCtl->nativeLink(true, status.station.rate_bps);
			AirportWCL::LinkStatusEvent link = {};
			memcpy(link.bssid, self->fJoinBSSID, 6); link.up = 1; link.reason = 255;
			self->fCtl->postMessage(self, AirportWCL::LinkStatus, &link, sizeof(link), true);
			if (!self->fJoined || !self->fJoinPending) return kIOReturnSuccess;
			AirportWCL::FirstBeaconEvent beacon = {};
			memcpy(beacon.peer.bssid, self->fJoinBSSID, 6);
			self->fCtl->postMessage(self, AirportWCL::FirstBeacon, &beacon, sizeof(beacon), true);
			if (!self->fJoined || !self->fJoinPending) return kIOReturnSuccess;
			self->finishJoin(kIOReturnSuccess);
		}
		if (self->fJoined && (!samePeer || !status.authorized)) {
			self->disconnectJoin();
			IOLockLock(self->fJoinLock);
			self->fJoinOperation = JoinIdle;
			IOLockUnlock(self->fJoinLock);
			return kIOReturnSuccess;
		}
		if (self->fJoined) {
			// Same Tahoe WCL snapshot as AirportMtkWlan's beacon handler.
			// RX wakes this worker after each eight beacons. The periodic
			// status poll cannot create updates from a stalled RX counter.
			uint32_t delta = self->fJoinBeacons.observe(status, self->fJoinBSSID);
			AirportWCL::LqmBeaconUpdate update;
			if (AirportWCL::makeBeaconUpdate(delta, update)) {
				int rssi = status.station.rssi_dbm;
				int quality = rssi >= -70 ? 100 : (rssi >= -85 ? 50 : 20);
				if (quality != self->fReportedLqm) {
					self->setLQM(static_cast<unsigned long long>(quality));
					self->fReportedLqm = quality;
				}
				self->fCtl->setProperty("QwzAirportBeaconSequence", status.beacon_sequence, 32);
				self->fCtl->setProperty("QwzAirportLqmUpdates", ++self->fLqmUpdates, 32);
				self->fCtl->postMessage(self, AirportWCL::LqmUpdate, &update, sizeof(update), true);
				// WCL may synchronously leave from a notification callback.
				if (!self->fJoined) return kIOReturnSuccess;
			}
		}
		// RX-triggered polls must not shorten the references' 30-second join timeout.
		if (self->fJoinPending && mach_absolute_time() >= self->fJoinDeadline) {
			self->finishJoin(kIOReturnTimeout);
			self->disconnectJoin();
			IOLockLock(self->fJoinLock);
			self->fJoinOperation = JoinLeave;
			IOLockUnlock(self->fJoinLock);
			self->scheduleJoin(0, batch->serial);
		} else self->scheduleJoin(self->fJoined ? 1000 : 250, batch->serial);
		return kIOReturnSuccess;
	}
	if (operation == 2 || operation == 3) {
		self->finishJoin(kIOReturnAborted);
		self->disconnectJoin();
		IOLockLock(self->fJoinLock);
		QwzJoin::wipe(&self->fJoinRequest, sizeof(self->fJoinRequest));
		uint32_t serial = ++self->fJoinSerial;
		self->fJoinOperation = JoinLeave;
		IOLockUnlock(self->fJoinLock);
		self->scheduleJoin(0, serial);
		if (operation == 2)
			self->fCtl->postMessage(self, AirportWCL::JoinAbortComplete, nullptr, 0, true);
		return kIOReturnSuccess;
	}
	if (operation != 1 || !a2) return kIOReturnBadArgument;
	AirportWCL::JoinRequest wire = {};
	qwzmac_native_join request = {};
	bool decoded = AirportWCL::decodeJoin(a2, 0x2cc, wire);
	bool supported = decoded && QwzJoin::translate(wire, request);
	QwzJoin::wipe(&wire, sizeof(wire));
	self->fCtl->setProperty("QwzAirportJoinDecoded", decoded);
	self->fCtl->setProperty("QwzAirportJoinSupported", supported);
	if (!supported) { QwzJoin::wipe(&request, sizeof(request)); return decoded ? kIOReturnUnsupported : kIOReturnBadArgument; }
	if (!self->fCtl->radioOn() || self->fJoinPending) {
		QwzJoin::wipe(&request, sizeof(request)); return kIOReturnBusy;
	}
	IOLockLock(self->fJoinLock);
	bool busy = self->fJoinStopping || self->fJoinOperation == JoinLeave || self->fMacError;
	IOLockUnlock(self->fJoinLock);
	if (busy) { QwzJoin::wipe(&request, sizeof(request)); return kIOReturnBusy; }
	self->finishScan(kIOReturnAborted);
	self->disconnectJoin();
	IOLockLock(self->fJoinLock);
	self->fJoinRequest = request;
	self->fJoinPending = true;
	self->fJoinOperation = JoinApply;
	clock_interval_to_deadline(30, kSecondScale, &self->fJoinDeadline);
	memcpy(self->fJoinBSSID, request.bssid, 6);
	uint32_t serial = ++self->fJoinSerial;
	IOLockUnlock(self->fJoinLock);
	QwzJoin::wipe(&request, sizeof(request));
	self->fCtl->setProperty("QwzAirportJoinRequested", serial, 32);
	self->scheduleJoin(0, serial);
	return kIOReturnSuccess;
}

void QwzAirportInterface::beaconReceived()
{
	IOLockLock(fJoinLock);
	if (!fJoinStopping && fJoinOperation == JoinPoll && !fMacPending && !fMacApplying)
		thread_call_enter(fJoinTc);
	IOLockUnlock(fJoinLock);
}

void QwzAirportInterface::scheduleJoin(unsigned milliseconds, uint32_t serial)
{
	IOLockLock(fJoinLock);
	if (!fJoinStopping && fJoinSerial == serial && fJoinOperation != JoinIdle) {
		if (!milliseconds) thread_call_enter(fJoinTc);
		else {
			uint64_t deadline;
			clock_interval_to_deadline(milliseconds, kMillisecondScale, &deadline);
			thread_call_enter_delayed(fJoinTc, deadline);
		}
	}
	IOLockUnlock(fJoinLock);
}
bool QwzAirportInterface::macBusy()
{
	IOLockLock(fJoinLock);
	bool busy = fMacPending || fMacApplying || fJoinStopping;
	IOLockUnlock(fJoinLock);
	return busy;
}
void QwzAirportInterface::queueMacAddress(const uint8_t mac[6])
{
	finishScan(kIOReturnAborted);
	disconnectJoin();
	IOLockLock(fJoinLock);
	if (!fJoinStopping) {
		memcpy(fMacAddress, mac, 6);
		fMacPending = true;
		fMacError = 0;
		thread_call_enter(fJoinTc);
	}
	IOLockUnlock(fJoinLock);
}
void QwzAirportInterface::joinTc(thread_call_param_t p0, thread_call_param_t)
{
	auto self = static_cast<QwzAirportInterface *>(p0);
	IOLockLock(self->fJoinLock);
	if (!self->fJoinStopping && self->fMacPending) {
		uint8_t mac[6];
		memcpy(mac, self->fMacAddress, 6);
		self->fMacPending = false;
		self->fMacApplying = true;
		IOLockUnlock(self->fJoinLock);
		const auto driver = self->fCtl->driver();
		int error = driver->set_mac(driver->dev, mac);
		self->fCtl->gate()->runAction(joinCommand, self, (void *)8, &error);
		return;
	}
	if (self->fJoinStopping || self->fJoinOperation == JoinIdle) {
		IOLockUnlock(self->fJoinLock); return;
	}
	JoinBatch batch = {self->fJoinSerial, self->fJoinOperation, 0, nullptr};
	qwzmac_native_join request = self->fJoinRequest;
	QwzJoin::wipe(&self->fJoinRequest, sizeof(self->fJoinRequest));
	if (batch.operation == JoinApply) self->fJoinOperation = JoinPoll;
	IOLockUnlock(self->fJoinLock);
	const auto driver = self->fCtl->driver();
	if (batch.operation == JoinApply) batch.error = driver->join(driver->dev, &request);
	else if (batch.operation == JoinLeave) batch.error = driver->leave(driver->dev);
	else {
		batch.status = static_cast<qwzmac_native_status *>(IOMalloc(sizeof(*batch.status)));
		batch.error = batch.status ? driver->join_status(driver->dev, batch.status) : ENOMEM;
	}
	QwzJoin::wipe(&request, sizeof(request));
	self->fCtl->gate()->runAction(joinCommand, self, (void *)4, &batch);
	if (batch.status) IOFree(batch.status, sizeof(*batch.status));
}
void QwzAirportInterface::stopJoins()
{
	if (!fJoinLock) return;
	IOLockLock(fJoinLock);
	fJoinStopping = true;
	++fJoinSerial;
	QwzJoin::wipe(&fJoinRequest, sizeof(fJoinRequest));
	IOLockUnlock(fJoinLock);
	if (fJoinTc) {
		qwz_airport_call_stop(fJoinTc);
		thread_call_free(fJoinTc); fJoinTc = nullptr;
	}
}
