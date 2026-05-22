#include "BlazeScene3DAdapter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {
#if defined(GRAPHICSENGINE_HAS_OPENMP)
#define GRAPHICSENGINE_OMP_PARALLEL_FOR _Pragma("omp parallel for")
#define GRAPHICSENGINE_OMP_PARALLEL_FOR_REDUCTION_SUM _Pragma("omp parallel for reduction(+:validPointCount)")
#else
#define GRAPHICSENGINE_OMP_PARALLEL_FOR
#define GRAPHICSENGINE_OMP_PARALLEL_FOR_REDUCTION_SUM
#endif

// Forward declare struct coordinates for blaze 3D
struct Coord3DPoint
{
    float x;
    float y;
    float z;
};

struct BlazeScene3DOptions
{
    constexpr BlazeScene3DOptions(const bool flipVertical = false,
                                  const bool flipHorizontal = false,
                                  const bool rotatePointCloudAroundX180 = false,
                                  const GraphicsScene3DContent content = GraphicsScene3DContent::All,
                                  const bool includeRangeAuxiliaryChannels = true,
                                  const bool includePointCloudColors = true) noexcept
        : flipVertical(flipVertical)
        , flipHorizontal(flipHorizontal)
        , rotatePointCloudAroundX180(rotatePointCloudAroundX180)
        , content(content)
        , includeRangeAuxiliaryChannels(includeRangeAuxiliaryChannels)
        , includePointCloudColors(includePointCloudColors)
    {
    }

    bool flipVertical;
    bool flipHorizontal;
    bool rotatePointCloudAroundX180;
    GraphicsScene3DContent content;
    bool includeRangeAuxiliaryChannels;
    bool includePointCloudColors;
};

[[nodiscard]] constexpr BlazeScene3DOptions blazeScene3DOptionsFromRequest(
    const GraphicsScene3DRequest& request,
    const bool flipVertical = false,
    const bool flipHorizontal = false,
    const bool rotatePointCloudAroundX180 = false) noexcept
{
    return BlazeScene3DOptions(flipVertical,
                               flipHorizontal,
                               rotatePointCloudAroundX180,
                               request.content,
                               request.includeRangeAuxiliaryChannels,
                               request.includePointCloudColors);
}

[[nodiscard]] Pylon::CPylonDataComponent findComponentByType(
    const Pylon::CPylonDataContainer& container,
    const Pylon::EComponentType type)
{
    for (size_t i = 0; i < container.GetDataComponentCount(); ++i)
    {
        const auto component = container.GetDataComponent(i);
        if (component.IsValid() && component.GetComponentType() == type)
        {
            return component;
        }
    }

    return {};
}

[[nodiscard]] std::size_t bytesPerPixel(const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Confidence8:
        return 1U;
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Confidence16:
        return 2U;
    default:
        return 0U;
    }
}

[[nodiscard]] bool componentStride(const Pylon::CPylonDataComponent& component,
                                   const std::size_t fallbackBytesPerPixel,
                                   std::size_t& stride)
{
    if (component.GetStride(stride))
    {
        return true;
    }

    if (fallbackBytesPerPixel == 0U)
    {
        return false;
    }

    stride = static_cast<std::size_t>(component.GetWidth()) * fallbackBytesPerPixel
           + component.GetPaddingX();
    return true;
}

[[nodiscard]] bool hasSameExtent(const Pylon::CPylonDataComponent& component,
                                 const std::size_t width,
                                 const std::size_t height)
{
    return component.IsValid()
        && static_cast<std::size_t>(component.GetWidth()) == width
        && static_cast<std::size_t>(component.GetHeight()) == height;
}

[[nodiscard]] std::size_t orientedIndex(const std::size_t x,
                                        const std::size_t y,
                                        const std::size_t width,
                                        const std::size_t height,
                                        const BlazeScene3DOptions& options) noexcept
{
    const std::size_t dstX = options.flipHorizontal ? (width - 1U - x) : x;
    const std::size_t dstY = options.flipVertical ? (height - 1U - y) : y;
    return dstY * width + dstX;
}

[[nodiscard]] float readScalar(const std::uint8_t* row,
                               const std::size_t x,
                               const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Confidence8:
        return static_cast<float>(row[x]);
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Confidence16:
        return static_cast<float>(reinterpret_cast<const std::uint16_t*>(row)[x]);
    default:
        return std::numeric_limits<float>::quiet_NaN();
    }
}

[[nodiscard]] std::uint8_t scalarBits(const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Confidence8:
        return 8U;
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Confidence16:
        return 16U;
    default:
        return 0U;
    }
}

