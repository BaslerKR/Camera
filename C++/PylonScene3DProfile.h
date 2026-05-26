#pragma once

/**
 * @file PylonScene3DProfile.h
 * @brief Stream configuration facts required to decode pylon 3D frames.
 */

struct PylonScene3DProfile
{
    enum class DeviceFamily
    {
        Image2D,
        Blaze,
        StereoAce,
        StereoMini
    };

    enum class GeometryKind
    {
        None,
        DirectXyzRange,
        DisparityReconstruction
    };

    DeviceFamily family = DeviceFamily::Image2D;
    GeometryKind geometry = GeometryKind::None;
    bool colorRegisteredToRange = false;
    double coordinateScale = 0.0;
    double coordinateOffset = 0.0;
    double baseline = 0.0;
    double focalLength = 0.0;
    double principalPointU = 0.0;
    double principalPointV = 0.0;

    [[nodiscard]] bool hasDisparityCalibration() const noexcept
    {
        return geometry == GeometryKind::DisparityReconstruction
            && coordinateScale != 0.0
            && baseline != 0.0
            && focalLength != 0.0;
    }
};
