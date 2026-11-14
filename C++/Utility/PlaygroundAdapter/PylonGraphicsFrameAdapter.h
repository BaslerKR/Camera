#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "Camera.h"
#include "PylonScene3DProfile.h"
#include "engine/GraphicsFrame.h"
#include "engine/GraphicsImageWriter.h"

#include <cstdint>
#include <mutex>
#include <optional>

class PylonGraphicsFrameAdapter final
{
public:
    PylonGraphicsFrameAdapter() = default;

    /** Sets where 2D images are copied; empty restores storage of the adapter's own. */
    void setFrameStorage(GraphicsImageStorage storage);

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
    GraphicsImageStorage _storage;
};
