/*
 * QwzWifi: user client for HeliPort, itlwm's Wi-Fi menu app, so the
 * network (and its password, kept in the user's Keychain by HeliPort) is
 * picked at run time instead of being built into the kext.
 *
 * HeliPort finds us by IOClass "itlwm" (set in QwzWifi::start()), opens
 * this client (IOUserClientClass in Info.plist) and sends the requests in
 * kext/heliport.h. Each request maps to a qwzmac_qwz_* management call
 * (compat/autoconf.c), which does what ifconfig(8) would in OpenBSD.
 *
 * Like itlwm, any local process may use it: the same as joining a network
 * from the menu bar. The passphrase is never logged and is wiped after the
 * PSK is derived.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <net/kpi_interface.h>
#include <sys/errno.h>

#include "QwzWifi.h"
#include "heliport.h"

/* Networks returned per scan-result walk; net80211 keeps fewer. */
#define QWZ_SCAN_MAX	256

class QwzUserClient : public IOUserClient {
	OSDeclareDefaultStructors(QwzUserClient)
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) override;
	bool start(IOService *provider) override;
	IOReturn clientClose() override;
	void free() override;
	IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) override;

private:
	IOReturn request(uint32_t id, bool set, void *buf);
	IOReturn driverInfo(hp_driver_info *di);
	IOReturn staInfo(hp_sta_info *st);
	IOReturn associate(hp_associate *as);
	IOReturn scanResult(hp_network_info *ni);

	QwzWifi		*fDriver;
	IOLock		*fLock;		/* one request at a time per client */
	qwzmac_bss	*fScan;		/* snapshot being walked, or null */
	int		 fScanCount;
	int		 fScanNext;
};

OSDefineMetaClassAndStructors(QwzUserClient, IOUserClient)

/* Size of each request's struct, get and set alike (order of hp_ioctl_id). */
static const size_t sReqSize[HP_IOCTL_ID_MAX] = {
	sizeof(hp_driver_info),		/* DRIVER_INFO */
	sizeof(hp_sta_info),		/* STA_INFO */
	sizeof(hp_power),		/* POWER */
	sizeof(hp_state),		/* STATE */
	sizeof(hp_nw_id),		/* NW_ID */
	sizeof(hp_wpa_key),		/* WPA_KEY */
	sizeof(hp_associate),		/* ASSOCIATE */
	sizeof(hp_disassociate),	/* DISASSOCIATE */
	sizeof(hp_associate),		/* JOIN */
	sizeof(hp_version_only),	/* SCAN */
	sizeof(hp_network_info),	/* SCAN_RESULT */
	sizeof(hp_version_only),	/* TX_POWER_LEVEL */
	sizeof(hp_nw_bssid),		/* NW_BSSID */
};
static_assert(HP_IOCTL_NW_BSSID == 12 && HP_IOCTL_ID_MAX == 13, "sReqSize order");

bool
QwzUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	fDriver = nullptr;
	fScan = nullptr;
	fScanCount = fScanNext = 0;
	fLock = IOLockAlloc();
	if (fLock == nullptr)
		return false;
	return IOUserClient::initWithTask(owningTask, securityID, type,
	    properties);
}

bool
QwzUserClient::start(IOService *provider)
{
	fDriver = OSDynamicCast(QwzWifi, provider);
	if (fDriver == nullptr)
		return false;
	return IOUserClient::start(provider);
}

IOReturn
QwzUserClient::clientClose()
{
	if (!isInactive())
		terminate();
	return kIOReturnSuccess;
}

void
QwzUserClient::free()
{
	if (fScan != nullptr) {
		IOFree(fScan, sizeof(*fScan) * QWZ_SCAN_MAX);
		fScan = nullptr;
	}
	if (fLock != nullptr) {
		IOLockFree(fLock);
		fLock = nullptr;
	}
	IOUserClient::free();
}

