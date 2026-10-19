#include "../airport/CurrentLink.hpp"
#include <assert.h>
#include <stdio.h>
int main() {
    qwzmac_native_status s = {};
    QwzCurrentLink::Channel out;
    assert(!QwzCurrentLink::channel(s,out));
    s.station.state=4; s.station.channel=100; s.station.width_mhz=20;
    s.channel_flags=QWZMAC_CHAN_5GHZ | QWZMAC_CHAN_PASSIVE | QWZMAC_CHAN_DFS;
    assert(QwzCurrentLink::channel(s,out));
    assert(out.version==1 && out.channelVersion==1 && out.number==100);
    assert(out.flags==(QwzCurrentLink::Band5 | QwzCurrentLink::Width20 | QwzCurrentLink::DFS));
    assert(!(out.flags & QwzCurrentLink::Active)); // DFS/passive is not an active channel
    s.station.channel=13; s.channel_flags=QWZMAC_CHAN_2GHZ;
    assert(QwzCurrentLink::channel(s,out));
    assert(out.flags==(QwzCurrentLink::Band2 | QwzCurrentLink::Width20 | QwzCurrentLink::Active));
    s.station.width_mhz=40; s.channel_flags|=QWZMAC_CHAN_EXT_ABOVE;
    assert(QwzCurrentLink::channel(s,out) && (out.flags & QwzCurrentLink::Above));
    assert((out.flags & QwzCurrentLink::Width40) && !(out.flags & QwzCurrentLink::Width20));
    s.station.width_mhz=20;
    assert(QwzCurrentLink::channel(s,out) && !(out.flags & QwzCurrentLink::Above));
    s.station.width_mhz=80; s.station.channel=36; s.channel_flags=QWZMAC_CHAN_5GHZ;
    assert(QwzCurrentLink::channel(s,out) && (out.flags & QwzCurrentLink::Width80));
    s.station.width_mhz=160;
    assert(QwzCurrentLink::channel(s,out) && (out.flags & QwzCurrentLink::Width160));
    s.station.width_mhz=0;
    assert(!QwzCurrentLink::channel(s,out) && !out.number && !out.flags);
    s.station.width_mhz=20; s.station.channel=0;
    assert(!QwzCurrentLink::channel(s,out));
    s.station.channel=197;
    assert(!QwzCurrentLink::channel(s,out));
    s.station.channel=100; s.channel_flags=0;
    assert(!QwzCurrentLink::channel(s,out));
    s.channel_flags=QWZMAC_CHAN_2GHZ | QWZMAC_CHAN_5GHZ;
    assert(!QwzCurrentLink::channel(s,out));
    s.channel_flags=QWZMAC_CHAN_5GHZ;
    assert(QwzCurrentLink::channel(s,out));
    s.station.state=1;
    assert(!QwzCurrentLink::channel(s,out) && !out.number); // no stale channel after leave
    puts("airport current link channel/width/band/passive/DFS/stale-state tests: OK");
}
