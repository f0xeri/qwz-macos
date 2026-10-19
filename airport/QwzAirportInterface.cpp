/*
 * QwzAirportInterface (QwzAirportInterface.hpp).
 *
 * Stage C2: WCL learns that the driver is there (ResetInterface,
 * DriverAvailable), can switch the radio and reads capabilities and
 * channels. C3 adds full scans; until C4 nothing here reports
 * an association, even while QwzWifi's own Ethernet interface has one (that
 * join was HeliPort's, not WCL's).
 *
 * The lifecycle messages follow laobamac/itlwm 738d4c6, WCL/WCL.cpp
 * (handleWCLEvent): driver ready -> ResetInterface(1) + DriverAvailable(1),
 * going down -> ResetInterface(0), stopped -> DriverAvailable(0).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "QwzAirportInterface.hpp"
#include "Scan.hpp"
#include <kern/clock.h>
#include <sys/errno.h>

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(QwzAirportInterface, IO80211InfraProtocol);

/* Enough for 2.4 and 5 GHz; qwz has no 6 GHz channels. */
#define QWZ_AIRPORT_MAX_CHAN	APPLE80211_MAX_CHANNELS

bool
QwzAirportInterface::initWithController(QwzAirport *controller)
{
	ether_addr addr;

	if (!IO80211InfraInterface::init())
		return false;
	fCtl = controller;
	fRadioLifecycle = {};
	fScanLock = IOLockAlloc();
	fScanTc = nullptr;
	fScanStopping = fScanPending = fScanStarted = false;
	fScanSerial = fScanGeneration = fScanPolls = 0;
	fScanAccepted = fScanCompleted = 0;
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
	stopScans();
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

void
QwzAirportInterface::radioChanged(int state)
{
	if (state == QWZMAC_RADIO_GOING_DOWN || state == QWZMAC_RADIO_DOWN)
		finishScan(kIOReturnAborted);
	// GOING_DOWN and DOWN may arrive separately or be coalesced. Pair
	// exactly one reset begin with the ready completion in either case.
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
	if (actions & QwzRadio::Available) postDriverAvailable(true);
	fCtl->setProperty("QwzAirportResetPending", fRadioLifecycle.resetPending ?
	    kOSBooleanTrue : kOSBooleanFalse);
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
			if (length) {
				self->fCtl->postMessage(self, AirportWCL::ScanResult,
				    &message, length, true);
				++published;
			}
		}
		self->fCtl->setProperty("QwzAirportScanResults", published, 32);
		self->finishScan(batch->status);
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
	IOLockLock(self->fScanLock);
	if (self->fScanStopping || self->fScanPending) {
		IOLockUnlock(self->fScanLock);
		return kIOReturnBusy;
	}
	self->fScanRequest = request;
	self->fScanPending = true;
	self->fScanStarted = false;
	self->fScanPolls = 0;
	uint32_t serial = ++self->fScanSerial;
	IOLockUnlock(self->fScanLock);
	self->fCtl->setProperty("QwzAirportScanAccepted", ++self->fScanAccepted, 32);
	self->fCtl->setProperty("QwzAirportScanIdentifier", request.identifier, 32);
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
	if (error == EINPROGRESS && polls < 80) {
		self->scheduleScan(250, serial);
		return;
	}
	ScanBatch batch = {serial, nullptr, 0, error == 0 ? kIOReturnSuccess :
	    error == EINPROGRESS ? kIOReturnTimeout : kIOReturnError};
	const size_t bytes = 128 * sizeof(qwzmac_scan_bss);
	if (error == 0) {
		batch.networks = static_cast<qwzmac_scan_bss *>(IOMalloc(bytes));
		if (batch.networks == nullptr) batch.status = kIOReturnNoMemory;
		else batch.count = drv->scan_snapshot(drv->dev, generation, batch.networks, 128);
	}
	ALOG("scan worker: errno=%d results=%d", error, batch.count);
	self->fCtl->gate()->runAction(scanCommand, self, nullptr, &batch);
	if (batch.networks != nullptr) IOFree(batch.networks, bytes);
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
		while (thread_call_isactive(fScanTc)) {
			thread_call_cancel(fScanTc);
			IOSleep(5);
		}
		thread_call_free(fScanTc);
		fScanTc = nullptr;
	}
}
