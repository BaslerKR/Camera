#include "BlazeGraphicsFrameAdapter.h"

#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {
#define GRAPHICSENGINE_OMP_PARALLEL_FOR _Pragma("omp parallel for")
#define GRAPHICSENGINE_OMP_PARALLEL_FOR_REDUCTION_SUM _Pragma("omp parallel for reduction(+:validPointCount)")

// Forward declare struct coordinates for blaze 3D
struct Coord3DPoint
{
    float x;
    float y;
    float z;
};

struct BlazeFrameOptions
{
    constexpr BlazeFrameOptions(const bool flipVertical = false,
                                  const bool flipHorizontal = false,
                                  const bool rotatePointCloudAroundX180 = false,
                                  const GraphicsFrameComponent components = GraphicsFrameComponent::All,
                                  const bool includeRangeAuxiliaryChannels = true,
                                  const bool includePointCloudColors = true) noexcept
        : flipVertical(flipVertical)
        , flipHorizontal(flipHorizontal)
        , rotatePointCloudAroundX180(rotatePointCloudAroundX180)
        , components(components)
        , includeRangeAuxiliaryChannels(includeRangeAuxiliaryChannels)
        , includePointCloudColors(includePointCloudColors)
    {
    }

    bool flipVertical;
    bool flipHorizontal;
    bool rotatePointCloudAroundX180;
    GraphicsFrameComponent components;
    bool includeRangeAuxiliaryChannels;
    bool includePointCloudColors;
};

[[nodiscard]] constexpr BlazeFrameOptions blazeFrameOptionsFromRequest(
    const GraphicsFrameRequest& request,
    const bool flipVertical = false,
    const bool flipHorizontal = false,
    const bool rotatePointCloudAroundX180 = false) noexcept
{
    return BlazeFrameOptions(flipVertical,
                               flipHorizontal,
                               rotatePointCloudAroundX180,
                               request.components,
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
    const std::size_t width = static_cast<std::size_t>(component.GetWidth());
    const std::size_t height = static_cast<std::size_t>(component.GetHeight());
    const std::size_t dataSize = component.GetDataSize();
    if (component.GetStride(stride))
    {
        return stride > 0U && (height == 0U || stride <= dataSize / height);
    }

    if (fallbackBytesPerPixel == 0U)
    {
        return false;
    }

    if (width != 0U && fallbackBytesPerPixel > (std::numeric_limits<std::size_t>::max)() / width)
    {
        return false;
    }
    const std::size_t packedStride = width * fallbackBytesPerPixel;
    const std::size_t padding = component.GetPaddingX();
    if (packedStride > (std::numeric_limits<std::size_t>::max)() - padding)
    {
        return false;
    }
    stride = packedStride + padding;
    return stride > 0U && (height == 0U || stride <= dataSize / height);
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
                                        const BlazeFrameOptions& options) noexcept
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

void writeScalarComponent(const BlazeScalarSource& component,
                          const std::size_t width,
                          const std::size_t height,
                          float* values,
                          const BlazeFrameOptions& options)
{
    if (values == nullptr || component.data == nullptr || component.stride == 0U)
    {
        return;
    }

    GRAPHICSENGINE_OMP_PARALLEL_FOR
    for (std::ptrdiff_t ySigned = 0; ySigned < static_cast<std::ptrdiff_t>(height); ++ySigned)
    {
        const auto y = static_cast<std::size_t>(ySigned);
        const auto* row = component.data + y * component.stride;
        for (std::size_t x = 0; x < width; ++x)
        {
            values[orientedIndex(x, y, width, height, options)] =
                readScalar(row, x, component.pixelType);
        }
    }
}

[[nodiscard]] std::optional<BlazeScalarSource> scalarComponentSource(
    const Pylon::CPylonDataComponent& component,
    const std::size_t width,
    const std::size_t height)
{
    const auto pixelType = component.GetPixelType();
    const std::size_t pixelBytes = bytesPerPixel(pixelType);
    if (pixelBytes == 0U || !hasSameExtent(component, width, height))
    {
        return std::nullopt;
    }

    const auto* src = static_cast<const std::uint8_t*>(component.GetData());
    if (src == nullptr)
    {
        return std::nullopt;
    }

    std::size_t stride = 0;
    if (!componentStride(component, pixelBytes, stride))
    {
        return std::nullopt;
    }

    BlazeScalarSource source;
    source.data = src;
    source.stride = stride;
    source.pixelType = pixelType;
    return source;
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
                                                 const BlazeScalarSource* pointColorSource,
                                                 const BlazeFrameOptions& options)
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

    const bool hasPointColor = pointColorSource != nullptr && options.includePointCloudColors;
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
            const auto* intensityRow = pointColorSource != nullptr ? pointColorSource->data + y * pointColorSource->stride : nullptr;
            for (std::size_t x = 0; x < width; ++x)
            {
                const Coord3DPoint& point = row[x];
                const std::size_t tupleOffset = (y * width + x) * 3U;
                cloud.xyz[tupleOffset] = point.x;
                cloud.xyz[tupleOffset + 1U] = options.rotatePointCloudAroundX180 ? -point.y : point.y;
                cloud.xyz[tupleOffset + 2U] = options.rotatePointCloudAroundX180 ? -point.z : point.z;
                if (hasPointColor && intensityRow != nullptr)
                {
                    const std::uint8_t gray = readScalarRgb8(intensityRow, x, pointColorSource->pixelType);
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
        const auto* intensityRow = pointColorSource != nullptr ? pointColorSource->data + y * pointColorSource->stride : nullptr;
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
                const std::uint8_t gray = readScalarRgb8(intensityRow, x, pointColorSource->pixelType);
                cloud.rgb[tupleOffset] = gray;
                cloud.rgb[tupleOffset + 1U] = gray;
                cloud.rgb[tupleOffset + 2U] = gray;
            }
            ++out;
        }
    }

    return cloud;
}