[[nodiscard]] std::uint8_t scalarToRgb8(const float value, const Pylon::EPixelType pixelType) noexcept
{
    if (!std::isfinite(value))
    {
        return 0U;
    }

    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Confidence8:
        return static_cast<std::uint8_t>(value);
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Confidence16:
        return static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8U);
    default:
        return 255U;
    }
}

[[nodiscard]] std::uint8_t readScalarRgb8(const std::uint8_t* row,
                                          const std::size_t x,
                                          const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Confidence8:
        return row[x];
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Confidence16:
        return static_cast<std::uint8_t>(reinterpret_cast<const std::uint16_t*>(row)[x] >> 8U);
    default:
        return 255U;
    }
}

void copyScalarComponent(const Pylon::CPylonDataComponent& component,
                         const std::size_t width,
                         const std::size_t height,
                         std::vector<float>& values,
                         const BlazeScene3DOptions& options)
{
    const auto pixelType = component.GetPixelType();
    const std::size_t pixelBytes = bytesPerPixel(pixelType);
    if (pixelBytes == 0U || !hasSameExtent(component, width, height))
    {
        return;
    }

    const auto* src = static_cast<const std::uint8_t*>(component.GetData());
    if (src == nullptr)
    {
        return;
    }

    std::size_t stride = 0;
    if (!componentStride(component, pixelBytes, stride))
    {
        return;
    }

    values.resize(width * height);
    GRAPHICSENGINE_OMP_PARALLEL_FOR
    for (std::ptrdiff_t ySigned = 0; ySigned < static_cast<std::ptrdiff_t>(height); ++ySigned)
    {
        const auto y = static_cast<std::size_t>(ySigned);
        const auto* row = src + y * stride;
        for (std::size_t x = 0; x < width; ++x)
        {
            values[orientedIndex(x, y, width, height, options)] = readScalar(row, x, pixelType);
        }
    }
}

struct ScalarComponentView
{
    const std::uint8_t* data = nullptr;
    std::size_t stride = 0;
    Pylon::EPixelType pixelType = Pylon::PixelType_Mono8;
    bool valid = false;
};

[[nodiscard]] ScalarComponentView scalarComponentView(const Pylon::CPylonDataComponent& component,
                                                      const std::size_t width,
                                                      const std::size_t height)
{
    ScalarComponentView view;
    const auto pixelType = component.GetPixelType();
    const std::size_t pixelBytes = bytesPerPixel(pixelType);
    if (pixelBytes == 0U || !hasSameExtent(component, width, height))
    {
        return view;
    }

    const auto* src = static_cast<const std::uint8_t*>(component.GetData());
    if (src == nullptr)
    {
        return view;
    }

    std::size_t stride = 0;
    if (!componentStride(component, pixelBytes, stride))
    {
        return view;
    }

    view.data = src;
    view.stride = stride;
    view.pixelType = pixelType;
    view.valid = true;
    return view;
}

