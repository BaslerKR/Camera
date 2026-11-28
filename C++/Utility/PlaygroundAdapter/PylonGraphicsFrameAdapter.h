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

/** Non-owning SDK part view; payload stays owned by the caller through conversion. */
struct CameraStereoPartView
{
    const void* data = nullptr;
    std::size_t bytes = 0, width = 0, height = 0, stride = 0, padding = 0;
    Pylon::EPixelType pixelType = Pylon::PixelType_Undefined;
    bool valid = false, hasStride = false;
    bool IsValid() const noexcept { return valid; }
    const void* GetData() const noexcept { return data; }
    std::size_t GetDataSize() const noexcept { return bytes; }
    std::size_t GetWidth() const noexcept { return width; }
    std::size_t GetHeight() const noexcept { return height; }
    std::size_t GetPaddingX() const noexcept { return padding; }
    Pylon::EPixelType GetPixelType() const noexcept { return pixelType; }
    bool GetStride(std::size_t& out) const noexcept { out = stride; return hasStride; }
};
struct CameraStereoParts { CameraStereoPartView intensity, range, disparity; };

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
        const std::atomic<bool>* cancelled = nullptr,
        PylonFrameAnomalies* anomalies = nullptr) const;

    /** Converts non-owning Stereo part views while their SDK container is retained. */
    [[nodiscard]] std::optional<GraphicsFrame> convertStereoParts(
        const CameraStereoParts& parts, const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile, GraphicsFrameContextSource& contextSource,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonImageRejection>* imageRejection = nullptr,
        std::optional<PylonRangeRejection>* rangeRejection = nullptr,
        GraphicsFrameError* storageError = nullptr,
        const std::atomic<bool>* cancelled = nullptr) const;

    /** Writes a non-owning image part, retaining the caller's bytes for this call. */
    [[nodiscard]] std::optional<GraphicsFrame> convertImageViewFrame(
        const CameraStereoPartView& part, GraphicsFrameContextSource& contextSource,
        std::uint64_t frameSequence = 0U, std::optional<PylonImageRejection>* rejection = nullptr,
        GraphicsFrameError* storageError = nullptr, const std::atomic<bool>* cancelled = nullptr) const;

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
