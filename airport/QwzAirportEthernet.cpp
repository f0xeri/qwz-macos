/*
 * QwzAirportEthernet (QwzAirportEthernet.hpp).
 * Based on AirportItlwmEthernetInterface of laobamac/itlwm 738d4c6.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "QwzAirportEthernet.hpp"
#include "QwzAirport.hpp"

#include <net/ethernet.h>
#include <sys/kpi_mbuf.h>

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(QwzAirportEthernet, IOEthernetInterface);

/*
 * IONetworkInterface::setInterfaceSubType() is private SPI: exported by
 * IONetworkingFamily, but only MacKernelSDK's header declares it (under
 * __PRIVATE_SPI__), not the macOS SDK we build with. It is not virtual, so
 * call it by its symbol, with the object as the first argument.
 */
extern "C" bool qwz_setInterfaceSubType(IONetworkInterface *self, uint32_t subType)
    __asm__("__ZN18IONetworkInterface19setInterfaceSubTypeEj");

bool
QwzAirportEthernet::initWithSkywalkInterface(IONetworkController *controller,
    IO80211SkywalkInterface *interface)
{
	memcpy(fUniqueId, "QWZAIRPORT", 10);
	auto ctl = OSDynamicCast(QwzAirport, controller);
	if (!ctl) return false;
	ctl->macAddress(fUniqueId + 10);
	fInterface = interface;
	fAttached = false;
	return super::init(controller);
}

/* An ifnet's immutable uniqueid is separate from its on-air MAC. The
 * default uses the provider MAC, which prevents a second factory-MAC
 * Ethernet endpoint from attaching even after the native MAC changes.
 * Keep a stable role-specific ID; leave all actual MAC values untouched. */
bool QwzAirportEthernet::initIfnetParams(struct ifnet_init_params *params)
{
	if (!super::initIfnetParams(params)) return false;
	params->uniqueid = fUniqueId;
	params->uniqueid_len = sizeof(fUniqueId);
	return true;
}

/* The Skywalk interface must prepare the ifnet inside the work-loop gate. */
IOReturn
QwzAirportEthernet::prepareBSDGated(OSObject *, void *a0, void *, void *, void *)
{
	QwzAirportEthernet *self = (QwzAirportEthernet *)a0;
	IO80211Controller *ctl = OSDynamicCast(IO80211Controller, self->getController());
	IO80211WorkQueue *queue = ctl != nullptr ? ctl->getWorkQueue() : nullptr;

	if (queue == nullptr || !queue->inGate() || queue->onThread())
		return kIOReturnNotPermitted;
	return self->fInterface->prepareBSDInterface(self->getIfnet(), 0);
}

IOReturn
QwzAirportEthernet::attachToDataLinkLayer(IOOptionBits options, void *parameter)
{
	QwzAirport *ctl = OSDynamicCast(QwzAirport, getController());
	IOEthernetAddress addr;
	char name[IFNAMSIZ];
	UInt8 builtIn = 0;
	IOReturn ret;

	ALOG("BSD attach: enter");
	/* Subtype 3: Wi-Fi, as Apple's drivers set it. */
	if (!qwz_setInterfaceSubType(this, 3))
		return kIOReturnNotReady;
	ret = super::attachToDataLinkLayer(options, parameter);
	if (ret != kIOReturnSuccess || fInterface == nullptr || ctl == nullptr)
		return ret;

	OSData *bi = OSData::withBytes(&builtIn, sizeof(builtIn));
	fInterface->setProperty("built-in", bi);
	OSSafeReleaseNULL(bi);
	snprintf(name, sizeof(name), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
	OSString *s = OSString::withCString(name);
	fInterface->setProperty("IOInterfaceName", s);
	OSSafeReleaseNULL(s);
	OSNumber *u = OSNumber::withNumber(ifnet_unit(getIfnet()), 8);
	fInterface->setProperty(kIOInterfaceUnit, u);
	OSSafeReleaseNULL(u);
	s = OSString::withCString(ifnet_name(getIfnet()));
	fInterface->setProperty(kIOInterfaceNamePrefix, s);
	OSSafeReleaseNULL(s);
	if (ctl->getHardwareAddress(&addr) == kIOReturnSuccess)
		setProperty(kIOMACAddress, (void *)&addr, kIOEthernetAddressSize);

	ALOG("BSD attach: prepareBSDInterface enter");
	ret = ctl->gate()->runAction(prepareBSDGated, this);
	if (ret != kIOReturnSuccess) {
		ALOG("prepareBSDInterface failed: 0x%x", ret);
		super::detachFromDataLinkLayer(options, parameter);
		fAttached = false;
		return ret;
	}
	fAttached = true;
	fInterface->registerService();
	ctl->bsdAttached();
	ALOG("BSD interface %s attached", name);
	return kIOReturnSuccess;
}

void
QwzAirportEthernet::detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
	QwzAirport *ctl = OSDynamicCast(QwzAirport, getController());
	if (ctl != nullptr)
		ctl->bsdDetached();
	super::detachFromDataLinkLayer(options, parameter);
	fAttached = false;
}

IOService *
QwzAirportEthernet::getProvider() const
{
	return fAttached ? fInterface : super::getProvider();
}

bool
QwzAirportEthernet::setLinkState(IO80211LinkState state)
{
	if (state == kIO80211NetworkLinkUp)
		ifnet_set_flags(getIfnet(), IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	else
		ifnet_set_flags(getIfnet(), 0, IFF_UP | IFF_RUNNING);
	return true;
}

/* The stack wants one contiguous Ethernet frame (laobamac's fix). */
static mbuf_t
linearize(mbuf_t packet, UInt32 length)
{
	const size_t len = length ? length : mbuf_pkthdr_len(packet);
	unsigned int chunks = 1;
	mbuf_t m = nullptr;

	if (!(mbuf_flags(packet) & MBUF_PKTHDR) || len < ETHER_HDR_LEN) {
		mbuf_freem(packet);
		return nullptr;
	}
	if (mbuf_next(packet) == nullptr && mbuf_len(packet) >= len)
		return packet;
	if (mbuf_allocpacket(MBUF_DONTWAIT, len + ETHER_ALIGN, &chunks, &m) != 0) {
		mbuf_freem(packet);
		return nullptr;
	}
	UInt8 *data = (UInt8 *)mbuf_data(m) + ETHER_ALIGN;
	if (mbuf_copydata(packet, 0, len, data) != 0 ||
	    mbuf_copy_pkthdr(m, packet) != 0 ||
	    mbuf_setdata(m, data, len) != 0) {
		mbuf_freem(m);
		mbuf_freem(packet);
		return nullptr;
	}
	mbuf_pkthdr_setlen(m, len);
	mbuf_pkthdr_setheader(m, nullptr);
	mbuf_freem(packet);
	return m;
}

UInt32
QwzAirportEthernet::inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options,
    void *param)
{
	bool direct = param == packet;

	if (packet == nullptr)
		return 0;
	if (getIfnet() == nullptr) {
		mbuf_freem(packet);
		return 0;
	}
	packet = linearize(packet, length);
	if (packet == nullptr) {
		IONetworkData *d = getParameter(kIONetworkStatsKey);
		IONetworkStats *st = d != nullptr ? (IONetworkStats *)d->getBuffer() : nullptr;
		if (st != nullptr)
			st->inputErrors++;
		return 0;
	}
	if (direct)
		param = packet;
	return super::inputPacket(packet, length, options, param);
}
