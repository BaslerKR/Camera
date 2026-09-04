#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "PylonScene3DProfile.h"
#include "engine/GraphicsFrame.h"

#include <optional>

class PylonGraphicsFrameAdapter final
{
public:
    PylonGraphicsFrameAdapter() = default;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile) const;

private:
    BlazeGraphicsFrameAdapter _blazeAdapter;
};
