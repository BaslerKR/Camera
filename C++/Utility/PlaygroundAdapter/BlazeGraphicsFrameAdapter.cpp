#include "BlazeGraphicsFrameAdapter.h"

#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
#define GRAPHICSENGINE_OMP_PARALLEL_FOR _Pragma("omp parallel for")

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

[[nodiscard]] PylonRangeRejection rangeEvidence(
    const BlazeRangeSource& source,
    const std::uint64_t frameSequence,
    const PylonRangeRejection::Reason reason,
    const std::uint64_t minimumRowBytes = 0U,
    const std::uint64_t effectiveStride = 0U,
    const std::uint64_t requestedStorageBytes = 0U)
{
    PylonRangeRejection evidence;
    evidence.reason = reason;
    evidence.width = source.width > 0 ? static_cast<std::uint32_t>(source.width) : 0U;
    evidence.height = source.height > 0 ? static_cast<std::uint32_t>(source.height) : 0U;
    evidence.frameSequence = frameSequence;
    evidence.pixelFormat = static_cast<std::uint64_t>(source.rangePixelType);
    evidence.reportedStride = static_cast<std::uint64_t>(source.reportedCoordinateStride);
    evidence.effectiveStride = effectiveStride;
    evidence.minimumRowBytes = minimumRowBytes;
    evidence.readableBytes = static_cast<std::uint64_t>(source.coordinateReadableBytes);
    const auto requiredStride = std::max(effectiveStride, minimumRowBytes);
    if (requiredStride != 0U && evidence.height != 0U
        && evidence.height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride)
    {
        evidence.requiredBytes = requiredStride * evidence.height;
    }
    evidence.requestedStorageBytes = requestedStorageBytes;
    return evidence;
}

void recordRangeRejection(std::optional<PylonRangeRejection>* rejection,
                          const PylonRangeRejection& evidence)
{
    if (rejection && !*rejection)
    {
        *rejection = evidence;
    }
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
    GraphicsFrameContext storage,
    const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection)
{
    const bool wantsRangeFrame = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::Range);
    const bool wantsPointCloud = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::PointCloud);
    if (!wantsRangeFrame && !wantsPointCloud)
    {
        // Not a failed rule: the request never asked for range data.
        return std::nullopt;
    }
    const auto minimumRowBytes = source.width > 0
        && static_cast<std::uint64_t>(source.width)
            <= (std::numeric_limits<std::uint64_t>::max)() / sizeof(Coord3DPoint)
        ? static_cast<std::uint64_t>(source.width) * sizeof(Coord3DPoint) : 0U;
    if (source.coordinates == nullptr || source.coordinateStride == 0U
        || source.width <= 0 || source.height <= 0)
    {
        const auto reason = source.coordinates == nullptr
            ? PylonRangeRejection::Reason::MissingData
            : (source.width <= 0 || source.height <= 0
                ? PylonRangeRejection::Reason::InvalidDimensions
                : PylonRangeRejection::Reason::InvalidLayout);
        recordRangeRejection(rejection, rangeEvidence(source, frameSequence, reason, minimumRowBytes));
        return std::nullopt;
    }

    if (source.coordinateStride < minimumRowBytes)
    {
        recordRangeRejection(rejection, rangeEvidence(source, frameSequence,
            PylonRangeRejection::Reason::StrideBelowMinimum, minimumRowBytes, source.coordinateStride));
        return std::nullopt;
    }

    const std::size_t width = static_cast<std::size_t>(source.width);
    const std::size_t height = static_cast<std::size_t>(source.height);
    const bool allValid = !hasInvalidPoint(source, width, height);
    // A point cloud is derived from the range by the consumer, so a
    // PointCloud-only request still carries the range. Its RGB channel keeps
    // the grayscale intensity colours the explicit cloud used to carry.
    const bool includeIntensity = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && source.hasIntensity;
    const bool includeConfidence = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && source.hasConfidence;
    const bool includePointColors = wantsPointCloud && options.includePointCloudColors
        && source.hasIntensity && source.intensity.data != nullptr;
    const BlazeScalarSource* const pointColorSource =
        includePointColors ? &source.intensity : nullptr;

    GraphicsRangeFrameLayout layout;
    layout.width = source.width;
    layout.height = source.height;
    layout.channels.x = true;
    layout.channels.y = true;
    layout.channels.validMask = !allValid;
    layout.channels.intensity = includeIntensity;
    layout.channels.confidence = includeConfidence;
    layout.channels.rgb = includePointColors;
    GraphicsFrameBuilder builder(storage);
    GraphicsRangeFrameWriter writer(builder, layout);
    if (!writer.isValid())
    {
        using Reason = PylonRangeRejection::Reason;
        Reason reason = Reason::InvalidLayout;
        switch (writer.failure())
        {
        case GraphicsRangeFrameWriterFailure::InvalidDimensions:
            reason = Reason::InvalidDimensions; break;
        case GraphicsRangeFrameWriterFailure::SizeLimitExceeded:
            reason = Reason::SizeLimitExceeded; break;
        case GraphicsRangeFrameWriterFailure::StorageRefused:
            reason = Reason::StorageRefused; break;
        case GraphicsRangeFrameWriterFailure::MisalignedStorage:
            reason = Reason::StorageMisaligned; break;
        case GraphicsRangeFrameWriterFailure::None:
            break;
        }
        recordRangeRejection(rejection, rangeEvidence(source, frameSequence, reason,
            minimumRowBytes, source.coordinateStride, writer.blockBytes()));
        return std::nullopt;
    }

    float* const xValues = writer.xData();
    float* const yValues = writer.yData();
    float* const zValues = writer.zData();
    std::uint8_t* const mask = writer.validMaskData();
    std::uint8_t* const rgb = writer.rgbData();
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

            xValues[index] = point.x;
            yValues[index] = worldY;
            zValues[index] = point.z;
            if (mask != nullptr)
            {
                mask[index] = valid ? 1U : 0U;
            }
            if (rgb != nullptr)
            {
                const std::uint8_t gray = readScalarRgb8(
                    intensityRow, x, pointColorSource->pixelType);
                const std::size_t colorOffset = index * 3U;
                rgb[colorOffset] = gray;
                rgb[colorOffset + 1U] = gray;
                rgb[colorOffset + 2U] = gray;
            }
        }
    }

    if (includeIntensity)
    {
        writeScalarComponent(source.intensity, width, height, writer.intensityData(), options);
    }
    if (includeConfidence)
    {
        writeScalarComponent(source.confidence, width, height, writer.confidenceData(), options);
    }

    RangeFrame frame = writer.takeFrame();
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

    auto& scene = builder.content();
    scene.metadata.sourceName = "Basler blaze";
    if (frame.isValid())
    {
        scene.addRange("range", std::move(frame), "Range", "primary");
        scene.ranges.back().initialView = blazeInitialView(1.0);
    }

    if (!scene.isValid())
    {
        recordRangeRejection(rejection, rangeEvidence(
            source, frameSequence, PylonRangeRejection::Reason::InvalidLayout, minimumRowBytes));
        return std::nullopt;
    }

    auto published = std::move(builder).publish();
    if (!published)
    {
        recordRangeRejection(rejection, rangeEvidence(
            source, frameSequence, PylonRangeRejection::Reason::StorageRefused, minimumRowBytes));
        return std::nullopt;
    }
    return std::move(published).value();
}

} // namespace

