#pragma once

/**
 * @file BlazeGraphicsFrameAdapter.h
 * @brief Basler blaze source adapter for neutral GraphicsFrame data.
 */

#include "PylonAssemblyRejection.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <pylon/PylonIncludes.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

/** @brief One scalar component view mapped from a pylon data container. */
struct BlazeScalarSource
{
    const std::uint8_t* data = nullptr;
    std::size_t stride = 0;
    Pylon::EPixelType pixelType = Pylon::PixelType_Mono8;
};

/**
 * @brief Neutral source data for the blaze conversion.
 *
 * The pylon adapter fills this from its data container; tests build the same
 * view directly so the conversion math is covered without a device.
 */
struct BlazeRangeSource
{
    /** Coord3D_ABC32f samples. */
    const std::uint8_t* coordinates = nullptr;
    std::size_t coordinateStride = 0;
    int width = 0;
    int height = 0;
    /** Producer pixel-format identifier reported alongside the range component. */
    Pylon::EPixelType rangePixelType = Pylon::PixelType_Coord3D_ABC32f;
    BlazeScalarSource intensity;
    BlazeScalarSource confidence;
    bool hasIntensity = false;
    bool hasConfidence = false;
    /** SDK-reported stride and readable extent. Zero denotes an unavailable value. */
    std::size_t reportedCoordinateStride = 0;
    std::size_t coordinateReadableBytes = 0;
};

class BlazeGraphicsFrameAdapter final
    : public GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>
{
public:
    BlazeGraphicsFrameAdapter() = default;
    ~BlazeGraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>::convertFrame;

    /** Sets where range samples are written; empty restores storage of the adapter's own. */
    void setFrameStorage(GraphicsFrameContext storage);

    /** Converts a neutral source view; the pylon adapter and tests share it. */
    [[nodiscard]] std::optional<GraphicsFrame> convertSource(
        const BlazeRangeSource& source,
        const GraphicsFrameRequest& request,
        std::uint64_t frameSequence = 0U,
        std::optional<PylonRangeRejection>* rejection = nullptr) const;

    /** Converts a container and optionally reports the first failed range rule. */
    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        std::uint64_t frameSequence,
        std::optional<PylonRangeRejection>* rejection) const;

private:
    friend class GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request) const;

    mutable std::mutex _storageMutex;
    GraphicsFrameContext _storage = GraphicsFrameContext::local();
};
