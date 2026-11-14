#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "Camera.h"
#include "PylonScene3DProfile.h"
#include "engine/GraphicsFrame.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdint>

class PylonGraphicsFrameAdapter final
{
public:
    PylonGraphicsFrameAdapter() = default;

    /**
     * Storage for one image: returns at least `bytes` writable bytes, or empty to drop the frame.
     * Must not wait; called on grab threads.
     */
    using FrameStorage = std::function<std::shared_ptr<std::uint8_t>(std::size_t bytes)>;
    /** Sets where 2D images are copied; empty restores storage of the adapter's own. */
    void setFrameStorage(FrameStorage storage);

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile) const;

    /** Converts one owned 2D pylon image into the canonical GraphicsImage. */
    [[nodiscard]] GraphicsImage convertGraphicsImage(
        const Pylon::CPylonImage& image,
        std::uint64_t frameSequence = 0U) const;

private:
    BlazeGraphicsFrameAdapter _blazeAdapter;
    mutable std::mutex _storageMutex;
    FrameStorage _storage;
};
