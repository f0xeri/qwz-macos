/* Serialized desired-power reconciliation. GPL-2.0-only. */
#ifndef QWZ_POWER_WORKER_HPP
#define QWZ_POWER_WORKER_HPP
#include <stdint.h>
namespace QwzPower {
struct Request { int wanted; uint32_t serial; };
// The caller must serialize invocations (THREAD_CALL_OPTIONS_ONCE). Requests
// may change during slow apply(); read them again before returning. Failures
// are reported once, not retried forever when no newer request exists.
template<class Load, class Read, class Apply, class Report, class Stopping>
void drain(Load load, Read read, Apply apply, Report report, Stopping stopping)
{
    while (!stopping()) {
        Request request = load();
        int error = read() == request.wanted ? 0 : apply(request.wanted);
        report(request, read(), error);
        if (stopping() || load().serial == request.serial) return;
    }
}
}
#endif
