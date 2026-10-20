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
// Controller postMessage queues both PostOffice and Glue work. Its return
// does not acknowledge WCL readiness. Arm before posting ready; acknowledge
// only from the fwLoadComplete scan-home/away callback, then consume once on
// radioTc. A new DOWN cancels the old recovery, including a queued callback.
class Recovery {
    unsigned state;
public:
    void cancel() { __atomic_store_n(&state, 0u, __ATOMIC_SEQ_CST); }
    void arm() { __atomic_store_n(&state, 1u, __ATOMIC_SEQ_CST); }
    bool acknowledge() {
        unsigned expected = 1;
        return __atomic_compare_exchange_n(&state, &expected, 2u, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool take() {
        unsigned expected = 2;
        return __atomic_compare_exchange_n(&state, &expected, 0u, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
};
// Load intent AFTER ready callbacks: they can synchronously issue a newer OFF.
// The sender re-enters WCL, not the hardware power path. A repeated actual ON
// is acknowledged by the ordinary power worker without restarting firmware.
template<class Load, class Send, class Fallback, class Notify, class Report>
void recover(bool acknowledged, bool ready, Load load, Send send, Fallback fallback,
    Notify notify, Report report)
{
    if (!acknowledged || !ready) return;
    Request intent = load();
    if (!intent.serial || !intent.wanted) return;
    Data data = {1, 1, {1, 0, 0, 0}};
    int result = send(SetRequest, PowerCommand, &data, sizeof(data));
    // SSM observes POWER but may leave it unhandled. Apple's normal wrapper
    // then calls the driver's setPOWER; this is not a failed power request.
    if (static_cast<uint32_t>(result) == 0xe082280f) result = fallback(&data);
    report(intent, result);
    if (result == 0) notify(); // same POWER_CHANGED completion as normal setPOWER
}
}
#endif