[[nodiscard]] bool isFinitePoint(const Coord3DPoint& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

[[nodiscard]] InitialView3D blazeInitialView(const double distanceScale) noexcept
{
    InitialView3D view;
    view.lookDirection = {0.0, 0.0, 1.0};
    view.viewUp = {0.0, -1.0, 0.0};
    view.parallelProjection = false;
    view.distanceScale = distanceScale;
    return view;
}

[[nodiscard]] PointCloudData buildPointCloudOnly(const std::uint8_t* src,
                                                 const std::size_t stride,
                                                 const std::size_t width,
                                                 const std::size_t height,
                                                 const ScalarComponentView& pointColorSource,
                                                 const BlazeScene3DOptions& options)
{
    std::vector<std::size_t> rowValidCounts(height, 0U);
    std::size_t validPointCount = 0U;

    GRAPHICSENGINE_OMP_PARALLEL_FOR_REDUCTION_SUM
    for (std::ptrdiff_t ySigned = 0; ySigned < static_cast<std::ptrdiff_t>(height); ++ySigned)
    {
        const auto y = static_cast<std::size_t>(ySigned);
        const auto* row = reinterpret_cast<const Coord3DPoint*>(src + y * stride);
        std::size_t rowCount = 0U;
        for (std::size_t x = 0; x < width; ++x)
        {
            rowCount += isFinitePoint(row[x]) ? 1U : 0U;
        }
        rowValidCounts[y] = rowCount;
        validPointCount += rowCount;
    }

    PointCloudData cloud;
    if (validPointCount == 0U)
    {
        return cloud;
    }

    const bool hasPointColor = pointColorSource.valid && options.includePointCloudColors;
    cloud.xyz.resize(validPointCount * 3U);
    if (hasPointColor)
    {
        cloud.rgb.resize(validPointCount * 3U);
    }

    if (validPointCount == width * height)
    {
        GRAPHICSENGINE_OMP_PARALLEL_FOR
        for (std::ptrdiff_t ySigned = 0; ySigned < static_cast<std::ptrdiff_t>(height); ++ySigned)
        {
            const auto y = static_cast<std::size_t>(ySigned);
            const auto* row = reinterpret_cast<const Coord3DPoint*>(src + y * stride);
            const auto* intensityRow = pointColorSource.valid ? pointColorSource.data + y * pointColorSource.stride : nullptr;
            for (std::size_t x = 0; x < width; ++x)
            {
                const Coord3DPoint& point = row[x];
                const std::size_t tupleOffset = (y * width + x) * 3U;
                cloud.xyz[tupleOffset] = point.x;
                cloud.xyz[tupleOffset + 1U] = options.rotatePointCloudAroundX180 ? -point.y : point.y;
                cloud.xyz[tupleOffset + 2U] = options.rotatePointCloudAroundX180 ? -point.z : point.z;
                if (hasPointColor && intensityRow != nullptr)
                {
                    const std::uint8_t gray = readScalarRgb8(intensityRow, x, pointColorSource.pixelType);
                    cloud.rgb[tupleOffset] = gray;
                    cloud.rgb[tupleOffset + 1U] = gray;
                    cloud.rgb[tupleOffset + 2U] = gray;
                }
            }
        }

        return cloud;
    }

    std::vector<std::size_t> rowOffsets(height);
    std::size_t offset = 0U;
    for (std::size_t y = 0; y < height; ++y)
    {
        rowOffsets[y] = offset;
        offset += rowValidCounts[y];
    }

    GRAPHICSENGINE_OMP_PARALLEL_FOR
    for (std::ptrdiff_t ySigned = 0; ySigned < static_cast<std::ptrdiff_t>(height); ++ySigned)
    {
        const auto y = static_cast<std::size_t>(ySigned);
        const auto* row = reinterpret_cast<const Coord3DPoint*>(src + y * stride);
        const auto* intensityRow = pointColorSource.valid ? pointColorSource.data + y * pointColorSource.stride : nullptr;
        std::size_t out = rowOffsets[y];
        for (std::size_t x = 0; x < width; ++x)
        {
            const Coord3DPoint& point = row[x];
            if (!isFinitePoint(point))
            {
                continue;
            }

            const std::size_t tupleOffset = out * 3U;
            cloud.xyz[tupleOffset] = point.x;
            cloud.xyz[tupleOffset + 1U] = options.rotatePointCloudAroundX180 ? -point.y : point.y;
            cloud.xyz[tupleOffset + 2U] = options.rotatePointCloudAroundX180 ? -point.z : point.z;
            if (hasPointColor && intensityRow != nullptr)
            {
                const std::uint8_t gray = readScalarRgb8(intensityRow, x, pointColorSource.pixelType);
                cloud.rgb[tupleOffset] = gray;
                cloud.rgb[tupleOffset + 1U] = gray;
                cloud.rgb[tupleOffset + 2U] = gray;
            }
            ++out;
        }
    }

    return cloud;
}

std::optional<GraphicsScene3D> buildScene3D(
    const Pylon::CPylonDataContainer& container,
    const BlazeScene3DOptions& options)
{
    const bool wantsRangeFrame = hasScene3DContent(options.content, GraphicsScene3DContent::RangeFrame);
    const bool wantsPointCloud = hasScene3DContent(options.content, GraphicsScene3DContent::PointCloud);
    if (!wantsRangeFrame && !wantsPointCloud)
    {
        return std::nullopt;
    }

    const auto range = findComponentByType(container, Pylon::ComponentType_Range);
    if (!range.IsValid() || range.GetPixelType() != Pylon::PixelType_Coord3D_ABC32f)
    {
        return std::nullopt;
    }

    const std::size_t width = static_cast<std::size_t>(range.GetWidth());
    const std::size_t height = static_cast<std::size_t>(range.GetHeight());
    if (width == 0U || height == 0U)
    {
        return std::nullopt;
    }

    const auto* src = static_cast<const std::uint8_t*>(range.GetData());
    if (src == nullptr)
    {
        return std::nullopt;
    }

    std::size_t stride = 0;
    if (!componentStride(range, sizeof(Coord3DPoint), stride))
    {
        return std::nullopt;
    }

    const std::size_t pixelCount = width * height;
    RangeFrame frame;
    if (wantsRangeFrame)
    {
        frame.width = static_cast<int>(width);
        frame.height = static_cast<int>(height);
        frame.zScaleMm = options.rotatePointCloudAroundX180 ? -1.0 : 1.0;
        frame.sensorType = "Basler blaze";
        frame.zValues.resize(pixelCount);
        frame.xValues.resize(pixelCount);
        frame.yValues.resize(pixelCount);
        frame.validMask.clear();
        frame.intensity.clear();
        frame.confidence.clear();
    }

    PointCloudData cloud;
    if (wantsPointCloud && wantsRangeFrame)
    {
        cloud.xyz.reserve(pixelCount * 3U);
        cloud.xyz.clear();
    }
    cloud.rgb.clear();

    bool rangeAllValid = true;
    const auto intensity = findComponentByType(container, Pylon::ComponentType_Intensity);
    const ScalarComponentView pointColorSource = (wantsPointCloud && options.includePointCloudColors)
        ? scalarComponentView(intensity, width, height)
        : ScalarComponentView{};
    if (pointColorSource.valid && wantsRangeFrame)
    {
        cloud.rgb.reserve(pixelCount * 3U);
    }

    if (wantsPointCloud && !wantsRangeFrame)
    {
        cloud = buildPointCloudOnly(src,
                                    stride,
                                    width,
                                    height,
                                    pointColorSource,
                                    options);
    }

    if (wantsRangeFrame)
    {
        for (std::size_t y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const Coord3DPoint*>(src + y * stride);
            const auto* intensityRow = pointColorSource.valid ? pointColorSource.data + y * pointColorSource.stride : nullptr;
            for (std::size_t x = 0; x < width; ++x)
            {
                const std::size_t index = orientedIndex(x, y, width, height, options);
                const Coord3DPoint& point = row[x];
                const bool valid = isFinitePoint(point);
                const float worldY = options.rotatePointCloudAroundX180 ? -point.y : point.y;
                const float worldZ = options.rotatePointCloudAroundX180 ? -point.z : point.z;

                frame.xValues[index] = point.x;
                frame.yValues[index] = worldY;
                frame.zValues[index] = point.z;
                if (!valid)
                {
                    if (rangeAllValid)
                    {
                        frame.validMask.assign(pixelCount, 1U);
                        rangeAllValid = false;
                    }
                    frame.validMask[index] = 0U;
                }

                if (wantsPointCloud)
                {
                    if (!valid)
                    {
                        continue;
                    }

                    cloud.xyz.push_back(point.x);
                    cloud.xyz.push_back(worldY);
                    cloud.xyz.push_back(worldZ);
                    if (intensityRow != nullptr)
                    {
                        const std::uint8_t gray = scalarToRgb8(readScalar(intensityRow, x, pointColorSource.pixelType),
                                                               pointColorSource.pixelType);
                        cloud.rgb.push_back(gray);
                        cloud.rgb.push_back(gray);
                        cloud.rgb.push_back(gray);
                    }
                }
            }
        }
    }

    if (wantsRangeFrame && options.includeRangeAuxiliaryChannels)
    {
        copyScalarComponent(intensity, width, height, frame.intensity, options);
        frame.intensityBits = frame.intensity.empty() ? 0U : scalarBits(intensity.GetPixelType());
    }

    const auto confidence = findComponentByType(container, Pylon::ComponentType_Confidence);
    if (wantsRangeFrame && options.includeRangeAuxiliaryChannels)
    {
        copyScalarComponent(confidence, width, height, frame.confidence, options);
        frame.confidenceBits = frame.confidence.empty() ? 0U : scalarBits(confidence.GetPixelType());
    }

    GraphicsScene3D scene;
    scene.content = GraphicsScene3DContent::None;
    scene.meta.sourceName = "Basler blaze";
    scene.surfaceInitialView = blazeInitialView(1.0);
    scene.pointCloudInitialView = blazeInitialView(1.15);
    if (frame.isValid())
    {
        scene.content = scene.content | GraphicsScene3DContent::RangeFrame;
        scene.rangeFrame = std::move(frame);
    }
    if (cloud.isValid())
    {
        scene.content = scene.content | GraphicsScene3DContent::PointCloud;
        scene.pointCloud = std::move(cloud);
    }

    if (scene.content == GraphicsScene3DContent::None)
    {
        return std::nullopt;
    }

    return scene;
}

}

std::optional<GraphicsScene3D> BlazeScene3DAdapter::convertScene3D(
    const Pylon::CPylonDataContainer& container,
    const GraphicsScene3DRequest& request) const
{
    auto scene = buildScene3D(container, blazeScene3DOptionsFromRequest(request));
    if (scene.has_value())
    {
        scene->meta.retainSurfaceMesh = request.retainSurfaceMesh;
    }
    return scene;
}
