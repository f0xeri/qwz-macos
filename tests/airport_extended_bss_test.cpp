#include "../airport/ExtendedBss.hpp"
#include <assert.h>
#include <stdio.h>
static uint32_t read32(const uint8_t *p) { return p[0] | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
int main() {
    qwzmac_native_status s = {};
    uint8_t guarded[QwzExtendedBss::Size+16]; memset(guarded,0xa5,sizeof(guarded));
    auto out = guarded+8;
    assert(!QwzExtendedBss::encode(s,out,QwzExtendedBss::Size));
    s.station.state=4; s.rate_count=2; s.rates[0]=0x8c; s.rates[1]=108;
    s.ht_mcs[0]=0xff; s.ht_mcs[1]=0xff; s.vht_mcs=0xffff; s.he_mcs=0xffff;
    const uint8_t rsn[]={48,4,1,0,0,0}; memcpy(s.negotiated_rsn,rsn,sizeof(rsn)); s.negotiated_rsn_length=sizeof(rsn);
    assert(!QwzExtendedBss::encode(s,out,QwzExtendedBss::Size-1));
    assert(QwzExtendedBss::encode(s,out,QwzExtendedBss::Size));
    assert(read32(out)==1 && out[4]==2 && read32(out+8)==1 && read32(out+12)==0x8c);
    assert(read32(out+24)==108 && read32(out+0xbc)==1 && out[0xc0]==0xff && out[0xc1]==0xff);
    assert(read32(out+0xcc)==1 && out[0xd0]==0xff && out[0xd1]==0xff);
    assert(read32(out+0xd4)==1 && out[0xd8]==0xff && out[0xd9]==0xff);
    assert(!memcmp(out+0x113,rsn,sizeof(rsn)));
    for(unsigned i=0;i<8;i++) assert(guarded[i]==0xa5 && guarded[sizeof(guarded)-1-i]==0xa5);
    s.rate_count=32; s.negotiated_rsn_length=257; memset(s.negotiated_rsn,0x5a,257);
    assert(QwzExtendedBss::encode(s,out,QwzExtendedBss::Size) && out[4]==15 && out[0x213]==0x5a);
    s.negotiated_rsn_length=258; assert(!QwzExtendedBss::encode(s,out,QwzExtendedBss::Size));
    s.negotiated_rsn_length=0; s.rate_count=2;
    assert(QwzExtendedBss::encode(s,out,QwzExtendedBss::Size) && out[0x113]==0);
    puts("airport extended BSS rates/MCS/RSN/bounds tests: OK");
}
