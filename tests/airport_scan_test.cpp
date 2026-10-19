/* Host-side native message bounds/security regression tests. GPL-2.0-only. */
#include "../airport/Scan.hpp"
#include "../airport/RadioLifecycle.hpp"
#include "../compat/include/qwzmac/scan_observation.h"
#include <assert.h>
#include <stdio.h>
int main()
{
    // AP A remains in the net80211 cache after it disappears. Scan 11
    // must publish B alone, not relabel A's observation from scan 10.
    qwzmac_scan_observation apA = {}, apB = {};
    assert(qwzmac_scan_observe(&apA, 10));
    assert(qwzmac_scan_observed(&apA, 10));
    assert(!qwzmac_scan_observe(&apA, 10)); // RSSI is not reset per frame
    assert(qwzmac_scan_observe(&apB, 11));
    assert(!qwzmac_scan_observed(&apA, 11));
    assert(qwzmac_scan_observed(&apB, 11));
    // Cache reads and ordinary RX cannot renew native-scan freshness.
    assert(!qwzmac_scan_observe(&apA, 0));
    assert(!qwzmac_scan_observed(&apA, 11));
    assert(!qwzmac_scan_observed(&apA, 0));
    // Partial scan of another channel doesn't delete A. Actual reception
    // in a later scan republishes it and restarts its RSSI baseline.
    assert(apA.ticket == 10);
    assert(qwzmac_scan_observe(&apA, 12));
    assert(qwzmac_scan_observed(&apA, 12));
    assert(!qwzmac_scan_observed(&apB, 12));
    // Publication rights are distinct from being queued for the next cycle.
    assert(!qwzmac_scan_publishable(0,0,0,0,1));
    assert(!qwzmac_scan_publishable(12,11,10,0,1)); // pending 12 is not active
    assert(qwzmac_scan_publishable(12,12,11,0,1)); // genuine running observations
    assert(!qwzmac_scan_publishable(12,12,11,0,0)); // final requires completion
    assert(qwzmac_scan_publishable(12,0,12,0,1)); // completed between poll and copy
    assert(qwzmac_scan_publishable(12,0,12,0,0));
    assert(!qwzmac_scan_publishable(12,0,12,5,1)); // failed scans cannot renew results
    assert(!qwzmac_scan_publishable(12,12,12,5,1)); // failure wins over inconsistent active ID
    assert(!qwzmac_scan_publishable(12,13,13,0,1)); // no old ticket reuse

    QwzScan::ProgressResults progress = {};
    qwzmac_scan_bss seen = {}; seen.bssid[0]=2; seen.channel=100;
    assert(progress.publish(seen,false));
    assert(!progress.publish(seen,false)); // polling a cache is not another RX event
    memcpy(seen.ssid,"test",4); seen.ssid_len=4;
    assert(progress.publish(seen,false)); // hidden beacon -> named probe response
    assert(!progress.publish(seen,false));
    seen.channel=36; assert(progress.publish(seen,false));
    seen.rssi_dbm=-40;
    assert(!progress.publish(seen,false)); // interim RSSI refresh is batched at completion
    assert(progress.publish(seen,true)); // final always supplies latest RSSI/security
    for (unsigned i=1;i<128;++i) { seen.bssid[5]=uint8_t(i); assert(progress.publish(seen,false)); }
    seen.bssid[5]=128;
    assert(!progress.publish(seen,false)); // bounded progress state
    assert(progress.publish(seen,true)); // capacity cannot suppress final snapshot
    progress.reset(); assert(progress.publish(seen,false)); // new request can rediscover it
    QwzRadio::State radio = {};
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_UP) ==
        (QwzRadio::EndReset | QwzRadio::Available));
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_UP) == 0);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_GOING_DOWN) == QwzRadio::BeginReset);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_GOING_DOWN) == 0);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_DOWN) == QwzRadio::Unavailable);
    assert(radio.resetPending && !radio.available);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_UP) ==
        (QwzRadio::EndReset | QwzRadio::Available));
    assert(!radio.resetPending && radio.available);
    // Coalesced DOWN still begins one reset; repeated DOWN cannot begin another.
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_DOWN) ==
        (QwzRadio::BeginReset | QwzRadio::Unavailable));
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_DOWN) == QwzRadio::Unavailable);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_UP) ==
        (QwzRadio::EndReset | QwzRadio::Available));
    // DOWN may be coalesced away between GOING_DOWN and UP.
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_GOING_DOWN) == QwzRadio::BeginReset);
    assert(QwzRadio::transition(radio, QWZMAC_RADIO_UP) ==
        (QwzRadio::EndReset | QwzRadio::Available));
    assert(!radio.resetPending);
    uint8_t wire[0x1550] = {};
    wire[0x40] = 1;
    AirportWCL::ScanRequest request;
    assert(!AirportWCL::decodeScan(wire, 0x57, request));
    assert(AirportWCL::decodeScan(wire, 0x58, request));
    assert(QwzScan::supported(request));
    wire[0x54] = 1;
    assert(!AirportWCL::decodeScan(wire, 0x58, request));
    wire[0x54] = 0x91; wire[0x55] = 1; // 401 channels: reject even in full buffer
    assert(!AirportWCL::decodeScan(wire, sizeof(wire), request));
    wire[0x54] = wire[0x55] = 0;
    wire[0x1c] = 33;
    assert(!AirportWCL::decodeScan(wire, sizeof(wire), request));
    wire[0x1c] = 0;
    wire[0x40] = 2;
    assert(AirportWCL::decodeScan(wire, sizeof(wire), request));
    assert(QwzScan::supported(request));
    auto passive = QwzScan::driverRequest(request);
    assert(!passive.active && !passive.restricted);
    request.channelCount = 1; request.channels[100 / 8] |= 1U << (100 % 8);
    auto restricted = QwzScan::driverRequest(request);
    assert(restricted.restricted && !restricted.active);
    assert(restricted.channels[100 / 8] == (1U << (100 % 8)));
    request.type = 1; request.ssidLength = 4;
    memcpy(request.ssid, "test", 4);
    auto directed = QwzScan::driverRequest(request);
    assert(directed.active && directed.ssid_len == 4);
    assert(!memcmp(directed.ssid, "test", 4));
    request = {}; request.type = 1;
    request.bssid[2] = 0xff;
    assert(!QwzScan::supported(request)); // mixed zero/broadcast isn't wildcard
    memset(request.bssid, 0xff, 6);
    assert(QwzScan::supported(request));

    qwzmac_scan_bss b = {};
    b.channel = 100; b.rssi_dbm = -63; b.interval = 100;
    b.capability = 0x11;
    memcpy(b.ssid, "test", 4); b.ssid_len = 4;
    // Realistic RSN IE: WPA2/CCMP PSK, PMF capable. Never rewrite these bytes.
    const uint8_t security[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,
                               1,0,0,15,172,2,128,0};
    memcpy(b.ies, security, sizeof(security)); b.ie_len = sizeof(security);
    QwzScan::Message message;
    size_t size = QwzScan::encode(b, message);
    assert(size == 0x44 + sizeof(security));
    auto bytes = reinterpret_cast<const uint8_t *>(&message);
    assert(bytes[4] == 100 && bytes[5] == 0xd0); // exact Tahoe channel spec
    assert(bytes[0x26] == 4 && bytes[0x27] == 100);
    assert(AirportWCL::read32(bytes + 0x30) == uint32_t(-63));
    assert(!memcmp(bytes + 0x44, security, sizeof(security)));
    b.ie_len = sizeof(b.ies) + 1;
    assert(!QwzScan::encode(b, message));
    b.ie_len = 1; // truncated IE header
    assert(!QwzScan::encode(b, message));
    b.ie_len = sizeof(security) - 1; // truncated security IE body
    assert(!QwzScan::encode(b, message));
    b.ie_len = sizeof(security); b.ssid_len = 33;
    assert(!QwzScan::encode(b, message));
    b.ssid_len = 4; b.channel = 0;
    assert(!QwzScan::encode(b, message));
    puts("airport scan wire/bounds/security tests: OK");
}
