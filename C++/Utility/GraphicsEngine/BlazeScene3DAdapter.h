#pragma once

/**
 * @file BlazeScene3DAdapter.h
 * @brief Basler blaze 3D source adapter for neutral GraphicsEngine scene data.
 */

#include "engine/Scene3DAdapter.h"
#include "engine/GraphicsSceneTypes.h"

#include <pylon/PylonIncludes.h>

#include <cstddef>
#include <optional>

class BlazeScene3DAdapter final
    : public Scene3DAdapter<BlazeScene3DAdapter, Pylon::CPylonDataContainer>
{
public:
    BlazeScene3DAdapter() = default;
    ~BlazeScene3DAdapter() = default;

    using Scene3DAdapter<BlazeScene3DAdapter, Pylon::CPylonDataContainer>::convert;

private:
    friend class Scene3DAdapter<BlazeScene3DAdapter, Pylon::CPylonDataContainer>;

    [[nodiscard]] std::optional<GraphicsScene3D> convertScene3D(
        const Pylon::CPylonDataContainer& container,
        const GraphicsScene3DRequest& request) const;
};
