#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "Camera.h"
#include "PylonAssemblyRejection.h"
#include "PylonScene3DProfile.h"
#include "engine/GraphicsFrame.h"
#include "engine/GraphicsFrameContext.h"
#include "engine/GraphicsImageWriter.h"

#include <atomic>
#include <cstdint>
#include <optional>

class PylonGraphicsFrameAdapter final
{
public:
    PylonGraphicsFrameAdapter() = default;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* imageRejection = nullptr,
        std::optional<PylonRangeRejection>* rangeRejection = nullptr,
        GraphicsFrameContextSource* contextSource = nullptr,
        GraphicsFrameError* storageError = nullptr,
        const std::atomic<bool>* cancelled = nullptr) const;

    /** Converts one owned 2D pylon image into the canonical GraphicsImage. */
    [[nodiscard]] GraphicsImage convertGraphicsImage(
        const Pylon::CPylonImage& image,
        GraphicsFrameBuilder& builder,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* rejection = nullptr) const;

    /** Plans and converts one owned 2D pylon image through a whole-frame context transaction. */
    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsImageFrame(
        const Pylon::CPylonImage& image,
        GraphicsFrameContextSource& contextSource,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* rejection = nullptr,
        GraphicsFrameError* storageError = nullptr,
        const std::atomic<bool>* cancelled = nullptr) const;

private:
    BlazeGraphicsFrameAdapter _blazeAdapter;
};
