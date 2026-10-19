#include "../airport/PowerWorker.hpp"
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
    puts("airport power overlap/latest-target/error tests: OK");
}
