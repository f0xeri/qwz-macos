/* WCL reset/availability sequencing. GPL-2.0-only. */
#ifndef QWZ_RADIO_LIFECYCLE_HPP
#define QWZ_RADIO_LIFECYCLE_HPP
#include "../compat/include/qwzmac/attach.h"
namespace QwzRadio {
enum Action { BeginReset = 1, EndReset = 2, Available = 4, Unavailable = 8 };
struct State { bool available; bool resetPending; };
inline unsigned transition(State &state, int radio)
{
    unsigned actions = 0;
    if (radio == QWZMAC_RADIO_GOING_DOWN || radio == QWZMAC_RADIO_DOWN) {
        if (state.available && !state.resetPending) {
            state.resetPending = true;
            actions |= BeginReset;
        }
        if (radio == QWZMAC_RADIO_DOWN) {
            state.available = false;
            actions |= Unavailable;
        }
    } else if (radio == QWZMAC_RADIO_UP && (!state.available || state.resetPending)) {
        // Initial ready notification also requires resetInterfacesComplete.
        state.resetPending = false;
        state.available = true;
        actions |= EndReset | Available;
    }
    return actions;
}
}
#endif
