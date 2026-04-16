#ifndef CONVERTERPCL_H
#define CONVERTERPCL_H

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
