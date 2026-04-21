#ifndef CONVERTERPCL_H
#define CONVERTERPCL_H

/**
 * @file PCLConverter.h
 * @brief Converts Basler pylon 3D image containers into PCL XYZRGB point clouds.
 *
 * This is an internal Camera utility layer and does not expose PCL types through
 * the GraphicsEngine public API.
 */

#include <pylon/PylonIncludes.h>

#include <cstdint>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

enum class PCLColorSource
{
    Auto,
    Intensity,
    None
};

struct PCLConversionOptions
{
    PCLColorSource colorSource = PCLColorSource::Auto;
    bool preferIntensity = true;
    bool ignoreInvalidPoints = false;
    std::uint8_t defaultColor = 255;
};

pcl::PointCloud<pcl::PointXYZRGB>::Ptr convertPylon3DImageToPCL(
    const Pylon::CPylonDataContainer& container,
    const PCLConversionOptions& options = {}
);

#endif // CONVERTERPCL_H
