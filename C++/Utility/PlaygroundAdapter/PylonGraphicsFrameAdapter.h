#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "Camera.h"
#include "PylonAssemblyRejection.h"
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
    void setFrameStorage(GraphicsFrameContext storage);

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* imageRejection = nullptr,
        std::optional<PylonRangeRejection>* rangeRejection = nullptr) const;

    /** Converts one owned 2D pylon image into the canonical GraphicsImage. */
    [[nodiscard]] GraphicsImage convertGraphicsImage(
        const Pylon::CPylonImage& image,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* rejection = nullptr) const;

private:
    BlazeGraphicsFrameAdapter _blazeAdapter;
    mutable std::mutex _storageMutex;
    GraphicsFrameContext _storage = GraphicsFrameContext::local();
};
