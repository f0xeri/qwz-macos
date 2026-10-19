/* Current link channel response, following AirportItlwm/AirportMtkWlan.
 * Plain wire types keep the translation testable outside the kernel. GPL-2.0-only. */
#ifndef QWZ_CURRENT_LINK_HPP
#define QWZ_CURRENT_LINK_HPP
#include "../compat/include/qwzmac/attach.h"
namespace QwzCurrentLink {
enum : uint32_t { Width20=2, Width40=4, Band2=8, Band5=16,
    Active=0x80, DFS=0x100, Above=0x200, Width80=0x400, Width160=0x800 };
struct Channel { uint32_t version, channelVersion, number, flags; };
inline bool channel(const qwzmac_native_status &s, Channel &out) {
    out = {};
    if (s.station.state != 4 || !s.station.channel || s.station.channel > 196) return false;
    uint32_t band = s.channel_flags & (QWZMAC_CHAN_2GHZ | QWZMAC_CHAN_5GHZ);
    if (band != QWZMAC_CHAN_2GHZ && band != QWZMAC_CHAN_5GHZ) return false;
    uint32_t width;
    switch (s.station.width_mhz) {
    case 20: width=Width20; break;
    case 40: width=Width40; break;
    case 80: width=Width80; break;
    case 160: width=Width160; break;
    default: return false;
    }
    out = {1, 1, s.station.channel, width | (band == QWZMAC_CHAN_5GHZ ? Band5 : Band2)};
    if (!(s.channel_flags & QWZMAC_CHAN_PASSIVE)) out.flags |= Active;
    if (s.channel_flags & QWZMAC_CHAN_DFS) out.flags |= DFS;
    if (s.station.width_mhz == 40 && (s.channel_flags & QWZMAC_CHAN_EXT_ABOVE)) out.flags |= Above;
    return true;
}
}
#endif
