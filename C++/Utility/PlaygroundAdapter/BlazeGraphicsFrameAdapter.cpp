#include "BlazeGraphicsFrameAdapter.h"

#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        try {
            const auto bits = Pylon::BitPerPixel(pixelType);
            return bits == 8U || bits == 16U || bits == 24U || bits == 32U ? bits / 8U : 0U;
        } catch (...) { return 0U; }
    }
}

[[nodiscard]] bool componentStride(const Pylon::CPylonDataComponent& component,
                                   const std::size_t fallbackBytesPerPixel,
                                   std::size_t& stride)
{
    const std::size_t width = static_cast<std::size_t>(component.GetWidth());
    if (component.GetStride(stride))
    {
        return stride > 0U;
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
    return stride > 0U;
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

[[nodiscard]] std::uint16_t boundedScalar(const BlazeScalarSource& source,
                                                std::size_t x, std::size_t y) noexcept
{
    const auto knownBytes = bytesPerPixel(source.pixelType);
    const auto bytes = knownBytes ? knownBytes : 1U;
    const auto max = (std::numeric_limits<std::size_t>::max)();
    if (!source.data || !source.stride || !bytes || y > max / source.stride || x > max / bytes) return 0U;
    const auto row = y * source.stride;
    const auto column = x * bytes;
    if (column >= source.stride || row > max - column) return 0U;
    const auto offset = row + column;
    if (offset >= source.readableBytes) return 0U;
    std::uint16_t value = 0U;
    std::memcpy(&value, source.data + offset,
                (std::min)({bytes, sizeof(value), source.stride - column, source.readableBytes - offset}));
    return value;
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
        for (std::size_t x = 0; x < width; ++x)
        {
            values[orientedIndex(x, y, width, height, options)] =
                static_cast<float>(boundedScalar(component, x, y));
        }
    }
}

[[nodiscard]] std::optional<BlazeScalarSource> scalarComponentSource(
    const Pylon::CPylonDataComponent& component,
    const std::size_t width,
    const std::size_t height)
{
    if (!component.IsValid()) return std::nullopt;
    auto pixelType = component.GetPixelType();
    const std::size_t pixelBytes = bytesPerPixel(pixelType) == 2U ? 2U : 1U;
    const auto intMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());

    BlazeScalarSource source;
    source.data = static_cast<const std::uint8_t*>(component.GetData());
    source.pixelType = pixelType;
    source.readableBytes = source.data ? component.GetDataSize() : 0U;
    if (!component.GetWidth() || !component.GetHeight() || component.GetWidth() > intMax
        || component.GetHeight() > intMax)
    {
        // Unusable declared extents: the held bytes are one raw row; the extent mismatch is counted.
        if (source.readableBytes > intMax) return std::nullopt;
        source.pixelType = Pylon::PixelType_Mono8;
        source.width = static_cast<int>(source.readableBytes);
        source.height = 1;
        source.stride = source.readableBytes;
        return source;
    }
    source.width = static_cast<int>(component.GetWidth());
    source.height = static_cast<int>(component.GetHeight());
    std::size_t stride = 0;
    if (!component.GetStride(stride) || stride == 0U)
    {
        // A missing or zero SDK stride reads packed rows plus any padding.
        if (!componentStride(component, pixelBytes, stride)) stride = component.GetWidth() * pixelBytes;
    }
    source.stride = stride;
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

[[nodiscard]] Coord3DPoint boundedPoint(const BlazeRangeSource& source,
                                         std::size_t x, std::size_t y) noexcept
{
    Coord3DPoint point{};
    const auto max = (std::numeric_limits<std::size_t>::max)();
    if (!source.coordinates || !source.coordinateStride || y > max / source.coordinateStride
        || x > max / sizeof(Coord3DPoint)) return point;
    const auto row = y * source.coordinateStride;
    const auto column = x * sizeof(Coord3DPoint);
    if (column >= source.coordinateStride || row > max - column) return point;
    const auto offset = row + column;
    if (offset >= source.coordinateReadableBytes) return point;
    const auto available = (std::min)({sizeof(point), source.coordinateStride - column,
                                        source.coordinateReadableBytes - offset});
    std::memcpy(&point, source.coordinates + offset, available);
    return point;
}

[[nodiscard]] bool hasInvalidPoint(const BlazeRangeSource& source,
                                   std::size_t width, std::size_t height) noexcept
{
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            if (!isFinitePoint(boundedPoint(source, x, y))) return true;
    return false;
}

std::optional<GraphicsFrame> buildBlazeFrame(
    const BlazeRangeSource& source,
    const BlazeFrameOptions& options,
    GraphicsFrameContextSource& contextSource,
    const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection,
    GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled, PylonFrameAnomalies* anomalies)
{
    const auto record = [&](PylonRangeRejection evidence, std::uint32_t part = (std::numeric_limits<std::uint32_t>::max)()) {
        evidence.affectedPart = part == (std::numeric_limits<std::uint32_t>::max)() ? source.rangePartIndex : part;
        if (anomalies) anomalies->add(evidence);
        recordRangeRejection(rejection, evidence);
    };
    const bool wantsRangeFrame = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::Range);
    const bool wantsPointCloud = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::PointCloud);
    const bool rangePresent = source.coordinates && source.coordinateReadableBytes != 0U && !source.rangeAsImage;
    const bool rawRangeImage = (wantsRangeFrame || wantsPointCloud) && source.rangeAsImage
        && source.rawRange.data && source.rawRange.readableBytes;
    const bool intensityPresent = source.hasIntensity && source.intensity.data && source.intensity.readableBytes;
    const bool confidencePresent = source.hasConfidence && source.confidence.data && source.confidence.readableBytes;
    const auto intensityPartIndex = source.intensity.partIndex == (std::numeric_limits<std::uint32_t>::max)()
        ? 1U : source.intensity.partIndex;
    const auto confidencePartIndex = source.confidence.partIndex == (std::numeric_limits<std::uint32_t>::max)()
        ? 2U : source.confidence.partIndex;
    if (source.hasIntensity && !intensityPresent)
        record(rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::MissingData), intensityPartIndex);
    if (source.hasConfidence && !confidencePresent)
        record(rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::MissingData), confidencePartIndex);
    const bool hasRange = (wantsRangeFrame || wantsPointCloud) && rangePresent;
    const bool wantsImage = hasGraphicsFrameComponent(options.components, GraphicsFrameComponent::Image)
        && intensityPresent;
    const auto sameExtent = [&](const BlazeScalarSource& scalar) {
        return (scalar.width <= 0 || scalar.width == source.width)
            && (scalar.height <= 0 || scalar.height == source.height);
    };
    const bool intensitySame = sameExtent(source.intensity);
    const bool confidenceSame = sameExtent(source.confidence);
    const bool confidenceImage = confidencePresent && (wantsRangeFrame || wantsPointCloud)
        && (!hasRange || !confidenceSame);
    const bool intensityImage = wantsImage || (intensityPresent && (wantsRangeFrame || wantsPointCloud)
        && (!hasRange || !intensitySame));
    if (!hasRange && !intensityImage && !confidenceImage && !rawRangeImage)
    {
        if (wantsRangeFrame || wantsPointCloud)
            record(rangeEvidence(source, frameSequence,
                source.coordinates ? PylonRangeRejection::Reason::MissingRange : PylonRangeRejection::Reason::MissingData));
        return std::nullopt;
    }
    const auto minimumRowBytes = source.width > 0
        && static_cast<std::uint64_t>(source.width)
            <= (std::numeric_limits<std::uint64_t>::max)() / sizeof(Coord3DPoint)
        ? static_cast<std::uint64_t>(source.width) * sizeof(Coord3DPoint) : 0U;
    if (source.width <= 0 || source.height <= 0) {
        record(rangeEvidence(source, frameSequence,
            PylonRangeRejection::Reason::InvalidDimensions, minimumRowBytes));
        return std::nullopt;
    }
    if (!rangePresent && !rawRangeImage && (wantsRangeFrame || wantsPointCloud))
        record(rangeEvidence(source, frameSequence,
            PylonRangeRejection::Reason::MissingRange, minimumRowBytes));
    else if (rawRangeImage)
        record(rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::UnsupportedFormat));
    if (rangePresent && source.coordinateStride < minimumRowBytes)
        record(rangeEvidence(source, frameSequence,
            PylonRangeRejection::Reason::StrideBelowMinimum, minimumRowBytes, source.coordinateStride));
    if (rangePresent && source.coordinateStride != 0U && static_cast<std::size_t>(source.height)
             > source.coordinateReadableBytes / source.coordinateStride)
        record(rangeEvidence(source, frameSequence,
            PylonRangeRejection::Reason::TruncatedData, minimumRowBytes, source.coordinateStride));

    const std::size_t width = static_cast<std::size_t>(source.width);
    const std::size_t height = static_cast<std::size_t>(source.height);
    const auto diagnoseScalar = [&](const BlazeScalarSource& scalar, bool present, std::uint32_t part) {
        if (!present) return;
        const auto bytes = bytesPerPixel(scalar.pixelType);
        const auto scalarWidth = static_cast<std::size_t>(scalar.width > 0 ? scalar.width : source.width);
        const auto scalarHeight = static_cast<std::size_t>(scalar.height > 0 ? scalar.height : source.height);
        const auto scalarEvidence = [&](PylonRangeRejection::Reason reason) {
            auto result = rangeEvidence(source, frameSequence, reason);
            result.width = static_cast<std::uint32_t>(scalarWidth);
            result.height = static_cast<std::uint32_t>(scalarHeight);
            result.pixelFormat = static_cast<std::uint64_t>(scalar.pixelType);
            result.reportedStride = scalar.stride;
            result.effectiveStride = scalar.stride;
            result.readableBytes = scalar.readableBytes;
            return result;
        };
        if (scalar.pixelType != Pylon::PixelType_Mono8 && scalar.pixelType != Pylon::PixelType_Mono16
            && scalar.pixelType != Pylon::PixelType_Confidence8 && scalar.pixelType != Pylon::PixelType_Confidence16)
            record(scalarEvidence(PylonRangeRejection::Reason::UnsupportedFormat), part);
        if (bytes && (!scalar.stride || scalarWidth > (std::numeric_limits<std::size_t>::max)() / bytes
                      || scalar.stride < scalarWidth * bytes))
            record(scalarEvidence(PylonRangeRejection::Reason::StrideBelowMinimum), part);
        if (scalar.stride && scalarHeight > scalar.readableBytes / scalar.stride)
            record(scalarEvidence(PylonRangeRejection::Reason::TruncatedData), part);
    };
    const auto intensityPart = intensityPartIndex;
    const auto confidencePart = confidencePartIndex;
    diagnoseScalar(source.intensity, intensityPresent, intensityPart);
    diagnoseScalar(source.confidence, confidencePresent, confidencePart);
    if (intensityPresent && !intensitySame)
        record(rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::InvalidDimensions), intensityPart);
    if (confidencePresent && !confidenceSame)
        record(rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::InvalidDimensions), confidencePart);
    const bool allValid = !hasRange || !hasInvalidPoint(source, width, height);
    // A point cloud is derived from the range by the consumer, so a
    // PointCloud-only request still carries the range. Its RGB channel keeps
    // the grayscale intensity colours the explicit cloud used to carry.
    const bool includeIntensity = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && intensityPresent && intensitySame;
    const bool includeConfidence = wantsRangeFrame && options.includeRangeAuxiliaryChannels
        && confidencePresent && confidenceSame;
    const bool includePointColors = wantsPointCloud && options.includePointCloudColors
        && intensityPresent && intensitySame;
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
    auto rangePlan = planGraphicsRangeWrite(layout);
    if (hasRange && !rangePlan) {
        if (storageError) *storageError = rangePlan.error();
        return std::nullopt;
    }
    auto planImage = [&](const BlazeScalarSource& scalar, GraphicsImageWriteLayout& layout) {
        GraphicsImageWriterSource image;
        image.data = scalar.data;
        image.width = scalar.width > 0 ? scalar.width : source.width;
        image.height = scalar.height > 0 ? scalar.height : source.height;
        const auto imageHeight = static_cast<std::size_t>(image.height);
        const auto bpp = bytesPerPixel(scalar.pixelType);
        image.pixelFormat = bpp == 2U ? GraphicsImagePixelFormat::Mono16
            : bpp == 3U ? GraphicsImagePixelFormat::RGB24
            : bpp == 4U ? GraphicsImagePixelFormat::Mono32 : GraphicsImagePixelFormat::Mono8;
        image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(image.pixelFormat);
        const auto rowBytes = GraphicsImageValidation::minimumRowBytes(image.pixelFormat, image.width);
        if (!rowBytes || rowBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())
            || imageHeight > (std::numeric_limits<std::size_t>::max)() / rowBytes) return false;
        image.stride = static_cast<int>(rowBytes); image.size = rowBytes * imageHeight;
        auto plan = planGraphicsImageWrite(image);
        if (!plan) return false;
        layout = plan.value();
        return true;
    };
    GraphicsImageWriteLayout intensityLayout, confidenceLayout, rawRangeLayout;
    if ((intensityImage && !planImage(source.intensity, intensityLayout))
        || (confidenceImage && !planImage(source.confidence, confidenceLayout))
        || (rawRangeImage && !planImage(source.rawRange, rawRangeLayout))) {
        if (storageError) *storageError = GraphicsFrameError::Capacity; return std::nullopt;
    }
    std::size_t sampleBytes = 0U;
    if ((intensityImage && !appendAligned(sampleBytes, intensityLayout.bytes, intensityLayout.alignment))
        || (confidenceImage && !appendAligned(sampleBytes, confidenceLayout.bytes, confidenceLayout.alignment))
        || (rawRangeImage && !appendAligned(sampleBytes, rawRangeLayout.bytes, rawRangeLayout.alignment))
        || (hasRange && !appendAligned(sampleBytes, rangePlan.value().totalBytes, rangePlan.value().alignment))) {
        if (storageError) *storageError = GraphicsFrameError::Capacity; return std::nullopt;
    }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    auto context = contextSource.beginFrame(sampleBytes);
    if (!context) {
        if (storageError) *storageError = context.error();
        return std::nullopt;
    }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    GraphicsFrameBuilder builder(context.value());
    auto appendImage = [&](const BlazeScalarSource& scalar, const GraphicsImageWriteLayout& imageLayout,
                           const char* id, const char* label) {
        auto reserved = builder.reserve<std::uint8_t>(imageLayout.bytes, imageLayout.alignment);
        if (!reserved) { if (storageError) *storageError = reserved.error(); return false; }
        auto region = std::move(reserved).value();
        std::memset(region.data(), 0, region.size());
        const auto rowBytes = static_cast<std::size_t>(imageLayout.strideBytes);
        const auto stride = scalar.stride ? scalar.stride : rowBytes;
        for (std::size_t y = 0; y < static_cast<std::size_t>(imageLayout.height); ++y) {
            if (stride && y > (std::numeric_limits<std::size_t>::max)() / stride) break;
            const auto offset = y * stride;
            if (offset >= scalar.readableBytes) break;
            std::memcpy(region.data() + y * rowBytes, scalar.data + offset,
                        (std::min)({rowBytes, stride, scalar.readableBytes - offset}));
        }
        GraphicsImage image;
        image.pixels = builder.seal(std::move(region)); image.width = imageLayout.width; image.height = imageLayout.height;
        image.stride = imageLayout.strideBytes; image.pixelFormat = imageLayout.format;
        image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(imageLayout.format);
        image.sourceFormat = static_cast<int>(scalar.pixelType); image.frameSequence = frameSequence;
        builder.addImage(id, std::move(image), label, "primary");
        if (hasRange && imageLayout.width == source.width && imageLayout.height == source.height) {
            auto& descriptor = builder.content().images.back().descriptor;
            descriptor.registration = GraphicsImageRegistration::RegisteredToRange;
            descriptor.registeredResourceId = "range";
        }
        return true;
    };
    if ((intensityImage && !appendImage(source.intensity, intensityLayout, "image", "Intensity"))
        || (confidenceImage && !appendImage(source.confidence, confidenceLayout, "confidence", "Confidence"))
        || (rawRangeImage && !appendImage(source.rawRange, rawRangeLayout, "range-raw", "Range")))
        return std::nullopt;
    if (!hasRange) {
        auto published = std::move(builder).publish();
        if (!published) { if (storageError) *storageError = published.error(); return std::nullopt; }
        return std::move(published).value();
    }
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
        case GraphicsRangeFrameWriterFailure::None:
            break;
        }
        if (storageError && reason == Reason::StorageRefused) *storageError = GraphicsFrameError::Allocation;
        record(rangeEvidence(source, frameSequence, reason,
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
        for (std::size_t x = 0; x < width; ++x)
        {
            const std::size_t index = orientedIndex(x, y, width, height, options);
            const Coord3DPoint point = boundedPoint(source, x, y);
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
                const auto scalar = boundedScalar(*pointColorSource, x, y);
                const std::uint8_t gray = static_cast<std::uint8_t>(
                    scalarBits(pointColorSource->pixelType) == 16U ? scalar >> 8U : scalar);
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

    // The description strings live in a per-thread frame, so a warm producer assigns none per frame.
    thread_local RangeFrame frame;
    struct ReleaseSpans {
        RangeFrame& value;
        ~ReleaseSpans() {
            value.xValues = {}; value.yValues = {}; value.zValues = {}; value.validMask = {};
            value.intensity = {}; value.confidence = {}; value.rgb = {};
        }
    } releaseSpans{frame};
    const RangeFrame taken = writer.takeFrame();
    frame.width = taken.width; frame.height = taken.height;
    frame.xValues = taken.xValues; frame.yValues = taken.yValues; frame.zValues = taken.zValues;
    frame.validMask = taken.validMask; frame.intensity = taken.intensity; frame.confidence = taken.confidence;
    frame.rgb = taken.rgb;
    frame.lengthUnit = GraphicsLengthUnit::Millimeter;
    frame.zScale = options.rotatePointCloudAroundX180 ? -1.0 : 1.0;
    frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    const auto describe = [](MeasurementFieldDescriptor& field, const char* id, const char* name,
                             const char* unit, const MeasurementValueDomain domain, const std::uint8_t bits) {
        field.id.assign(id); field.displayName.assign(name); field.unitSymbol.assign(unit);
        field.domain = domain; field.sampleKind = MeasurementSampleKind::GridSample; field.bitsPerSample = bits;
    };
    frame.intensityBits = includeIntensity ? scalarBits(source.intensity.pixelType) : 0U;
    frame.confidenceBits = includeConfidence ? scalarBits(source.confidence.pixelType) : 0U;
    describe(frame.rangeField, "basler.blaze.z-coordinate", "Z coordinate", "mm",
             MeasurementValueDomain::Calibrated, 32U);
    describe(frame.intensityField, "basler.blaze.intensity", "Intensity", "",
             MeasurementValueDomain::Native, frame.intensityBits);
    describe(frame.confidenceField, "basler.blaze.confidence", "Confidence", "",
             MeasurementValueDomain::Native, frame.confidenceBits);
    frame.sensorType.assign("Basler blaze");

    auto& scene = builder.content();
    scene.metadata.sourceName = "Basler blaze";
    if (frame.isValid())
    {
        scene.addRange("range", frame, "Range", "primary");
        scene.ranges.back().initialView = blazeInitialView(1.0);
    }

    if (!scene.isValid())
    {
        record(rangeEvidence(
            source, frameSequence, PylonRangeRejection::Reason::InvalidLayout, minimumRowBytes));
        return std::nullopt;
    }

    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    auto published = std::move(builder).publish();
    if (!published)
    {
        if (storageError) *storageError = published.error();
        record(rangeEvidence(
            source, frameSequence, PylonRangeRejection::Reason::StorageRefused, minimumRowBytes));
        return std::nullopt;
    }
    return std::move(published).value();
}

} // namespace

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertSource(
    const BlazeRangeSource& source, const GraphicsFrameRequest& request,
    GraphicsFrameContextSource& contextSource, const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection, PylonFrameAnomalies* anomalies) const
{
    return buildBlazeFrame(source, blazeFrameOptionsFromRequest(request), contextSource,
                           frameSequence, rejection, nullptr, nullptr, anomalies);
}

std::optional<GraphicsFrame> BlazeGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    GraphicsFrameContextSource& contextSource,
    const std::uint64_t frameSequence,
    std::optional<PylonRangeRejection>* rejection,
    GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled, PylonFrameAnomalies* anomalies) const
{
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range)
        && !hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::PointCloud))
        rejection = nullptr;
    struct Parts {
        Pylon::CPylonDataComponent range, intensity, confidence;
        std::uint32_t rangeIndex = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t intensityIndex = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t confidenceIndex = (std::numeric_limits<std::uint32_t>::max)();
    } parts;
    for (std::size_t i = 0; i < container.GetDataComponentCount(); ++i) {
        auto part = container.GetDataComponent(i);
        if (!part.IsValid()) continue;
        switch (part.GetComponentType()) {
        case Pylon::ComponentType_Range: if (!parts.range.IsValid()) { parts.range = part; parts.rangeIndex = static_cast<std::uint32_t>(i); } break;
        case Pylon::ComponentType_Intensity: if (!parts.intensity.IsValid()) { parts.intensity = part; parts.intensityIndex = static_cast<std::uint32_t>(i); } break;
        case Pylon::ComponentType_Confidence: if (!parts.confidence.IsValid()) { parts.confidence = part; parts.confidenceIndex = static_cast<std::uint32_t>(i); } break;
        default: break;
        }
    }
    const auto& range = parts.range;
    const auto hasData = [](const Pylon::CPylonDataComponent& part) {
        return part.IsValid() && part.GetData() && part.GetDataSize() != 0U;
    };
    const auto& reference = hasData(range) ? range : hasData(parts.intensity) ? parts.intensity : parts.confidence;
    if (!hasData(reference)) {
        recordRangeRejection(rejection, rangeEvidence({}, frameSequence, PylonRangeRejection::Reason::MissingRange));
        if (storageError) *storageError = GraphicsFrameError::InvalidInput;
        return std::nullopt;
    }
    auto width = static_cast<std::size_t>(reference.GetWidth());
    auto height = static_cast<std::size_t>(reference.GetHeight());
    const auto intMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    BlazeRangeSource source;
    source.rangePartIndex = parts.rangeIndex;
    const bool degenerate = !width || !height || width > intMax || height > intMax;
    if (degenerate) {
        // Unusable declared extents: the reference part is published as one raw row and counted.
        if (reference.GetDataSize() > intMax) {
            if (storageError) *storageError = GraphicsFrameError::Capacity;
            return std::nullopt;
        }
        width = reference.GetDataSize(); height = 1U;
        auto evidence = rangeEvidence({}, frameSequence, PylonRangeRejection::Reason::InvalidDimensions);
        evidence.affectedPart = hasData(range) ? parts.rangeIndex
            : hasData(parts.intensity) ? parts.intensityIndex : parts.confidenceIndex;
        evidence.readableBytes = reference.GetDataSize();
        if (anomalies) anomalies->add(evidence);
        recordRangeRejection(rejection, evidence);
    }
    source.width = static_cast<int>(width); source.height = static_cast<int>(height);
    if (hasData(range)) {
        source.coordinates = static_cast<const std::uint8_t*>(range.GetData());
        source.coordinateReadableBytes = range.GetDataSize();
        source.rangePixelType = range.GetPixelType();
        if (range.GetStride(source.reportedCoordinateStride)) source.coordinateStride = source.reportedCoordinateStride;
        if (source.coordinateStride == 0U && !componentStride(range, sizeof(Coord3DPoint), source.coordinateStride))
            source.coordinateStride = width * sizeof(Coord3DPoint);
        if (degenerate) {
            source.rangeAsImage = true;
            source.rangePixelType = Pylon::PixelType_Mono8;
            source.coordinateStride = width;
            source.rawRange = {source.coordinates, width, Pylon::PixelType_Mono8, source.coordinateReadableBytes};
        } else if (source.rangePixelType != Pylon::PixelType_Coord3D_ABC32f) {
            source.rangeAsImage = true;
            source.rawRange = {source.coordinates, source.coordinateStride, source.rangePixelType, source.coordinateReadableBytes};
        }
    } else {
        recordRangeRejection(rejection, rangeEvidence(source, frameSequence, PylonRangeRejection::Reason::MissingRange));
    }
    if (const auto intensity = scalarComponentSource(parts.intensity, width, height)) {
        source.intensity = *intensity; source.intensity.partIndex = parts.intensityIndex; source.hasIntensity = true;
    }
    if (const auto confidence = scalarComponentSource(parts.confidence, width, height)) {
        source.confidence = *confidence; source.confidence.partIndex = parts.confidenceIndex; source.hasConfidence = true;
    }
    return buildBlazeFrame(source, blazeFrameOptionsFromRequest(request), contextSource,
                           frameSequence, rejection, storageError, cancelled, anomalies);
}
