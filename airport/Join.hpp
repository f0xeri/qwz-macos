/* WCL join wire validation and PMK translation. GPL-2.0-only. */
#ifndef QWZ_JOIN_HPP
#define QWZ_JOIN_HPP
#include "WCL.hpp"
#include "../compat/include/qwzmac/attach.h"
namespace QwzJoin {
inline void wipe(void *p, size_t n) {
    auto bytes = static_cast<volatile uint8_t *>(p);
    while (n--) *bytes++ = 0;
}
inline bool translate(const AirportWCL::JoinRequest &in, qwzmac_native_join &out) {
    out = {};
    auto auth = AirportWCL::classifyLegacyAuth(in);
    if (in.ssidLength == 0 || in.ssidLength > 32 ||
        (in.channelSpec & 0xc000) == 0x4000) return false;
    bool open = auth == AirportWCL::LegacyAuth::Open;
    bool personal = auth == AirportWCL::LegacyAuth::Personal && in.upperAuth == (1u << 3);
    if (!open && !personal) return false;
    if (personal && in.rsnLength && (in.rsnLength < 8 || in.rsn[0] != 48 ||
        in.rsn[4] != 0 || in.rsn[5] != 15 || in.rsn[6] != 172 || in.rsn[7] != 4))
        return false; // Accept WPA2-PSK CCMP; never downgrade TKIP, SAE or enterprise.
    uint8_t any = 0;
    for (auto byte : in.bssid) any |= byte;
    if (!any || (in.bssid[0] & 1)) return false;
    out.ssid_len = static_cast<uint8_t>(in.ssidLength);
    memcpy(out.ssid, in.ssid, out.ssid_len);
    memcpy(out.bssid, in.bssid, 6);
    out.secured = personal;
    if (personal) memcpy(out.pmk, in.key, sizeof(out.pmk));
    return true;
}
inline bool connected(const qwzmac_native_status &s, const uint8_t *bssid) {
    return s.station.state == 4 && s.authorized &&
        !memcmp(s.station.bssid, bssid, 6) && s.beacon_length >= 36 &&
        s.beacon_length <= sizeof(s.beacon) && (s.beacon[0] & 0xfc) == 0x80 &&
        !memcmp(s.beacon + 16, bssid, 6);
}
// Count only newly received associated beacons. Polling an unchanged cached
// frame must not keep WCL alive when the radio really lost the access point.
struct BeaconTracker {
    uint32_t sequence;
    bool initialized;
    void reset() { sequence = 0; initialized = false; }
    uint32_t observe(const qwzmac_native_status &s, const uint8_t *bssid) {
        if (!connected(s, bssid)) return 0;
        if (!initialized) { sequence = s.beacon_sequence; initialized = true; return 0; }
        uint32_t delta = s.beacon_sequence - sequence;
        if (delta < 8) return 0; // same eight-beacon batching as the reference fronts
        sequence = s.beacon_sequence;
        return delta; // unsigned subtraction also handles counter wrap
    }
};
}
#endif
