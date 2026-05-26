#pragma once

#include "BlazeScene3DAdapter.h"
#include "PylonScene3DProfile.h"

#include <optional>

class PylonScene3DAdapter final
{
public:
    [[nodiscard]] std::optional<GraphicsScene3D> convert(
        const Pylon::CPylonDataContainer& container,
        const GraphicsScene3DRequest& request,
        const PylonScene3DProfile& profile) const;

private:
    BlazeScene3DAdapter _blazeAdapter;
};