IOReturn
QwzUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
    OSObject *target, void *reference)
{
	if (selector == QWZ_IOCTL_HELIPORT_MODE) {
		if (clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
			return kIOReturnNotPrivileged;
		if (args->structureInputSize || args->structureOutputSize ||
		    args->scalarInputCount || args->scalarOutputCount)
			return kIOReturnBadArgument;
		return fDriver->requestHeliportMode();
	}
	bool set = (selector & HP_IOCTL_MASK) != 0;
	uint32_t id = selector & ~(uint32_t)HP_IOCTL_MASK;
	void *buf;
	IOReturn ret;

	(void)dispatch; (void)target; (void)reference;
	if (id >= HP_IOCTL_ID_MAX)
		return kIOReturnBadArgument;
	/* Small structs only: no memory descriptors, exact sizes. */
	if (set) {
		if (args->structureInput == nullptr ||
		    args->structureInputSize != sReqSize[id])
			return kIOReturnBadArgument;
		buf = (void *)args->structureInput;
	} else {
		if (args->structureOutput == nullptr ||
		    args->structureOutputSize < sReqSize[id])
			return kIOReturnBadArgument;
		buf = args->structureOutput;
		args->structureOutputSize = (uint32_t)sReqSize[id];
		bzero(buf, sReqSize[id]);
	}

	if (fDriver->nativeDataMode()) {
		if (set && (id == HP_IOCTL_ASSOCIATE || id == HP_IOCTL_JOIN || id == HP_IOCTL_WPA_KEY))
			bzero(buf, sReqSize[id]);
		return kIOReturnExclusiveAccess;
	}
	if (!fDriver->clientEnter())
		return kIOReturnNotReady;
	IOLockLock(fLock);
	ret = request(id, set, buf);
	IOLockUnlock(fLock);
	fDriver->clientExit();
	if (set && (id == HP_IOCTL_ASSOCIATE || id == HP_IOCTL_JOIN ||
	    id == HP_IOCTL_WPA_KEY))
		bzero(buf, sReqSize[id]);	/* the passphrase */
	return ret;
}

static IOReturn
errnoToIOReturn(int error)
{
	switch (error) {
	case 0:		return kIOReturnSuccess;
	case EBUSY:	return kIOReturnBusy;
	case EINVAL:	return kIOReturnBadArgument;
	case ENOTCONN:	return kIOReturnNotReady;
	default:	return kIOReturnError;
	}
}

IOReturn
QwzUserClient::request(uint32_t id, bool set, void *buf)
{
	void *dev = fDriver->qwzDev();
	qwzmac_sta_info si;

	switch (id) {
	case HP_IOCTL_DRIVER_INFO:
		return set ? kIOReturnUnsupported :
		    driverInfo((hp_driver_info *)buf);
	case HP_IOCTL_STA_INFO:
		return set ? kIOReturnUnsupported :
		    staInfo((hp_sta_info *)buf);
	case HP_IOCTL_POWER: {
		hp_power *pw = (hp_power *)buf;

		if (set)
			return errnoToIOReturn(
			    qwzmac_qwz_set_power(dev, pw->enabled != 0));
		pw->version = HP_IOCTL_VERSION;
		pw->enabled = (unsigned int)qwzmac_qwz_power(dev);
		return kIOReturnSuccess;
	}
	case HP_IOCTL_STATE: {
		hp_state *st = (hp_state *)buf;

		if (set)
			return kIOReturnUnsupported;
		qwzmac_qwz_sta_info(dev, &si);
		st->version = HP_IOCTL_VERSION;
		st->state = si.state;
		return kIOReturnSuccess;
	}
	case HP_IOCTL_NW_ID: {
		hp_nw_id *nw = (hp_nw_id *)buf;

		if (set)
			return kIOReturnUnsupported;
		nw->version = HP_IOCTL_VERSION;
		if (qwzmac_qwz_sta_info(dev, &si) == 0) {
			nw->len = si.ssid_len;
			memcpy(nw->nwid, si.ssid, si.ssid_len);
		}
		return kIOReturnSuccess;
	}
	case HP_IOCTL_NW_BSSID: {
		hp_nw_bssid *nb = (hp_nw_bssid *)buf;

		if (set)
			return kIOReturnUnsupported;
		nb->version = HP_IOCTL_VERSION;
		if (qwzmac_qwz_sta_info(dev, &si) == 0)
			memcpy(nb->bssid, si.bssid, sizeof(nb->bssid));
		return kIOReturnSuccess;
	}
	case HP_IOCTL_ASSOCIATE:
	case HP_IOCTL_JOIN:
		/* itlwm's JOIN adds to an auto-join list; we join one network only. */
		return set ? associate((hp_associate *)buf) :
		    kIOReturnUnsupported;
	case HP_IOCTL_DISASSOCIATE:
		if (!set)
			return kIOReturnUnsupported;
		QLOG("app: disconnect");
		return errnoToIOReturn(qwzmac_qwz_leave(dev));
	case HP_IOCTL_SCAN:
		if (!set)
			return kIOReturnUnsupported;
		qwzmac_qwz_scan(dev);
		return kIOReturnSuccess;
	case HP_IOCTL_SCAN_RESULT:
		return set ? kIOReturnUnsupported :
		    scanResult((hp_network_info *)buf);
	case HP_IOCTL_WPA_KEY:
	case HP_IOCTL_TX_POWER_LEVEL:
	default:
		return kIOReturnUnsupported;
	}
}

IOReturn
QwzUserClient::driverInfo(hp_driver_info *di)
{
	di->version = HP_IOCTL_VERSION;
	qwzmac_qwz_fw_version(fDriver->qwzDev(), di->fw_version,
	    sizeof(di->fw_version));
	strlcpy(di->driver_version, "QwzWifi " QWZ_VERSION,
	    sizeof(di->driver_version));
	/* Empty until the interface exists: HeliPort then says "not running". */
	fDriver->bsdName(di->bsd_name, sizeof(di->bsd_name));
	return kIOReturnSuccess;
}

IOReturn
QwzUserClient::staInfo(hp_sta_info *st)
{
	qwzmac_sta_info si;
	int error;

	error = qwzmac_qwz_sta_info(fDriver->qwzDev(), &si);
	if (error)
		return errnoToIOReturn(error);	/* HeliPort: not connected */
	st->version = HP_IOCTL_VERSION;
	st->op_mode = si.phy;
	st->max_mcs = si.mcs;
	st->cur_mcs = si.mcs;
	st->channel = si.channel;
	st->band_width = (uint16_t)si.width_mhz;
	st->rssi = (int16_t)si.rssi_dbm;
	st->noise = (int16_t)si.noise_dbm;
	st->rate = (unsigned int)(si.rate_bps / 1000000);
	memcpy(st->ssid, si.ssid, si.ssid_len);
	memcpy(st->bssid, si.bssid, sizeof(st->bssid));
	return kIOReturnSuccess;
}

IOReturn
QwzUserClient::associate(hp_associate *as)
{
	qwzmac_network net;
	size_t slen, plen;
	int error;

	bzero(&net, sizeof(net));
	slen = strnlen(as->nwid.nwid, sizeof(as->nwid.nwid));
	plen = strnlen(as->wpa_key.key, sizeof(as->wpa_key.key));
	if (slen == 0 || plen >= sizeof(net.passphrase)) {
		QLOG("app: join refused: SSID empty or passphrase too long");
		return kIOReturnBadArgument;
	}
	memcpy(net.ssid, as->nwid.nwid, slen);
	memcpy(net.passphrase, as->wpa_key.key, plen);
	QLOG("app: join \"%s\" (%s)", net.ssid, plen ? "WPA2-PSK" : "open");
	error = qwzmac_qwz_join(fDriver->qwzDev(), &net);	/* wipes it */
	bzero(&net, sizeof(net));
	return errnoToIOReturn(error);
}

/*
 * HeliPort calls this repeatedly on one connection until it gets
 * HP_NO_SCAN_RESULT. The first call takes a snapshot of net80211's node
 * list (nodes can go away between calls), the next ones walk it.
 */
IOReturn
QwzUserClient::scanResult(hp_network_info *ni)
{
	qwzmac_bss *b;

	if (fScan == nullptr) {
		fScan = (qwzmac_bss *)IOMalloc(sizeof(*fScan) * QWZ_SCAN_MAX);
		if (fScan == nullptr)
			return kIOReturnNoMemory;
		fScanCount = qwzmac_qwz_scan_results(fDriver->qwzDev(), fScan,
		    QWZ_SCAN_MAX);
		fScanNext = 0;
	}
	if (fScanNext >= fScanCount) {
		IOFree(fScan, sizeof(*fScan) * QWZ_SCAN_MAX);
		fScan = nullptr;
		return HP_NO_SCAN_RESULT;
	}
	b = &fScan[fScanNext++];
	memcpy(ni->ssid, b->ssid, b->ssid_len);
	ni->noise = 0;
	ni->rssi = (int16_t)b->rssi_dbm;
	memcpy(ni->bssid, b->bssid, sizeof(ni->bssid));
	ni->channel = b->channel;
	ni->supported_rsnprotos = b->supported_rsnprotos;
	ni->rsn_protos = b->rsnprotos;
	ni->supported_rsnakms = b->supported_rsnakms;
	ni->rsn_akms = b->rsnakms;
	ni->rsn_ciphers = b->rsnciphers;
	ni->rsn_groupcipher = (int)b->rsngroupcipher;
	ni->rsn_groupmgmtcipher = (int)b->rsngroupmgmtcipher;
	ni->ni_rsncaps = b->rsncaps;
	ni->ni_rsncipher = (int)b->rsncipher;
	return kIOReturnSuccess;
}