void BlazeGraphicsFrameAdapter::setFrameStorage(GraphicsFrameContext storage)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _storage = std::move(storage);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertSource(
    const BlazeRangeSource& source,
    const GraphicsFrameRequest& request,
    const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection) const
{
    GraphicsFrameContext storage = GraphicsFrameContext::local();
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        storage = _storage;
    }
    return buildBlazeFrame(source, blazeFrameOptionsFromRequest(request), storage,
                           frameSequence, rejection);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request) const
{
    return convertGraphicsFrame(container, request, 0U, nullptr);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection) const
{
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range)
        && !hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::PointCloud))
        rejection = nullptr;
    const auto range = findComponentByType(container, Pylon::ComponentType_Range);
    if (!range.IsValid())
    {
        recordRangeRejection(rejection, rangeEvidence({}, frameSequence,
            PylonRangeRejection::Reason::MissingRange));
        return std::nullopt;
    }

    const std::size_t width = static_cast<std::size_t>(range.GetWidth());
    const std::size_t height = static_cast<std::size_t>(range.GetHeight());
    const auto* src = static_cast<const std::uint8_t*>(range.GetData());
    const std::size_t readableBytes = src != nullptr ? range.GetDataSize() : 0U;
    std::size_t reportedStride = 0U;
    if (!range.GetStride(reportedStride)) reportedStride = 0U;
    BlazeRangeSource source;
    source.coordinates = src;
    source.reportedCoordinateStride = reportedStride;
    source.coordinateReadableBytes = readableBytes;
    source.width = static_cast<int>(width);
    source.height = static_cast<int>(height);
    source.rangePixelType = range.GetPixelType();
    const auto componentEvidence = [&](const PylonRangeRejection::Reason reason,
                                       const std::uint64_t minimum = 0U) {
        recordRangeRejection(rejection, rangeEvidence(source, frameSequence, reason, minimum));
    };
    if (source.rangePixelType != Pylon::PixelType_Coord3D_ABC32f)
    {
        componentEvidence(PylonRangeRejection::Reason::UnsupportedFormat);
        return std::nullopt;
    }

    if (width == 0U || height == 0U)
    {
        componentEvidence(PylonRangeRejection::Reason::InvalidDimensions);
        return std::nullopt;
    }

    if (src == nullptr)
    {
        componentEvidence(PylonRangeRejection::Reason::MissingData);
        return std::nullopt;
    }

    std::size_t stride = 0;
    if (!componentStride(range, sizeof(Coord3DPoint), stride))
    {
        componentEvidence(PylonRangeRejection::Reason::InvalidLayout,
                          width <= (std::numeric_limits<std::size_t>::max)() / sizeof(Coord3DPoint)
                              ? width * sizeof(Coord3DPoint) : 0U);
        return std::nullopt;
    }

    source.coordinateStride = stride;
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

    return convertSource(source, request, frameSequence, rejection);
}