[[nodiscard]] bool hasInvalidPoint(
    const BlazeRangeSource& source,
    const std::size_t width,
    const std::size_t height) noexcept
{
    for (std::size_t y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const Coord3DPoint*>(
            source.coordinates + y * source.coordinateStride);
        for (std::size_t x = 0; x < width; ++x)
        {
            if (!isFinitePoint(row[x]))
            {
                return true;
            }
        }
    }
    return false;
}

std::optional<GraphicsFrame> buildBlazeFrame(
    const BlazeRangeSource& source,
    const BlazeFrameOptions& options,
    const GraphicsImageStorage& storage)
{
    const bool wantsRangeFrame = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::Range);
    const bool wantsPointCloud = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::PointCloud);
    if (!wantsRangeFrame && !wantsPointCloud)
    {
        return std::nullopt;
    }
    if (source.coordinates == nullptr || source.coordinateStride == 0U
        || source.width <= 0 || source.height <= 0)
    {
        return std::nullopt;
    }

    const std::size_t width = static_cast<std::size_t>(source.width);
    const std::size_t height = static_cast<std::size_t>(source.height);
    const std::size_t pixelCount = width * height;
    const bool allValid = !hasInvalidPoint(source, width, height);
    const bool includeIntensity = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && source.hasIntensity;
    const bool includeConfidence = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && source.hasConfidence;
    const BlazeScalarSource* const pointColorSource =
        wantsPointCloud && options.includePointCloudColors && source.hasIntensity
        ? &source.intensity : nullptr;

    std::optional<GraphicsRangeFrameWriter> writer;
    if (wantsRangeFrame)
    {
        GraphicsRangeFrameLayout layout;
        layout.width = source.width;
        layout.height = source.height;
        layout.channels.x = true;
        layout.channels.y = true;
        layout.channels.validMask = !allValid;
        layout.channels.intensity = includeIntensity;
        layout.channels.confidence = includeConfidence;
        writer.emplace(layout, storage);
        if (!writer->isValid())
        {
            return std::nullopt;
        }
    }

    PointCloudData cloud;
    RangeFrame frame;
    if (writer.has_value())
    {
        float* const xValues = writer->xData();
        float* const yValues = writer->yData();
        float* const zValues = writer->zData();
        std::uint8_t* const mask = writer->validMaskData();
        for (std::size_t y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const Coord3DPoint*>(
                source.coordinates + y * source.coordinateStride);
            const auto* intensityRow = pointColorSource != nullptr
                ? pointColorSource->data + y * pointColorSource->stride : nullptr;
            for (std::size_t x = 0; x < width; ++x)
            {
                const std::size_t index = orientedIndex(x, y, width, height, options);
                const Coord3DPoint& point = row[x];
                const bool valid = isFinitePoint(point);
                const float worldY = options.rotatePointCloudAroundX180 ? -point.y : point.y;
                const float worldZ = options.rotatePointCloudAroundX180 ? -point.z : point.z;

                xValues[index] = point.x;
                yValues[index] = worldY;
                zValues[index] = point.z;
                if (mask != nullptr)
                {
                    mask[index] = valid ? 1U : 0U;
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
                        const std::uint8_t gray = scalarToRgb8(
                            readScalar(intensityRow, x, pointColorSource->pixelType),
                            pointColorSource->pixelType);
                        cloud.rgb.push_back(gray);
                        cloud.rgb.push_back(gray);
                        cloud.rgb.push_back(gray);
                    }
                }
            }
        }

        if (includeIntensity)
        {
            writeScalarComponent(source.intensity, width, height, writer->intensityData(), options);
        }
        if (includeConfidence)
        {
            writeScalarComponent(source.confidence, width, height, writer->confidenceData(), options);
        }

        frame = writer->takeFrame();
        frame.lengthUnit = GraphicsLengthUnit::Millimeter;
        frame.zScale = options.rotatePointCloudAroundX180 ? -1.0 : 1.0;
        frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
        frame.rangeField = {
            "basler.blaze.z-coordinate",
            "Z coordinate",
            "mm",
            MeasurementValueDomain::Calibrated,
            MeasurementSampleKind::GridSample,
            32U};
        frame.intensityField = {
            "basler.blaze.intensity",
            "Intensity",
            "",
            MeasurementValueDomain::Native,
            MeasurementSampleKind::GridSample,
            0U};
        frame.confidenceField = {
            "basler.blaze.confidence",
            "Confidence",
            "",
            MeasurementValueDomain::Native,
            MeasurementSampleKind::GridSample,
            0U};
        frame.sensorType = "Basler blaze";
        if (includeIntensity)
        {
            frame.intensityBits = scalarBits(source.intensity.pixelType);
            frame.intensityField.bitsPerSample = frame.intensityBits;
        }
        if (includeConfidence)
        {
            frame.confidenceBits = scalarBits(source.confidence.pixelType);
            frame.confidenceField.bitsPerSample = frame.confidenceBits;
        }
    }
    else if (wantsPointCloud)
    {
        cloud = buildPointCloudOnly(source.coordinates, source.coordinateStride, width, height,
            pointColorSource, options);
    }

    GraphicsFrame scene;
    scene.metadata.sourceName = "Basler blaze";
    if (frame.isValid())
    {
        scene.addRange("range", std::move(frame), "Range", "primary");
        scene.ranges.back().initialView = blazeInitialView(1.0);
    }
    if (cloud.isValid())
    {
        scene.addPointCloud("point-cloud", std::move(cloud), "Point Cloud", "primary",
                            blazeInitialView(1.15));
    }

    if (!scene.isValid())
    {
        return std::nullopt;
    }

    return scene;
}

} // namespace

void BlazeGraphicsFrameAdapter::setFrameStorage(GraphicsImageStorage storage)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _storage = std::move(storage);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertSource(
    const BlazeRangeSource& source,
    const GraphicsFrameRequest& request) const
{
    GraphicsImageStorage storage;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        storage = _storage;
    }
    return buildBlazeFrame(source, blazeFrameOptionsFromRequest(request), storage);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request) const
{
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

    BlazeRangeSource source;
    source.coordinates = src;
    source.coordinateStride = stride;
    source.width = static_cast<int>(width);
    source.height = static_cast<int>(height);
    if (const auto intensity = scalarComponentSource(
            findComponentByType(container, Pylon::ComponentType_Intensity), width, height))
    {
        source.intensity = *intensity;
        source.hasIntensity = true;
    }
    if (const auto confidence = scalarComponentSource(
            findComponentByType(container, Pylon::ComponentType_Confidence), width, height))
    {
        source.confidence = *confidence;
        source.hasConfidence = true;
    }

    auto scene = convertSource(source, request);
    if (scene.has_value())
    {
        scene->metadata.sourceName = "Basler blaze";
    }
    return scene;
}
