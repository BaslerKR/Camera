#ifndef CAMERA_CAPTURE_TRACKER_H
#define CAMERA_CAPTURE_TRACKER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>

/**
 * @brief SDK-independent capture identity of one delivered frame.
 *
 * `sequence` is derived only from the pylon block id of the grab result: a
 * block id that moves backwards (the 16-bit GigE Vision 1.x id wraps from
 * 65535 to 1) is unwrapped so the sequence stays monotonic within one run. A
 * repeated block id (a GigE device without block-id support reports 0 for every
 * frame) or `UINT64_MAX` is not capture evidence and leaves `valid` false. It is
 * never a module delivery counter.
 */
struct CameraFrameCapture
{
    bool valid = false;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t triggerId = 0;
};

/**
 * @brief Tracks the acquisition run, the newest delivered block id and software triggers.
 *
 * The grab worker calls beginRun(), noteDelivered() and endRun(). Any thread may
 * read counter() and dropCount(); both are lock-free. requestTrigger() serializes
 * with endRun() so a trigger never reaches a device whose run has ended.
 */
class CameraCaptureTracker final
{
public:
    /** @brief Mark read at a consumer free boundary. */
    struct Mark
    {
        std::uint64_t generation = 0;
        std::uint64_t value = 0;
    };

    /** @brief Pylon reports an unavailable block id as the maximum value. */
    static constexpr std::uint64_t kUnavailableBlockId = std::numeric_limits<std::uint64_t>::max();

    /**
     * @brief Starts a new run and returns its generation.
     * @param softwareTrigger Whether the run accepts software triggers.
     */
    std::uint64_t beginRun(const bool softwareTrigger)
    {
        std::lock_guard<std::mutex> lock(_triggerMutex);
        _lastRawBlockId = 0;
        _blockIdOffset = 0;
        _seenBlockId = false;
        _lastSequence.store(0, std::memory_order_release);
        _issuedTrigger.store(0, std::memory_order_release);
        _softwareTrigger.store(softwareTrigger, std::memory_order_release);
        const std::uint64_t generation = _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        _running.store(true, std::memory_order_release);
        return generation;
    }

    /** @brief Ends the run; waits only for an in-flight trigger request. */
    void endRun()
    {
        std::lock_guard<std::mutex> lock(_triggerMutex);
        _running.store(false, std::memory_order_release);
        _softwareTrigger.store(false, std::memory_order_release);
    }

    /**
     * @brief Records one successful grab result on the grab worker.
     * @param rawBlockId Pylon `GetBlockID()` of the result.
     * @param skippedImages Pylon `GetNumberOfSkippedImages()` of the result.
     */
    CameraFrameCapture noteDelivered(const std::uint64_t rawBlockId, const std::uint64_t skippedImages)
    {
        _dropped.fetch_add(skippedImages, std::memory_order_relaxed);
        CameraFrameCapture capture;
        capture.generation = _generation.load(std::memory_order_acquire);
        if (rawBlockId == kUnavailableBlockId
            || (_seenBlockId && rawBlockId == _lastRawBlockId))
        {
            return capture;
        }
        if (_seenBlockId && rawBlockId < _lastRawBlockId)
        {
            // Continue the sequence across a block-id wrap.
            _blockIdOffset += _lastRawBlockId;
        }
        _lastRawBlockId = rawBlockId;
        _seenBlockId = true;
        capture.valid = true;
        capture.sequence = _blockIdOffset + rawBlockId;
        if (_softwareTrigger.load(std::memory_order_acquire))
        {
            capture.triggerId = _issuedTrigger.load(std::memory_order_acquire);
        }
        _lastSequence.store(capture.sequence, std::memory_order_release);
        return capture;
    }

    /** @brief Counts a failed or incomplete grab result (for example a buffer underrun). */
    void noteFailedResult() noexcept
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief Returns the newest delivered sequence of the running generation without waiting. */
    [[nodiscard]] std::optional<Mark> counter() const noexcept
    {
        if (!_running.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }
        return Mark{_generation.load(std::memory_order_acquire),
                    _lastSequence.load(std::memory_order_acquire)};
    }

    /** @brief Returns whether the running generation accepts software triggers. */
    [[nodiscard]] bool softwareTriggerAvailable() const noexcept
    {
        return _running.load(std::memory_order_acquire)
            && _softwareTrigger.load(std::memory_order_acquire);
    }

    /**
     * @brief Issues one software trigger.
     * @param execute Sends the trigger to the device; returns false on failure.
     * @return The generation and trigger id that the next delivered frame carries.
     */
    std::optional<Mark> requestTrigger(const std::function<bool()>& execute)
    {
        std::lock_guard<std::mutex> lock(_triggerMutex);
        if (!_running.load(std::memory_order_acquire)
            || !_softwareTrigger.load(std::memory_order_acquire) || !execute)
        {
            return std::nullopt;
        }
        // Publish the id before the trigger so the frame it produces carries it.
        const std::uint64_t id = _issuedTrigger.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!execute())
        {
            return std::nullopt;
        }
        return Mark{_generation.load(std::memory_order_acquire), id};
    }

    /** @brief Returns pylon skipped images plus failed grab results. */
    [[nodiscard]] std::uint64_t dropCount() const noexcept
    {
        return _dropped.load(std::memory_order_relaxed);
    }

private:
    std::mutex _triggerMutex;
    std::atomic<bool> _running{false};
    std::atomic<bool> _softwareTrigger{false};
    std::atomic<std::uint64_t> _generation{0};
    std::atomic<std::uint64_t> _lastSequence{0};
    std::atomic<std::uint64_t> _issuedTrigger{0};
    std::atomic<std::uint64_t> _dropped{0};
    // Grab-worker state.
    std::uint64_t _lastRawBlockId = 0;
    std::uint64_t _blockIdOffset = 0;
    bool _seenBlockId = false;
};

#endif // CAMERA_CAPTURE_TRACKER_H
