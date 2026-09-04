#pragma once

/**
 * @file BlazeGraphicsFrameAdapter.h
 * @brief Basler blaze source adapter for neutral GraphicsFrame data.
 */

#include "engine/GraphicsFrameAdapter.h"

#include <pylon/PylonIncludes.h>

#include <cstddef>
#include <optional>

class BlazeGraphicsFrameAdapter final
    : public GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>
{
public:
    BlazeGraphicsFrameAdapter() = default;
    ~BlazeGraphicsFrameAdapter() = default;

    using GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>::convertFrame;

private:
    friend class GraphicsFrameAdapter<BlazeGraphicsFrameAdapter, Pylon::CPylonDataContainer>;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request) const;
};
