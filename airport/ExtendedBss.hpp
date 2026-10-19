/* Native 25G241 extended BSS layout, as AirportMtkWlan copyWCLExtendedBss.
 * Carries real negotiated node data only; no invented HT/VHT/HE capability. */
#ifndef QWZ_EXTENDED_BSS_HPP
#define QWZ_EXTENDED_BSS_HPP
#include "../compat/include/qwzmac/attach.h"
#include <string.h>
namespace QwzExtendedBss {
constexpr size_t Size = 0x214;
inline void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=uint8_t(v>>(8*i)); }
inline void put16(uint8_t *p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
inline bool encode(const qwzmac_native_status &s, void *buffer, size_t size) {
    if (!buffer || size < Size || s.station.state != 4 || !s.rate_count ||
        s.rate_count > sizeof(s.rates) || s.negotiated_rsn_length > sizeof(s.negotiated_rsn)) return false;
    auto out = static_cast<uint8_t *>(buffer);
    memset(out,0,Size);
    put32(out,1);
    unsigned count = s.rate_count < 15 ? s.rate_count : 15;
    put16(out+4,uint16_t(count));
    for (unsigned i=0;i<count;i++) { put32(out+8+12*i,1); put32(out+12+12*i,s.rates[i]); }
    put32(out+0xbc,1); memcpy(out+0xc0,s.ht_mcs,sizeof(s.ht_mcs));
    put32(out+0xcc,1); put16(out+0xd0,s.vht_mcs);
    put32(out+0xd4,1); put16(out+0xd8,s.he_mcs);
    memcpy(out+0x113,s.negotiated_rsn,s.negotiated_rsn_length);
    return true;
}
}
#endif
