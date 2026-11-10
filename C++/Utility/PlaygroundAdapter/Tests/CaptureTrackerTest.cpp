#include "CameraCaptureTracker.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

void captureSequenceIsSdkNumber()
{
    CameraCaptureTracker tracker;
    const std::uint64_t generation = tracker.beginRun(false);

    // LatestImageOnly skips block ids; the sequence follows them, not delivery count.
    auto capture = tracker.noteDelivered(10, 0);
    assert(capture.valid);
    assert(capture.generation == generation);
    assert(capture.sequence == 10);
    capture = tracker.noteDelivered(14, 3);
    assert(capture.sequence == 14);
    assert(tracker.counter().has_value());
    assert(tracker.counter()->generation == generation);
    assert(tracker.counter()->value == 14);
    assert(tracker.dropCount() == 3);

    // GigE Vision 1.x block ids wrap from 65535 to 1; the sequence stays monotonic.
    tracker.noteDelivered(65535, 0);
    capture = tracker.noteDelivered(1, 0);
    assert(capture.valid);
    assert(capture.sequence == 65536);

    // A repeated id (no block-id support) or UINT64_MAX is not capture evidence.
    assert(!tracker.noteDelivered(1, 0).valid);
    assert(!tracker.noteDelivered(CameraCaptureTracker::kUnavailableBlockId, 0).valid);
    assert(tracker.counter()->value == 65536);

    tracker.noteFailedResult();
    assert(tracker.dropCount() == 4);
    tracker.endRun();
    assert(!tracker.counter().has_value());
}

void generationBumpsOnRestart()
{
    CameraCaptureTracker tracker;
    const std::uint64_t first = tracker.beginRun(false);
    tracker.noteDelivered(500, 0);
    tracker.endRun();

    const std::uint64_t second = tracker.beginRun(false);
    assert(second > first);
    // Block ids restart with the run, and so does the counter.
    assert(tracker.counter()->generation == second);
    assert(tracker.counter()->value == 0);
    const auto capture = tracker.noteDelivered(1, 0);
    assert(capture.generation == second);
    assert(capture.sequence == 1);
}

void softwareTriggerLabelsTheNextFrame()
{
    CameraCaptureTracker tracker;
    tracker.beginRun(false);
    assert(!tracker.softwareTriggerAvailable());
    assert(!tracker.requestTrigger([] { return true; }).has_value());
    tracker.endRun();

    const std::uint64_t generation = tracker.beginRun(true);
    assert(tracker.softwareTriggerAvailable());
    const auto mark = tracker.requestTrigger([] { return true; });
    assert(mark.has_value());
    assert(mark->generation == generation);
    const auto capture = tracker.noteDelivered(7, 0);
    assert(capture.triggerId == mark->value);

    // A failed trigger reports no id.
    assert(!tracker.requestTrigger([] { return false; }).has_value());
    tracker.endRun();
    assert(!tracker.softwareTriggerAvailable());
    assert(!tracker.requestTrigger([] { return true; }).has_value());
}

void captureCounterNonBlocking()
{
    CameraCaptureTracker tracker;
    tracker.beginRun(true);

    // The grab worker records results and a trigger request holds the trigger
    // lock; neither delays the lock-free counter read.
    std::atomic<bool> stop{false};
    std::thread grab([&] {
        std::uint64_t block = 1;
        while (!stop.load(std::memory_order_acquire)) {
            tracker.noteDelivered(block++, 0);
        }
    });
    std::thread trigger([&] {
        tracker.requestTrigger([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return true;
        });
    });

    std::vector<std::chrono::steady_clock::duration> samples;
    samples.reserve(1000);
    for (int index = 0; index < 1000; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto mark = tracker.counter();
        samples.push_back(std::chrono::steady_clock::now() - started);
        assert(mark.has_value());
    }
    stop.store(true, std::memory_order_release);
    grab.join();
    trigger.join();
    // p99: a shared CI runner can preempt any single call, which is not a lock wait.
    std::sort(samples.begin(), samples.end());
    assert(samples[samples.size() * 99 / 100] < std::chrono::milliseconds(1));
}

} // namespace

int main()
{
    captureSequenceIsSdkNumber();
    generationBumpsOnRestart();
    softwareTriggerLabelsTheNextFrame();
    captureCounterNonBlocking();
    return 0;
}
