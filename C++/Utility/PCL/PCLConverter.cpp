#include "PCLConverter.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

struct Coord3DPoint
{
    float x;
    float y;
    float z;
};

Pylon::CPylonDataComponent findComponentByType(const Pylon::CPylonDataContainer& container,
                                               const Pylon::EComponentType type)
{
    for (size_t i = 0; i < container.GetDataComponentCount(); ++i) {
        const auto component = container.GetDataComponent(i);
        if (component.IsValid() && component.GetComponentType() == type) {
            return component;
        }
    }
    return {};
}

Pylon::CPylonDataComponent findIntensityComponent(const Pylon::CPylonDataContainer& container,
                                                  const PCLConversionOptions& options)
{
    if (!options.preferIntensity && options.colorSource != PCLColorSource::Intensity) {
        return {};
    }

    auto intensity = findComponentByType(container, Pylon::ComponentType_Intensity);
    if (!intensity.IsValid()) {
        intensity = findComponentByType(container, Pylon::ComponentType_IntensityCombined_STA);
    }
    return intensity;
}

bool supportsIntensity(const Pylon::CPylonDataComponent& component)
{
    return component.IsValid()
           && (component.GetPixelType() == Pylon::PixelType_Mono8
               || component.GetPixelType() == Pylon::PixelType_Mono16);
}

std::uint8_t readGrayValue(const Pylon::CPylonDataComponent& intensity, const size_t index, const std::uint8_t fallback)
{
    if (!supportsIntensity(intensity)) {
        return fallback;
    }

    if (intensity.GetPixelType() == Pylon::PixelType_Mono8) {
        const auto* src = static_cast<const std::uint8_t*>(intensity.GetData());
        return src ? src[index] : fallback;
    }

    const auto* src = static_cast<const std::uint16_t*>(intensity.GetData());
    return src ? static_cast<std::uint8_t>(src[index] >> 8) : fallback;
}

bool shouldUseIntensity(const PCLConversionOptions& options, const Pylon::CPylonDataComponent& intensity)
{
    switch (options.colorSource) {
    case PCLColorSource::Intensity:
        return supportsIntensity(intensity);
    case PCLColorSource::None:
        return false;
    case PCLColorSource::Auto:
        return supportsIntensity(intensity);
    }
    return supportsIntensity(intensity);
}

bool isFinitePoint(const Coord3DPoint& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

}

pcl::PointCloud<pcl::PointXYZRGB>::Ptr convertPylon3DImageToPCL(
    const Pylon::CPylonDataContainer& container,
    const PCLConversionOptions& options)
{
    const auto range = findComponentByType(container, Pylon::ComponentType_Range);
    if (!range.IsValid() || range.GetPixelType() != Pylon::PixelType_Coord3D_ABC32f) {
        return {};
    }

    const size_t width = range.GetWidth();
    const size_t height = range.GetHeight();
    if (width == 0 || height == 0) {
        return {};
    }

    const auto* srcPoint = static_cast<const Coord3DPoint*>(range.GetData());
    if (!srcPoint) {
        return {};
    }

    const auto intensity = findIntensityComponent(container, options);
    const bool useIntensity = shouldUseIntensity(options, intensity);
    const auto defaultColor = options.defaultColor;
    const size_t pixelCount = width * height;

    auto cloud = pcl::PointCloud<pcl::PointXYZRGB>::Ptr(new pcl::PointCloud<pcl::PointXYZRGB>);
    cloud->width = static_cast<std::uint32_t>(width);
    cloud->height = static_cast<std::uint32_t>(height);
    cloud->is_dense = false;
    cloud->points.resize(pixelCount);

#pragma omp parallel for if(pixelCount >= 4096)
    for (long long i = 0; i < static_cast<long long>(pixelCount); ++i) {
        const auto index = static_cast<size_t>(i);
        const auto& src = srcPoint[index];
        auto& dst = cloud->points[index];

        dst.x = src.x;
        dst.y = src.y;
        dst.z = src.z;

        const auto gray = useIntensity ? readGrayValue(intensity, index, defaultColor) : defaultColor;
        dst.r = gray;
        dst.g = gray;
        dst.b = gray;

        if (options.ignoreInvalidPoints && !isFinitePoint(src)) {
            dst.x = std::numeric_limits<float>::quiet_NaN();
            dst.y = std::numeric_limits<float>::quiet_NaN();
            dst.z = std::numeric_limits<float>::quiet_NaN();
            dst.r = defaultColor;
            dst.g = defaultColor;
            dst.b = defaultColor;
        }
    }

    return cloud;
}
