/*
 * QwzAirportInterface (QwzAirportInterface.hpp).
 *
 * Stage C2: WCL learns that the driver is there (ResetInterface,
 * DriverAvailable), can switch the radio and reads capabilities and
 * channels. Scans and joins come in C3/C4; until then nothing here reports
 * an association, even while QwzWifi's own Ethernet interface has one (that
 * join was HeliPort's, not WCL's).
 *
 * The lifecycle messages follow laobamac/itlwm 738d4c6, WCL/WCL.cpp
 * (handleWCLEvent): driver ready -> ResetInterface(1) + DriverAvailable(1),
 * going down -> ResetInterface(0), stopped -> DriverAvailable(0).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "QwzAirportInterface.hpp"

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
	fAvailable = false;
	controller->macAddress(fPermanentMAC);
	memcpy(addr.octet, fPermanentMAC, sizeof(addr.octet));
	return IO80211SkywalkInterface::init(controller, &addr);
}

void
QwzAirportInterface::free()
{
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
	fAvailable = available;
	ALOG("driver %savailable", available ? "" : "not ");
}

void
QwzAirportInterface::radioChanged(int state)
{
	uint32_t reset;

	switch (state) {
	case QWZMAC_RADIO_GOING_DOWN:
		if (fAvailable) {
			reset = 0;	/* resetInterfacesBegin */
			fCtl->postMessage(this, AirportWCL::ResetInterface, &reset,
			    sizeof(reset), true);
		}
		break;
	case QWZMAC_RADIO_DOWN:
		postDriverAvailable(false);
		break;
	case QWZMAC_RADIO_UP:
		if (!fAvailable) {
			reset = 1;	/* resetInterfacesComplete */
			fCtl->postMessage(this, AirportWCL::ResetInterface, &reset,
			    sizeof(reset), true);
			postDriverAvailable(true);
		}
		break;
	}
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
