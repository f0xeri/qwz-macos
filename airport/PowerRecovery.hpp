/* Restore the user's radio intent after real firmware recovery. GPL-2.0-only. */
#ifndef QWZ_POWER_RECOVERY_HPP
#define QWZ_POWER_RECOVERY_HPP
#include "PowerWorker.hpp"
namespace QwzPower {
// Exact APPLE80211_IOC_POWER wire data on 25G241 (setPowerHandler: 0x18).
struct Data { uint32_t version, radios, state[4]; };
static_assert(sizeof(Data) == 24, "WCL power request size");
static constexpr unsigned SetRequest = 0x802869c8;
static constexpr unsigned PowerCommand = 19;
// Posting ready is asynchronous. Wait for WCL's scan-home/away callback,
// then consume once on radioTc. DOWN cancels pending recovery.
class Recovery {
    enum { Idle, Waiting, Ready };
    unsigned state;
public:
    void cancel() { __atomic_store_n(&state, unsigned(Idle), __ATOMIC_SEQ_CST); }
    void arm() { __atomic_store_n(&state, unsigned(Waiting), __ATOMIC_SEQ_CST); }
    bool acknowledge() {
        unsigned expected = Waiting;
        return __atomic_compare_exchange_n(&state, &expected, unsigned(Ready), false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool take() {
        unsigned expected = Ready;
        return __atomic_compare_exchange_n(&state, &expected, unsigned(Idle), false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
};
// Read intent after ready callbacks so a newer OFF wins. The ordinary power
// worker acknowledges an already-ON device without restarting firmware.
template<class Load, class Send, class Fallback, class Notify, class Report>
void recover(bool acknowledged, bool ready, Load load, Send send, Fallback fallback,
    Notify notify, Report report)
{
    if (!acknowledged || !ready) return;
    Request intent = load();
    if (!intent.serial || !intent.wanted) return;
    Data data = {1, 1, {1, 0, 0, 0}};
    int result = send(SetRequest, PowerCommand, &data, sizeof(data));
    // Match Apple's setPOWER wrapper when WCL leaves the request unhandled.
    if (static_cast<uint32_t>(result) == 0xe082280f) result = fallback(&data);
    report(intent, result);
    if (result == 0) notify(); // same POWER_CHANGED completion as normal setPOWER
}
}
#endif
