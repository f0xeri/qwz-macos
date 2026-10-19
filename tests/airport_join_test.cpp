#include "../airport/Join.hpp"
#include <assert.h>
#include <stdio.h>
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(8*i));
}
int main()
{
    uint8_t wire[0x2cc] = {};
    put32(wire+0x10,1); put32(wire+0x14,8);
    put32(wire+0x1c,4); memcpy(wire+0x20,"test",4);
    put32(wire+0x44,32); put32(wire+0x48,6);
    for (unsigned i=0;i<32;++i) wire[0x50+i]=uint8_t(i+1);
    put32(wire+0x214,1);
    const uint8_t peer[6]={2,3,4,5,6,7};
    memcpy(wire+0x21c,peer,6);
    wire[0x228]=100;wire[0x229]=0xd0;
    AirportWCL::JoinRequest decoded;
    qwzmac_native_join target;
    assert(AirportWCL::decodeJoin(wire,sizeof(wire),decoded));
    assert(QwzJoin::translate(decoded,target));
    assert(target.secured && target.ssid_len==4);
    assert(!memcmp(target.pmk,wire+0x50,32)); // PMK must not be PBKDF2'd again
    assert(!memcmp(target.bssid,peer,6));
    assert(!AirportWCL::decodeJoin(wire,0x229,decoded));
    put32(wire+0x214,11);
    assert(!AirportWCL::decodeJoin(wire,sizeof(wire),decoded));
    put32(wire+0x214,1); put32(wire+0x44,33);
    assert(!AirportWCL::decodeJoin(wire,sizeof(wire),decoded));
    put32(wire+0x44,32);
    assert(AirportWCL::decodeJoin(wire,sizeof(wire),decoded));
    decoded.upperAuth=4; // enterprise must never be translated into PSK
    assert(!QwzJoin::translate(decoded,target));
    decoded.upperAuth=8; decoded.keyLength=31;
    assert(!QwzJoin::translate(decoded,target));
    decoded.keyLength=32; decoded.channelSpec=0x4064;
    assert(!QwzJoin::translate(decoded,target)); // 6 GHz is unsupported
    decoded.channelSpec=0xd064; decoded.upperAuth=0; decoded.keyLength=0;
    assert(QwzJoin::translate(decoded,target) && !target.secured);
    for (auto b:target.pmk) assert(b==0);

    qwzmac_native_status status = {};
    status.station.state=4; memcpy(status.station.bssid,peer,6);
    assert(!QwzJoin::connected(status,peer)); // RUN alone is not keyed/link-up
    status.authorized=1;
    assert(!QwzJoin::connected(status,peer)); // no real associated beacon yet
    status.beacon_length=36; status.beacon[0]=0x80;
    memcpy(status.beacon+16,peer,6);
    assert(QwzJoin::connected(status,peer));
    status.beacon[16]^=2;
    assert(!QwzJoin::connected(status,peer)); // stale beacon from another AP
    status.beacon[16]^=2; status.beacon[0]=0x50;
    assert(!QwzJoin::connected(status,peer)); // probe isn't FirstBeacon
    status.beacon[0]=0x80; status.beacon_length=2049;
    assert(!QwzJoin::connected(status,peer));
    memset(target.pmk,0x5a,32); QwzJoin::wipe(&target,sizeof(target));
    for (const auto b:target.pmk) assert(b==0);
    puts("airport join wire/security/PMK/authorization/beacon tests: OK");
}
