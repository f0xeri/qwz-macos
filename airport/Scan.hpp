/* Qwz scan snapshot -> WCL message. GPL-2.0-only.
 * WCL layout/decoder: laobamac (WCL.hpp); net80211 translation: QwzWifi. */
#ifndef QWZ_AIRPORT_SCAN_HPP
#define QWZ_AIRPORT_SCAN_HPP
#include "WCL.hpp"
#include "../compat/include/qwzmac/attach.h"
namespace QwzScan {
struct Message {
    AirportWCL::BeaconMetadata metadata;
    uint8_t ies[384];
};
static_assert(offsetof(Message, ies) == sizeof(AirportWCL::BeaconMetadata),
    "IEs immediately follow WCL metadata");
inline bool supported(const AirportWCL::ScanRequest &request)
{
    if (request.type == 2 && request.ssidLength)
        return false;
    bool zero = true, broadcast = true;
    for (unsigned i = 0; i < 6; ++i) {
        zero &= request.bssid[i] == 0;
        broadcast &= request.bssid[i] == 0xff;
    }
    return zero || broadcast;
}
inline qwzmac_scan_request driverRequest(const AirportWCL::ScanRequest &request)
{
    qwzmac_scan_request out = {};
    out.active = request.type != 2;
    out.restricted = request.channelCount != 0;
    out.ssid_len = static_cast<uint8_t>(request.ssidLength);
    memcpy(out.ssid, request.ssid, request.ssidLength);
    memcpy(out.channels, request.channels, sizeof(out.channels));
    memcpy(out.bssid, request.bssid, sizeof(out.bssid));
    return out;
}
inline size_t encode(const qwzmac_scan_bss &b, Message &message)
{
    if (b.ssid_len > sizeof(b.ssid) || !b.channel || b.channel > 196 ||
        b.ie_len > sizeof(b.ies) || !AirportWCL::validIEs(b.ies, b.ie_len))
        return 0;
    message = {};
    auto &m = message.metadata;
    m.ieLength = b.ie_len;
    m.channelSpec = b.channel | (b.channel > 14 ? 0xd000 : 0x1000);
    m.channel = static_cast<uint8_t>(b.channel);
    m.ssidLength = b.ssid_len;
    memcpy(m.ssid, b.ssid, b.ssid_len);
    memcpy(m.bssid, b.bssid, 6);
    m.rssi = b.rssi_dbm;
    m.interval = b.interval;
    m.capability = b.capability;
    m.flags = AirportWCL::SSIDPresent | AirportWCL::RSSIValid;
    memcpy(message.ies, b.ies, b.ie_len);
    return sizeof(m) + b.ie_len;
}
}
#endif
