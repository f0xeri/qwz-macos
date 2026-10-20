#include "../airport/PowerRecovery.hpp"
#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <stdio.h>
int main()
{
    std::mutex lock;
    std::condition_variable cv;
    QwzPower::Request wanted = {0, 1};
    bool offStarted = false, releaseOff = false;
    std::atomic<int> actual{1};
    std::vector<int> applied;
    unsigned completed = 0;
    auto load = [&] { std::lock_guard<std::mutex> g(lock); return wanted; };
    std::thread worker([&] {
        QwzPower::drain(load, [&] { return actual.load(); }, [&](int on) {
            applied.push_back(on);
            if (!on) {
                std::unique_lock<std::mutex> g(lock);
                offStarted = true; cv.notify_all();
                cv.wait(g, [&] { return releaseOff; });
            }
            actual = on;
            return 0;
        }, [&](QwzPower::Request r, int observed, int error) {
            assert(!error && observed == r.wanted); completed = r.serial;
        }, [] { return false; });
    });
    {
        std::unique_lock<std::mutex> g(lock);
        cv.wait(g, [&] { return offStarted; });
        // An ON arriving while hardware still reports ON must not be lost
        // merely because the older OFF hasn't completed yet.
        assert(actual == 1);
        wanted = {1, 2}; releaseOff = true;
    }
    cv.notify_all(); worker.join();
    assert(actual == 1 && completed == 2);
    assert((applied == std::vector<int>{0, 1}));
    // A duplicate request is acknowledged without touching hardware.
    QwzPower::drain(load, [&] { return actual.load(); }, [](int) {
        assert(false); return 0;
    }, [&](QwzPower::Request r, int observed, int error) {
        assert(r.serial == 2 && observed == 1 && !error);
    }, [] { return false; });
    // Failure is reported, not retried forever without a newer request.
    int attempts = 0, reports = 0;
    QwzPower::drain([] { return QwzPower::Request{0, 3}; }, [] { return 1; },
        [&](int) { ++attempts; return 16; },
        [&](QwzPower::Request, int observed, int error) {
            assert(observed == 1 && error == 16); ++reports;
        }, [] { return false; });
    assert(attempts == 1 && reports == 1);
    // Firmware recovered after WCL deferred sleep+awake. Reapply an actual
    // prior ON through WCL, and let the normal worker see hardware already ON.
    QwzPower::Request intent = {1, 40};
    int sends = 0, replayReports = 0, hardwareChanges = 0, loads = 0, notifications = 0;
    auto readIntent = [&] { ++loads; return intent; };
    auto route = [&](unsigned request, unsigned command, QwzPower::Data *data, size_t size) {
        ++sends;
        assert(request == 0x802869c8 && command == 19 && size == 24);
        assert(data->version == 1 && data->radios == 1 && data->state[0] == 1);
        assert(!data->state[1] && !data->state[2] && !data->state[3]);
        // Simulate the ordinary setPOWER callback and its existing worker.
        ++intent.serial;
        QwzPower::drain(readIntent, [] { return 1; }, [&](int) {
            ++hardwareChanges; return 0;
        }, [](QwzPower::Request, int actual, int error) {
            assert(actual == 1 && error == 0);
        }, [] { return false; });
        return 0;
    };
    auto fallback = [&](QwzPower::Data *) { assert(false); return 0; };
    auto report = [&](QwzPower::Request captured, int result) {
        assert(captured.wanted == 1 && result == 0); ++replayReports;
    };
    auto notify = [&] { assert(sends == replayReports); ++notifications; };
    // Initial publication and an unsuccessful wake must never synthesize ON.
    QwzPower::recover(false, true, readIntent, route, fallback, notify, report);
    QwzPower::recover(true, false, readIntent, route, fallback, notify, report);
    assert(sends == 0 && loads == 0);
    QwzPower::recover(true, true, readIntent, route, fallback, notify, report);
    assert(sends == 1 && replayReports == 1 && hardwareChanges == 0 && notifications == 1);
    // An OFF issued by a ready callback must win over the old ON intent.
    intent = {0, 42};
    QwzPower::recover(true, true, readIntent, route, fallback, notify, report);
    // No previous request, or teardown, is represented by serial=0.
    intent = {1, 0};
    QwzPower::recover(true, true, readIntent, route, fallback, notify, report);
    assert(sends == 1 && hardwareChanges == 0);
    // Rejected recovery is reported once; no polling/retry loop or OFF/ON.
    intent = {1, 43};
    QwzPower::recover(true, true, readIntent,
        [&](unsigned, unsigned, QwzPower::Data *, size_t) { ++sends; return 5; }, fallback, notify,
        [&](QwzPower::Request r, int error) {
            assert(r.serial == 43 && error == 5); ++replayReports;
        });
    assert(sends == 2 && replayReports == 2 && hardwareChanges == 0 && notifications == 1);
    // A posted ready is asynchronous: no recovery until the callback that
    // WCL issues from fwLoadComplete; a duplicate UP cannot bypass this.
    QwzPower::Recovery recovery;
    recovery.cancel();
    assert(!recovery.acknowledge() && !recovery.take());
    recovery.arm();
    assert(!recovery.take()); // ready queued, SSM still DEFER (0.3.10 bug)
    assert(recovery.acknowledge()); // process queued FW_LOADED
    assert(!recovery.acknowledge()); // duplicate callback
    assert(recovery.take() && !recovery.take());
    recovery.arm();
    assert(recovery.acknowledge());
    recovery.cancel(); // DOWN before the queued worker: no obsolete replay
    assert(!recovery.take() && !recovery.acknowledge());
    // The actual SSM observes POWER yet leaves routing unhandled. Match the
    // native wrapper's driver fallback and success notification exactly once.
    int fallbacks = 0;
    QwzPower::recover(true, true, readIntent,
        [&](unsigned, unsigned, QwzPower::Data *, size_t) {
            return static_cast<int>(0xe082280f);
        }, [&](QwzPower::Data *data) {
            assert(data->state[0] == 1); ++fallbacks; return 0;
        }, [&] { ++notifications; }, [](QwzPower::Request, int result) {
            assert(result == 0);
        });
    assert(fallbacks == 1 && notifications == 2);
    QwzPower::recover(true, true, readIntent,
        [](unsigned, unsigned, QwzPower::Data *, size_t) {
            return static_cast<int>(0xe082280f);
        }, [&](QwzPower::Data *) { ++fallbacks; return 16; },
        [&] { assert(false); }, [](QwzPower::Request, int result) {
            assert(result == 16);
        });
    assert(fallbacks == 2 && notifications == 2);
    puts("airport power overlap/latest-target/error/recovery tests: OK");
}
