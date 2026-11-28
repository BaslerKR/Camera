#include "PylonGraphicsFrameAdapter.h"

#include "engine/detail/GraphicsWriteLayout.h"

#include <pylon/ImageFormatConverter.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

struct Coord3DPoint
{
    float x;
    float y;
    float z;
};

using StereoParts = CameraStereoParts;

[[nodiscard]] CameraStereoPartView stereoPartView(const Pylon::CPylonDataComponent& part)
{
    CameraStereoPartView view;
    if (!part.IsValid()) return view;
    view.valid = true;
    view.data = part.GetData(); view.bytes = part.GetDataSize();
    view.width = part.GetWidth(); view.height = part.GetHeight();
    view.pixelType = part.GetPixelType(); view.padding = part.GetPaddingX();
    view.hasStride = part.GetStride(view.stride);
    return view;
}
[[nodiscard]] StereoParts stereoParts(const Pylon::CPylonDataContainer& container)
{
    StereoParts parts;
    for (std::size_t i = 0; i < container.GetDataComponentCount(); ++i) {
        auto part = container.GetDataComponent(i);
        if (!part.IsValid()) continue;
        switch (part.GetComponentType()) {
        case Pylon::ComponentType_Intensity: if (!parts.intensity.IsValid()) parts.intensity = stereoPartView(part); break;
        case Pylon::ComponentType_Range: if (!parts.range.IsValid()) parts.range = stereoPartView(part); break;
        case Pylon::ComponentType_Disparity: if (!parts.disparity.IsValid()) parts.disparity = stereoPartView(part); break;
        default: break;
        }
    }
    return parts;
}

[[nodiscard]] std::size_t bytesPerPixel(const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
        return 1U;
    case Pylon::PixelType_Mono16:
    case Pylon::PixelType_Coord3D_C16:
        return 2U;
    case Pylon::PixelType_RGB8packed:
    case Pylon::PixelType_BGR8packed:
        return 3U;
    case Pylon::PixelType_RGBA8packed:
    case Pylon::PixelType_BGRA8packed:
        return 4U;
    case Pylon::PixelType_Coord3D_ABC32f:
        return sizeof(Coord3DPoint);
    default:
        return 0U;
    }
}

[[nodiscard]] bool componentStride(const CameraStereoPartView& component,
                                   std::size_t& stride)
{
    const std::size_t pixelBytes = bytesPerPixel(component.GetPixelType());
    if (pixelBytes == 0U)
    {
        return false;
    }

    const std::size_t width = static_cast<std::size_t>(component.GetWidth());
    const std::size_t height = static_cast<std::size_t>(component.GetHeight());
    if (width != 0U && pixelBytes > (std::numeric_limits<std::size_t>::max)() / width)
    {
        return false;
    }
    const std::size_t packedStride = width * pixelBytes;
    const std::size_t dataSize = component.GetDataSize();
    std::size_t reportedStride = 0U;

    if (component.GetStride(reportedStride)) {
        if (reportedStride < packedStride || height == 0U || reportedStride > dataSize / height) return false;
        stride = reportedStride;
        return true;
    }
    const auto padding = component.GetPaddingX();
    if (padding > (std::numeric_limits<std::size_t>::max)() - packedStride) return false;
    const auto effective = packedStride + padding;
    if (height == 0U || effective > dataSize / height) return false;
    stride = effective;
    return true;
}

/** @brief Fixed layout values of a source, copied without vendor strings or payload. */
struct ImageEvidenceContext
{
    std::uint64_t frameSequence = 0U;
    std::uint64_t pixelFormat = 0U;
    std::uint64_t reportedStride = 0U;
    std::uint64_t readableBytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct RangeEvidenceContext
{
    std::uint64_t frameSequence = 0U;
    std::uint64_t pixelFormat = 0U;
    std::uint64_t reportedStride = 0U;
    std::uint64_t readableBytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] std::uint32_t boundedExtent(const std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, (std::numeric_limits<std::uint32_t>::max)()));
}

[[nodiscard]] std::uint64_t imageRequiredBytes(
    const std::uint64_t effectiveStride,
    const std::uint64_t minimumRowBytes,
    const std::uint32_t height) noexcept
{
    const std::uint64_t stride = std::max(effectiveStride, minimumRowBytes);
    return stride != 0U && height != 0U
        && height <= (std::numeric_limits<std::uint64_t>::max)() / stride
        ? stride * height : 0U;
}

[[nodiscard]] std::optional<PylonImageRejection::Reason> imageRejectionReason(
    const GraphicsImageFailure failure) noexcept
{
    using Reason = PylonImageRejection::Reason;
    switch (failure)
    {
    case GraphicsImageFailure::MissingBytes: return Reason::MissingBytes;
    case GraphicsImageFailure::InvalidDimensions: return Reason::InvalidDimensions;
    case GraphicsImageFailure::UnsupportedFormat: return Reason::UnsupportedFormat;
    case GraphicsImageFailure::InvalidStride: return Reason::InvalidStride;
    case GraphicsImageFailure::StrideBelowMinimum: return Reason::StrideBelowMinimum;
    case GraphicsImageFailure::ExtentOverflow: return Reason::ExtentOverflow;
    case GraphicsImageFailure::TruncatedBytes: return Reason::TruncatedBytes;
    case GraphicsImageFailure::OddWidth: return Reason::OddWidth;
    case GraphicsImageFailure::UnsupportedConversion: return Reason::UnsupportedConversion;
    case GraphicsImageFailure::StorageRefused: return Reason::StorageRefused;
    case GraphicsImageFailure::None:
    case GraphicsImageFailure::ExcessBytes: return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] PylonImageRejection::Conversion writerConversion(
    const GraphicsImageWriterOperation operation) noexcept
{
    using Conversion = PylonImageRejection::Conversion;
    switch (operation)
    {
    case GraphicsImageWriterOperation::SwapByteOrder: return Conversion::SwapByteOrder;
    case GraphicsImageWriterOperation::Copy:
    case GraphicsImageWriterOperation::ShiftMono16:
    case GraphicsImageWriterOperation::NarrowMono16To8: return Conversion::Copy;
    }
    return Conversion::Copy;
}

/** Records the first observed 2D failure; later fallback failures never replace it. */
void recordImageRejection(
    std::optional<PylonImageRejection>* rejection,
    const ImageEvidenceContext& context,
    const PylonImageRejection::Reason reason,
    const PylonImageRejection::Conversion conversion = PylonImageRejection::Conversion::None,
    const std::uint64_t effectiveStride = 0U,
    const std::uint64_t minimumRowBytes = 0U,
    const std::uint64_t requiredBytes = 0U)
{
    if (!rejection || *rejection) return;
    PylonImageRejection evidence;
    evidence.reason = reason;
    evidence.conversion = conversion;
    evidence.width = context.width;
    evidence.height = context.height;
    evidence.frameSequence = context.frameSequence;
    evidence.pixelFormat = context.pixelFormat;
    evidence.reportedStride = context.reportedStride;
    evidence.readableBytes = context.readableBytes;
    evidence.effectiveStride = effectiveStride;
    evidence.minimumRowBytes = minimumRowBytes;
    evidence.requiredBytes = requiredBytes;
    *rejection = std::move(evidence);
}

/** Records the first observed range failure; retained flags are set by the stream owner. */
void recordRangeRejection(
    std::optional<PylonRangeRejection>* rejection,
    const RangeEvidenceContext& context,
    const PylonRangeRejection::Reason reason,
    const std::uint64_t effectiveStride = 0U,
    const std::uint64_t minimumRowBytes = 0U)
{
    if (!rejection || *rejection) return;
    PylonRangeRejection evidence;
    evidence.reason = reason;
    evidence.width = context.width;
    evidence.height = context.height;
    evidence.frameSequence = context.frameSequence;
    evidence.pixelFormat = context.pixelFormat;
    evidence.reportedStride = context.reportedStride;
    evidence.readableBytes = context.readableBytes;
    evidence.effectiveStride = effectiveStride;
    evidence.minimumRowBytes = minimumRowBytes;
    evidence.requiredBytes = imageRequiredBytes(effectiveStride, minimumRowBytes, context.height);
    *rejection = std::move(evidence);
}

[[nodiscard]] RangeEvidenceContext componentRangeContext(
    const CameraStereoPartView& component,
    const std::uint64_t frameSequence)
{
    RangeEvidenceContext context;
    context.frameSequence = frameSequence;
    if (!component.IsValid()) return context;
    context.pixelFormat = static_cast<std::uint64_t>(component.GetPixelType());
    context.width = boundedExtent(component.GetWidth());
    context.height = boundedExtent(component.GetHeight());
    context.readableBytes = component.GetDataSize();
    std::size_t stride = 0U;
    if (component.GetStride(stride)) context.reportedStride = stride;
    return context;
}

struct PreparedPylonImage
{
    Pylon::CPylonImage convertedStorage;
    GraphicsImageWriterSource writerSource;
    GraphicsImageWriteLayout writeLayout;
    ImageEvidenceContext evidenceContext;
};

void recordWriterFailure(
    std::optional<PylonImageRejection>* rejection,
    const ImageEvidenceContext& evidenceContext,
    const GraphicsImageWriterSource& writerSource,
    const GraphicsImageFailure failure)
{
    if (!rejection) return;
    if (const auto reason = imageRejectionReason(failure))
    {
        const auto minimum = static_cast<std::uint64_t>(
            GraphicsImageValidation::minimumRowBytes(writerSource.pixelFormat, writerSource.width));
        const auto effective = writerSource.stride > 0
            ? static_cast<std::uint64_t>(writerSource.stride) : minimum;
        recordImageRejection(rejection, evidenceContext, *reason,
            writerConversion(writerSource.operation), effective, minimum,
            imageRequiredBytes(effective, minimum, evidenceContext.height));
    }
}

[[nodiscard]] bool planPreparedImage(
    PreparedPylonImage& prepared,
    std::optional<PylonImageRejection>* rejection)
{
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    const auto plan = planGraphicsImageWrite(prepared.writerSource, &failure);
    if (!plan)
    {
        recordWriterFailure(rejection, prepared.evidenceContext, prepared.writerSource, failure);
        return false;
    }
    prepared.writeLayout = plan.value();
    return true;
}

[[nodiscard]] GraphicsImage writeImage(
    const int width,
    const int height,
    const int stride,
    const int bitsPerPixel,
    const GraphicsImagePixelFormat pixelFormat,
    const int sourceFormat,
    const void* source,
    const std::size_t size,
    const std::uint8_t declaredMonoSourceBits,
    const std::uint64_t frameSequence,
    GraphicsFrameBuilder& builder,
    const GraphicsImageWriterOperation operation,
    const ImageEvidenceContext& evidenceContext = {},
    std::optional<PylonImageRejection>* rejection = nullptr)
{
    GraphicsImageWriterSource writerSource;
    writerSource.data = static_cast<const std::uint8_t*>(source);
    writerSource.size = size;
    writerSource.width = width;
    writerSource.height = height;
    writerSource.stride = stride;
    writerSource.pixelFormat = pixelFormat;
    writerSource.bitsPerPixel = bitsPerPixel;
    writerSource.sourceFormat = sourceFormat;
    writerSource.declaredMonoSourceBits = declaredMonoSourceBits;
    writerSource.frameSequence = frameSequence;
    writerSource.operation = operation;

    // The one copy out of the SDK buffer. An empty storage result drops the frame.
    // The failure code is read directly from the write that actually failed.
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    GraphicsImage image = writeGraphicsImageWithFailure(
        builder, writerSource, rejection ? &failure : nullptr);
    if (!image.isValid())
    {
        recordWriterFailure(rejection, evidenceContext, writerSource, failure);
    }
    return image;
}

[[nodiscard]] GraphicsImagePixelFormat graphicsPixelFormat(
    const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Mono8signed:
        return GraphicsImagePixelFormat::Mono8;
    case Pylon::PixelType_Mono16:
        return GraphicsImagePixelFormat::Mono16;
    case Pylon::PixelType_RGB8packed:
        return GraphicsImagePixelFormat::RGB24;
    case Pylon::PixelType_BGR8packed:
        return GraphicsImagePixelFormat::BGR24;
    case Pylon::PixelType_RGBA8packed:
        return GraphicsImagePixelFormat::RGBA32;
    case Pylon::PixelType_BGRA8packed:
        return GraphicsImagePixelFormat::BGRA32;
    default:
        return GraphicsImagePixelFormat::Unknown;
    }
}

[[nodiscard]] GraphicsImage componentToGraphicsImage(
    const CameraStereoPartView& component,
    GraphicsFrameBuilder& builder,
    const std::uint64_t frameSequence = 0U,
    std::optional<PylonImageRejection>* rejection = nullptr)
{
    ImageEvidenceContext context;
    context.frameSequence = frameSequence;
    if (!component.IsValid())
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::MissingBytes);
        return {};
    }
    context.pixelFormat = static_cast<std::uint64_t>(component.GetPixelType());
    context.width = boundedExtent(component.GetWidth());
    context.height = boundedExtent(component.GetHeight());
    context.readableBytes = component.GetDataSize();
    if (component.GetData() == nullptr)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::MissingBytes);
        return {};
    }

    const GraphicsImagePixelFormat pixelFormat = graphicsPixelFormat(component.GetPixelType());
    const int bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(pixelFormat);
    if (pixelFormat == GraphicsImagePixelFormat::Unknown || bitsPerPixel == 0)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::UnsupportedFormat);
        return {};
    }

    std::size_t reportedStride = 0U;
    component.GetStride(reportedStride);
    context.reportedStride = reportedStride;
    std::size_t stride = 0U;
    if (!componentStride(component, stride)
        || stride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        const auto minimum = component.GetWidth()
            <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
            ? static_cast<std::uint64_t>(GraphicsImageValidation::minimumRowBytes(
                pixelFormat, static_cast<int>(component.GetWidth()))) : 0U;
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidStride,
            PylonImageRejection::Conversion::None, reportedStride, minimum);
        return {};
    }
    if (component.GetWidth() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || component.GetHeight() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidDimensions);
        return {};
    }

    return writeImage(
        static_cast<int>(component.GetWidth()),
        static_cast<int>(component.GetHeight()),
        static_cast<int>(stride),
        bitsPerPixel,
        pixelFormat,
        static_cast<int>(component.GetPixelType()),
        component.GetData(),
        component.GetDataSize(),
        0U,
        frameSequence,
        builder,
        GraphicsImageWriterOperation::Copy,
        context,
        rejection);
}

[[nodiscard]] bool pylonStride(const Pylon::CPylonImage& image, int& stride,
                               std::uint64_t* reportedStride = nullptr)
{
    std::size_t strideBytes = 0U;
    if (!image.GetStride(strideBytes))
    {
        return false;
    }
    if (reportedStride) *reportedStride = strideBytes;
    if (strideBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }
    stride = static_cast<int>(strideBytes);
    return true;
}

[[nodiscard]] bool shouldConvertToMono16(const Pylon::EPixelType pixelType)
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono10:
    case Pylon::PixelType_Mono10packed:
    case Pylon::PixelType_Mono10p:
    case Pylon::PixelType_Mono12:
    case Pylon::PixelType_Mono12packed:
    case Pylon::PixelType_Mono12p:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::uint8_t monoSourceBits(const Pylon::EPixelType pixelType) noexcept
{
    switch (pixelType)
    {
    case Pylon::PixelType_Mono10:
    case Pylon::PixelType_Mono10packed:
    case Pylon::PixelType_Mono10p:
        return 10U;
    case Pylon::PixelType_Mono12:
    case Pylon::PixelType_Mono12packed:
    case Pylon::PixelType_Mono12p:
        return 12U;
    case Pylon::PixelType_Mono16:
        return 16U;
    default:
        return 0U;
    }
}

[[nodiscard]] bool prepareViaPylon(
    const Pylon::CPylonImage& pylonImage,
    const Pylon::EPixelType outputPixelType,
    const GraphicsImagePixelFormat pixelFormat,
    const int bitsPerPixel,
    const std::uint8_t declaredMonoSourceBits,
    const std::uint64_t frameSequence,
    const ImageEvidenceContext& sourceContext,
    PreparedPylonImage& prepared,
    std::optional<PylonImageRejection>* rejection)
{
    Pylon::CImageFormatConverter converter;
    converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;
    converter.OutputPixelFormat = outputPixelType;
    converter.Convert(prepared.convertedStorage, pylonImage);

    prepared.evidenceContext = sourceContext;
    if (rejection) prepared.evidenceContext.readableBytes = prepared.convertedStorage.GetImageSize();
    int stride = 0;
    if (!pylonStride(prepared.convertedStorage, stride))
    {
        recordImageRejection(rejection, prepared.evidenceContext, PylonImageRejection::Reason::InvalidStride);
        return false;
    }
    if (prepared.convertedStorage.GetWidth() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || prepared.convertedStorage.GetHeight() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        recordImageRejection(rejection, prepared.evidenceContext, PylonImageRejection::Reason::InvalidDimensions);
        return false;
    }
    const bool shiftMono16 = outputPixelType == Pylon::PixelType_Mono16
        && declaredMonoSourceBits > 0U
        && declaredMonoSourceBits < 16U;

    prepared.writerSource = {};
    prepared.writerSource.data = static_cast<const std::uint8_t*>(prepared.convertedStorage.GetBuffer());
    prepared.writerSource.size = prepared.convertedStorage.GetImageSize();
    prepared.writerSource.width = static_cast<int>(prepared.convertedStorage.GetWidth());
    prepared.writerSource.height = static_cast<int>(prepared.convertedStorage.GetHeight());
    prepared.writerSource.stride = stride;
    prepared.writerSource.pixelFormat = pixelFormat;
    prepared.writerSource.bitsPerPixel = bitsPerPixel;
    prepared.writerSource.sourceFormat = static_cast<int>(pylonImage.GetPixelType());
    prepared.writerSource.declaredMonoSourceBits = declaredMonoSourceBits;
    prepared.writerSource.frameSequence = frameSequence;
    prepared.writerSource.operation = shiftMono16
        ? GraphicsImageWriterOperation::ShiftMono16
        : GraphicsImageWriterOperation::Copy;
    return planPreparedImage(prepared, rejection);
}

[[nodiscard]] std::optional<PreparedPylonImage> preparePylonImageSource(
    const Pylon::CPylonImage& pylonImage,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* rejection)
{
    ImageEvidenceContext context;
    if (rejection) {
        context.frameSequence = frameSequence;
        context.pixelFormat = static_cast<std::uint64_t>(pylonImage.GetPixelType());
        context.width = boundedExtent(pylonImage.GetWidth());
        context.height = boundedExtent(pylonImage.GetHeight());
        context.readableBytes = pylonImage.GetImageSize();
    }

    if (pylonImage.GetBuffer() == nullptr || pylonImage.GetImageSize() == 0U)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::MissingBytes);
        return std::nullopt;
    }
    if (pylonImage.GetWidth() == 0 || pylonImage.GetHeight() == 0
        || pylonImage.GetWidth() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || pylonImage.GetHeight() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidDimensions);
        return std::nullopt;
    }

    PreparedPylonImage prepared;
    std::optional<PylonImageRejection> firstFailure;
    std::optional<PylonImageRejection>* failureSink = rejection ? &firstFailure : nullptr;
    int stride = 0;
    if (pylonStride(pylonImage, stride, rejection ? &context.reportedStride : nullptr))
    {
        const auto pixelType = pylonImage.GetPixelType();
        const auto pixelFormat = graphicsPixelFormat(pixelType);
        const int bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(pixelFormat);
        if (pixelFormat != GraphicsImagePixelFormat::Unknown && bitsPerPixel != 0)
        {
            prepared.evidenceContext = context;
            prepared.writerSource.data = static_cast<const std::uint8_t*>(pylonImage.GetBuffer());
            prepared.writerSource.size = pylonImage.GetImageSize();
            prepared.writerSource.width = static_cast<int>(pylonImage.GetWidth());
            prepared.writerSource.height = static_cast<int>(pylonImage.GetHeight());
            prepared.writerSource.stride = stride;
            prepared.writerSource.pixelFormat = pixelFormat;
            prepared.writerSource.bitsPerPixel = bitsPerPixel;
            prepared.writerSource.sourceFormat = static_cast<int>(pixelType);
            prepared.writerSource.declaredMonoSourceBits = 0U;
            prepared.writerSource.frameSequence = frameSequence;
            prepared.writerSource.operation = GraphicsImageWriterOperation::Copy;
            if (!planPreparedImage(prepared, rejection))
            {
                return std::nullopt;
            }
            return prepared;
        }
    }
    else if (failureSink && !*failureSink)
    {
        recordImageRejection(failureSink, context, PylonImageRejection::Reason::InvalidStride);
    }

    const auto pixelType = pylonImage.GetPixelType();
    if (shouldConvertToMono16(pixelType))
    {
        if (prepareViaPylon(
                pylonImage,
                Pylon::PixelType_Mono16,
                GraphicsImagePixelFormat::Mono16,
                16,
                monoSourceBits(pixelType),
                frameSequence,
                context,
                prepared,
                failureSink))
        {
            return prepared;
        }
        if (rejection && !*rejection && firstFailure) *rejection = std::move(firstFailure);
        return std::nullopt;
    }

    // Keep the conversion order. The first recorded writer rule wins; all thrown attempts report a failed conversion.
    try {
        if (prepareViaPylon(
                pylonImage,
                Pylon::PixelType_BGR8packed,
                GraphicsImagePixelFormat::BGR24,
                24,
                0U,
                frameSequence,
                context,
                prepared,
                failureSink))
        {
            return prepared;
        }
    } catch (const Pylon::GenericException&) {
    }
    try {
        if (prepareViaPylon(
                pylonImage,
                Pylon::PixelType_Mono16,
                GraphicsImagePixelFormat::Mono16,
                16,
                0U,
                frameSequence,
                context,
                prepared,
                failureSink))
        {
            return prepared;
        }
    } catch (const Pylon::GenericException&) {
    }
    try {
        if (prepareViaPylon(
                pylonImage,
                Pylon::PixelType_Mono8,
                GraphicsImagePixelFormat::Mono8,
                8,
                0U,
                frameSequence,
                context,
                prepared,
                failureSink))
        {
            return prepared;
        }
    } catch (const Pylon::GenericException&) {
    }
    if (rejection && !*rejection)
    {
        if (firstFailure) *rejection = std::move(firstFailure);
        else recordImageRejection(rejection, context, PylonImageRejection::Reason::UnsupportedConversion);
    }
    return std::nullopt;
}

[[nodiscard]] GraphicsImage pylonImageToGraphicsImage(
    const Pylon::CPylonImage& pylonImage,
    const std::uint64_t frameSequence,
    GraphicsFrameBuilder& builder,
    std::optional<PylonImageRejection>* rejection)
{
    auto prepared = preparePylonImageSource(pylonImage, frameSequence, rejection);
    if (!prepared)
    {
        return {};
    }
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    GraphicsImage image = writeGraphicsImageWithFailure(
        builder, prepared->writerSource, rejection ? &failure : nullptr);
    if (!image.isValid())
    {
        recordWriterFailure(rejection, prepared->evidenceContext, prepared->writerSource, failure);
    }
    return image;
}

[[nodiscard]] std::size_t mappedIndex(const int targetX,
                                      const int targetY,
                                      const int targetWidth,
                                      const int targetHeight,
                                      const int sourceWidth,
                                      const int sourceHeight) noexcept
{
    const int sourceX = std::min(sourceWidth - 1, targetX * sourceWidth / targetWidth);
    const int sourceY = std::min(sourceHeight - 1, targetY * sourceHeight / targetHeight);
    return static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(sourceWidth)
         + static_cast<std::size_t>(sourceX);
}

bool copyScalarIntensity(const CameraStereoPartView& component, RangeFrame& frame,
                         GraphicsFrameBuilder& builder)
{
    if (!component.IsValid() || component.GetData() == nullptr
        || (component.GetPixelType() != Pylon::PixelType_Mono8
            && component.GetPixelType() != Pylon::PixelType_Mono16))
    {
        return true;
    }

    const int sourceWidth = static_cast<int>(component.GetWidth());
    const int sourceHeight = static_cast<int>(component.GetHeight());
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return true;
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride))
    {
        return true;
    }

    const auto* data = static_cast<const std::uint8_t*>(component.GetData());
    auto region = builder.reserve<float>(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    if (!region) return false;
    frame.intensityBits = component.GetPixelType() == Pylon::PixelType_Mono8 ? 8U : 16U;
    for (int y = 0; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            const std::size_t sourceIndex = mappedIndex(
                x, y, frame.width, frame.height, sourceWidth, sourceHeight);
            const std::size_t sourceY = sourceIndex / static_cast<std::size_t>(sourceWidth);
            const std::size_t sourceX = sourceIndex % static_cast<std::size_t>(sourceWidth);
            const auto* row = data + sourceY * stride;
            const float value = component.GetPixelType() == Pylon::PixelType_Mono8
                ? static_cast<float>(row[sourceX])
                : static_cast<float>(reinterpret_cast<const std::uint16_t*>(row)[sourceX]);
            region.value().data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                          + static_cast<std::size_t>(x)] = value;
        }
    }
    frame.intensity = builder.seal(std::move(region).value());
    return true;
}

bool copyPointCloudRgb(const CameraStereoPartView& component, RangeFrame& frame,
                       GraphicsFrameBuilder& builder)
{
    if (!component.IsValid() || component.GetData() == nullptr
        || (component.GetPixelType() != Pylon::PixelType_RGB8packed
            && component.GetPixelType() != Pylon::PixelType_RGBA8packed))
    {
        return true;
    }

    const int sourceWidth = static_cast<int>(component.GetWidth());
    const int sourceHeight = static_cast<int>(component.GetHeight());
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return true;
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride))
    {
        return true;
    }

    const std::size_t channels = component.GetPixelType() == Pylon::PixelType_RGB8packed ? 3U : 4U;
    const auto* data = static_cast<const std::uint8_t*>(component.GetData());
    auto region = builder.reserve<std::uint8_t>(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U);
    if (!region) return false;
    for (int y = 0; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            const std::size_t sourceIndex = mappedIndex(
                x, y, frame.width, frame.height, sourceWidth, sourceHeight);
            const std::size_t sourceY = sourceIndex / static_cast<std::size_t>(sourceWidth);
            const std::size_t sourceX = sourceIndex % static_cast<std::size_t>(sourceWidth);
            const auto* source = data + sourceY * stride + sourceX * channels;
            const std::size_t target = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                                      + static_cast<std::size_t>(x)) * 3U;
            region.value().data()[target] = source[0];
            region.value().data()[target + 1U] = source[1];
            region.value().data()[target + 2U] = source[2];
        }
    }
    frame.rgb = builder.seal(std::move(region).value());
    return true;
}

void appendColorImage(const CameraStereoPartView& intensity,
                      const GraphicsFrameRequest& request,
                      const PylonScene3DProfile& profile,
                      GraphicsFrameBuilder& builder,
                      GraphicsFrameContent& scene,
                      const std::uint64_t frameSequence = 0U,
                      std::optional<PylonImageRejection>* rejection = nullptr)
{
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image))
    {
        return;
    }

    GraphicsImage image = componentToGraphicsImage(intensity, builder, frameSequence, rejection);
    if (!image.isValid())
    {
        return;
    }

    scene.addImage("image", std::move(image), "Image", "primary");
    scene.images.back().descriptor.registration = GraphicsImageRegistration::Unregistered;
}

void registerColorImageToRange(
    const PylonScene3DProfile& profile,
    GraphicsFrameContent& scene)
{
    if (!profile.colorRegisteredToRange || scene.images.empty() || scene.ranges.empty())
    {
        return;
    }
    auto& descriptor = scene.images.front().descriptor;
    descriptor.registeredResourceId = scene.ranges.front().descriptor.id;
    descriptor.registration = GraphicsImageRegistration::RegisteredToRange;
}

// Plan the same reservations as appendColorImage and the range writers, before any sample copy.
[[nodiscard]] bool planStereo(const StereoParts& parts, const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile, bool ace, std::uint64_t sequence, std::size_t& bytes)
{
    bytes = 0U;
    const auto& image = parts.intensity;
    const bool wantsImage = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image);
    const bool wantsRange = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range);
    if (wantsImage || (wantsRange && (request.includeRangeAuxiliaryChannels || request.includePointCloudColors))) {
        if (!image.IsValid() || !image.GetData() || !image.GetWidth() || !image.GetHeight()) return false;
        if (graphicsPixelFormat(image.GetPixelType()) == GraphicsImagePixelFormat::Unknown) return false;
        std::size_t imageStride = 0U;
        if (!componentStride(image, imageStride)) return false;
    }
    if (hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image)
        && image.IsValid() && image.GetData()
        && image.GetWidth() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
        && image.GetHeight() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        std::size_t stride = 0U;
        const auto format = graphicsPixelFormat(image.GetPixelType());
        if (format != GraphicsImagePixelFormat::Unknown && componentStride(image, stride)
            && stride <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            GraphicsImageWriterSource source;
            source.data = static_cast<const std::uint8_t*>(image.GetData());
            source.size = image.GetDataSize();
            source.width = static_cast<int>(image.GetWidth());
            source.height = static_cast<int>(image.GetHeight());
            source.stride = static_cast<int>(stride);
            source.pixelFormat = format;
            source.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(format);
            source.frameSequence = sequence;
            auto plan = planGraphicsImageWrite(source);
            if (!plan || !appendAligned(bytes, plan.value().bytes, plan.value().alignment)) return false;
        } else { return false; }
    }
    const auto& range = ace ? parts.disparity : parts.range;
    if (!wantsRange) return true;
    if ((ace && !profile.hasDisparityCalibration()) || !range.IsValid() || !range.GetData()) return false;
    const auto type = range.GetPixelType();
    if ((ace && type != Pylon::PixelType_Coord3D_C16)
        || (!ace && type != Pylon::PixelType_Coord3D_C16 && type != Pylon::PixelType_Coord3D_ABC32f)) return false;
    std::size_t stride = 0U;
    if (!componentStride(range, stride)) return false;
    const auto width = static_cast<std::size_t>(range.GetWidth());
    const auto height = static_cast<std::size_t>(range.GetHeight());
    if (!width || !height || width > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || height > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || height > (std::numeric_limits<std::size_t>::max)() / width) return false;
    const auto count = width * height;
    if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(float)
        || count > (std::numeric_limits<std::size_t>::max)() / 3U) return false;
    for (int i = 0; i < 3; ++i)
        if (!appendAligned(bytes, count * sizeof(float), alignof(float))) return false;
    if (!appendAligned(bytes, count, alignof(std::uint8_t))) return false;
    if (image.IsValid() && image.GetData() && image.GetWidth() && image.GetHeight()
        && componentStride(image, stride)) {
        const auto imageType = image.GetPixelType();
        if (request.includeRangeAuxiliaryChannels
            && (imageType == Pylon::PixelType_Mono8 || imageType == Pylon::PixelType_Mono16)
            && !appendAligned(bytes, count * sizeof(float), alignof(float))) return false;
        if (request.includePointCloudColors
            && (imageType == Pylon::PixelType_RGB8packed || imageType == Pylon::PixelType_RGBA8packed)
            && !appendAligned(bytes, count * 3U, alignof(std::uint8_t))) return false;
    }
    return true;
}

[[nodiscard]] std::optional<GraphicsFrame> publishScene(GraphicsFrameBuilder& builder,
    GraphicsFrameError* storageError, const std::atomic<bool>* cancelled)
{
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    if (!builder.content().isValid()) return std::nullopt;
    auto result = std::move(builder).publish();
    if (!result)
    {
        if (storageError) *storageError = result.error();
        std::fprintf(stderr, "Camera frame publication failed (error %u).\n",
                     static_cast<unsigned>(result.error()));
        return std::nullopt;
    }
    return std::move(result).value();
}

[[nodiscard]] std::optional<GraphicsFrame> buildDirectXyzScene(
    const StereoParts& parts,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    GraphicsFrameContext& context, GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled, const char* sourceName,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection)
{
    GraphicsFrameBuilder builder(context);
    auto& scene = builder.content();
    scene.metadata.sourceName = sourceName;
    InitialView3D view;
    view.lookDirection = {0.0, 0.0, 1.0};
    view.viewUp = {0.0, -1.0, 0.0};
    view.parallelProjection = false;
    view.distanceScale = 1.15;

    const auto& intensity = parts.intensity;
    appendColorImage(intensity, request, profile, builder, scene, frameSequence, imageRejection);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range))
    {
        return publishScene(builder, storageError, cancelled);
    }

    const auto& range = parts.range;
    const auto rangeContext = componentRangeContext(range, frameSequence);
    if (!range.IsValid())
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::MissingRange);
        return publishScene(builder, storageError, cancelled);
    }
    if (range.GetData() == nullptr)
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::MissingData);
        return publishScene(builder, storageError, cancelled);
    }

    const auto pixelType = range.GetPixelType();
    if (pixelType != Pylon::PixelType_Coord3D_ABC32f && pixelType != Pylon::PixelType_Coord3D_C16)
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::UnsupportedFormat);
        return publishScene(builder, storageError, cancelled);
    }

    std::size_t stride = 0U;
    if (!componentStride(range, stride))
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::InvalidLayout);
        return publishScene(builder, storageError, cancelled);
    }

    RangeFrame frame;
    frame.width = static_cast<int>(range.GetWidth());
    frame.height = static_cast<int>(range.GetHeight());
    frame.lengthUnit = GraphicsLengthUnit::Millimeter;
    frame.rangeField = {
        pixelType == Pylon::PixelType_Coord3D_ABC32f
            ? "basler.xyz.z-coordinate"
            : "basler.depth",
        pixelType == Pylon::PixelType_Coord3D_ABC32f
            ? "Z coordinate"
            : "Depth",
        "mm",
        MeasurementValueDomain::Calibrated,
        MeasurementSampleKind::GridSample,
        pixelType == Pylon::PixelType_Coord3D_ABC32f ? std::uint8_t{32} : std::uint8_t{16}};
    frame.intensityField = {
        "basler.intensity",
        "Intensity",
        "",
        MeasurementValueDomain::Native,
        MeasurementSampleKind::GridSample,
        0U};
    frame.sensorType = sourceName;
    const std::size_t count = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    auto xs = builder.reserve<float>(count);
    auto ys = builder.reserve<float>(count);
    auto zs = builder.reserve<float>(count);
    auto mask = builder.reserve<std::uint8_t>(count);
    if (!xs || !ys || !zs || !mask)
    {
        if (storageError) *storageError = GraphicsFrameError::Allocation;
        recordRangeRejection(rangeRejection, rangeContext, PylonRangeRejection::Reason::StorageRefused);
        return std::nullopt;
    }
    frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    const auto* data = static_cast<const std::uint8_t*>(range.GetData());

    if (pixelType == Pylon::PixelType_Coord3D_ABC32f)
    {
        for (int y = 0; y < frame.height; ++y)
        {
            const auto* row = reinterpret_cast<const Coord3DPoint*>(data + static_cast<std::size_t>(y) * stride);
            for (int x = 0; x < frame.width; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                                        + static_cast<std::size_t>(x);
                const Coord3DPoint& point = row[x];
                xs.value().data()[index] = point.x;
                ys.value().data()[index] = point.y;
                zs.value().data()[index] = point.z;
                mask.value().data()[index] = std::isfinite(point.x)
                    && std::isfinite(point.y)
                    && std::isfinite(point.z)
                    && point.z > 0.0F ? 1U : 0U;
            }
        }
    }
    else if (pixelType == Pylon::PixelType_Coord3D_C16)
    {
        for (int y = 0; y < frame.height; ++y)
        {
            const auto* row = reinterpret_cast<const std::uint16_t*>(data + static_cast<std::size_t>(y) * stride);
            for (int x = 0; x < frame.width; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                                        + static_cast<std::size_t>(x);
                const std::uint16_t raw = row[x];
                const double z = static_cast<double>(raw) * profile.coordinateScale + profile.coordinateOffset;

                if (raw == 0U || !std::isfinite(z) || z <= 0.0
                    || !std::isfinite(profile.focalLength) || profile.focalLength <= 0.0)
                {
                    xs.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                    ys.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                    zs.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                    mask.value().data()[index] = 0U;
                    continue;
                }

                xs.value().data()[index] = static_cast<float>((static_cast<double>(x) - profile.principalPointU)
                                                           * z / profile.focalLength);
                ys.value().data()[index] = static_cast<float>((static_cast<double>(y) - profile.principalPointV)
                                                           * z / profile.focalLength);
                zs.value().data()[index] = static_cast<float>(z);
                mask.value().data()[index] = 1U;
            }
        }
    }

    frame.xValues = builder.seal(std::move(xs).value());
    frame.yValues = builder.seal(std::move(ys).value());
    frame.zValues = builder.seal(std::move(zs).value());
    frame.validMask = builder.seal(std::move(mask).value());
    if (request.includeRangeAuxiliaryChannels)
    {
        if (!copyScalarIntensity(intensity, frame, builder))
        {
            if (storageError) *storageError = GraphicsFrameError::Allocation;
            recordRangeRejection(rangeRejection, rangeContext, PylonRangeRejection::Reason::StorageRefused);
            return std::nullopt;
        }
        frame.intensityField.bitsPerSample = frame.intensityBits;
    }
    if (request.includePointCloudColors)
    {
        if (!copyPointCloudRgb(intensity, frame, builder))
        {
            if (storageError) *storageError = GraphicsFrameError::Allocation;
            recordRangeRejection(rangeRejection, rangeContext, PylonRangeRejection::Reason::StorageRefused);
            return std::nullopt;
        }
    }

    if (frame.isValid())
    {
        scene.addRange("range", std::move(frame), "Range", "primary");
        scene.ranges.back().initialView = view;
        registerColorImageToRange(profile, scene);
    }
    else
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::InvalidLayout);
    }
    return publishScene(builder, storageError, cancelled);
}

[[nodiscard]] std::optional<GraphicsFrame> buildStereoAceScene(
    const StereoParts& parts,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    GraphicsFrameContext& context, GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled, const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection)
{
    GraphicsFrameBuilder builder(context);
    auto& scene = builder.content();
    scene.metadata.sourceName = "Basler Stereo ace";

    const auto& intensity = parts.intensity;
    appendColorImage(intensity, request, profile, builder, scene, frameSequence, imageRejection);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range))
    {
        return publishScene(builder, storageError, cancelled);
    }
    if (!profile.hasDisparityCalibration())
    {
        RangeEvidenceContext noRange;
        noRange.frameSequence = frameSequence;
        recordRangeRejection(rangeRejection, noRange,
                             PylonRangeRejection::Reason::MissingCalibration);
        return publishScene(builder, storageError, cancelled);
    }

    const auto& disparity = parts.disparity;
    const auto disparityContext = componentRangeContext(disparity, frameSequence);
    if (!disparity.IsValid())
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::MissingRange);
        return publishScene(builder, storageError, cancelled);
    }
    if (disparity.GetPixelType() != Pylon::PixelType_Coord3D_C16)
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::UnsupportedFormat);
        return publishScene(builder, storageError, cancelled);
    }
    if (disparity.GetData() == nullptr)
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::MissingData);
        return publishScene(builder, storageError, cancelled);
    }

    std::size_t stride = 0U;
    if (!componentStride(disparity, stride))
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::InvalidLayout);
        return publishScene(builder, storageError, cancelled);
    }

    RangeFrame frame;
    frame.width = static_cast<int>(disparity.GetWidth());
    frame.height = static_cast<int>(disparity.GetHeight());
    frame.lengthUnit = GraphicsLengthUnit::Millimeter;
    frame.rangeField = {
        "basler.stereo-ace.depth",
        "Depth",
        "mm",
        MeasurementValueDomain::Calibrated,
        MeasurementSampleKind::GridSample,
        16U};
    frame.intensityField = {
        "basler.stereo-ace.intensity",
        "Intensity",
        "",
        MeasurementValueDomain::Native,
        MeasurementSampleKind::GridSample,
        0U};
    frame.sensorType = "Basler Stereo ace";
    const std::size_t count = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    auto xs = builder.reserve<float>(count);
    auto ys = builder.reserve<float>(count);
    auto zs = builder.reserve<float>(count);
    auto mask = builder.reserve<std::uint8_t>(count);
    if (!xs || !ys || !zs || !mask)
    {
        if (storageError) *storageError = GraphicsFrameError::Allocation;
        recordRangeRejection(rangeRejection, disparityContext, PylonRangeRejection::Reason::StorageRefused);
        return std::nullopt;
    }
    frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    const auto* data = static_cast<const std::uint8_t*>(disparity.GetData());
    for (int y = 0; y < frame.height; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(data + static_cast<std::size_t>(y) * stride);
        for (int x = 0; x < frame.width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                                    + static_cast<std::size_t>(x);
            const std::uint16_t raw = row[x];
            const double calibrated = static_cast<double>(raw) * profile.coordinateScale + profile.coordinateOffset;
            if (raw == 0U || calibrated <= 0.0)
            {
                xs.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                ys.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                zs.value().data()[index] = std::numeric_limits<float>::quiet_NaN();
                mask.value().data()[index] = 0U;
                continue;
            }

            const float z = static_cast<float>(1000.0 * profile.baseline * profile.focalLength / calibrated);
            xs.value().data()[index] = static_cast<float>((static_cast<double>(x) - profile.principalPointU)
                                                       * z / profile.focalLength);
            ys.value().data()[index] = static_cast<float>((static_cast<double>(y) - profile.principalPointV)
                                                       * z / profile.focalLength);
            zs.value().data()[index] = z;
            mask.value().data()[index] = 1U;
        }
    }

    frame.xValues = builder.seal(std::move(xs).value());
    frame.yValues = builder.seal(std::move(ys).value());
    frame.zValues = builder.seal(std::move(zs).value());
    frame.validMask = builder.seal(std::move(mask).value());
    if (request.includeRangeAuxiliaryChannels)
    {
        if (!copyScalarIntensity(intensity, frame, builder))
        {
            if (storageError) *storageError = GraphicsFrameError::Allocation;
            recordRangeRejection(rangeRejection, disparityContext, PylonRangeRejection::Reason::StorageRefused);
            return std::nullopt;
        }
        frame.intensityField.bitsPerSample = frame.intensityBits;
    }
    if (request.includePointCloudColors)
    {
        if (!copyPointCloudRgb(intensity, frame, builder))
        {
            if (storageError) *storageError = GraphicsFrameError::Allocation;
            recordRangeRejection(rangeRejection, disparityContext, PylonRangeRejection::Reason::StorageRefused);
            return std::nullopt;
        }
    }

    if (frame.isValid())
    {
        scene.addRange("range", std::move(frame), "Range", "primary");
        registerColorImageToRange(profile, scene);
    }
    else
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::InvalidLayout);
    }
    return publishScene(builder, storageError, cancelled);
}

} // namespace

GraphicsImage PylonGraphicsFrameAdapter::convertGraphicsImage(
    const Pylon::CPylonImage& image,
    GraphicsFrameBuilder& builder,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* rejection) const
{
    return pylonImageToGraphicsImage(image, frameSequence, builder, rejection);
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertGraphicsImageFrame(
    const Pylon::CPylonImage& image,
    GraphicsFrameContextSource& contextSource,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* rejection,
    GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled) const
{
    const auto directFormat = graphicsPixelFormat(image.GetPixelType());
    if (directFormat != GraphicsImagePixelFormat::Unknown
        || (!shouldConvertToMono16(image.GetPixelType())
            && image.GetPixelType() != Pylon::PixelType_RGB8planar)) {
        CameraStereoPartView part;
        part.data = image.GetBuffer(); part.bytes = image.GetImageSize();
        part.width = image.GetWidth(); part.height = image.GetHeight();
        part.pixelType = image.GetPixelType(); part.valid = true;
        part.hasStride = image.GetStride(part.stride);
        return convertImageViewFrame(part, contextSource, frameSequence, rejection, storageError, cancelled);
    }
    PreparedPylonImage preparedValue;
    auto* prepared = &preparedValue;
    auto& source = prepared->writerSource;
    auto& evidence = prepared->evidenceContext;
    evidence.frameSequence = frameSequence;
    evidence.pixelFormat = static_cast<std::uint64_t>(image.GetPixelType());
    evidence.width = boundedExtent(image.GetWidth());
    evidence.height = boundedExtent(image.GetHeight());
    evidence.readableBytes = image.GetImageSize();
    if (!image.GetBuffer() || image.GetImageSize() == 0U || image.GetWidth() == 0U
        || image.GetHeight() == 0U
        || image.GetWidth() >= static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || image.GetHeight() >= static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidDimensions);
        return std::nullopt;
    }
    source.data = static_cast<const std::uint8_t*>(image.GetBuffer());
    source.size = image.GetImageSize();
    source.width = static_cast<int>(image.GetWidth());
    source.height = static_cast<int>(image.GetHeight());
    source.sourceFormat = static_cast<int>(image.GetPixelType());
    source.frameSequence = frameSequence;
    source.pixelFormat = graphicsPixelFormat(image.GetPixelType());
    source.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(source.pixelFormat);
    int stride = 0;
    const bool hasStride = pylonStride(image, stride, &evidence.reportedStride);
    bool direct = hasStride && source.pixelFormat != GraphicsImagePixelFormat::Unknown && source.bitsPerPixel != 0;
    bool repaired = false;
    std::size_t inputStride = hasStride && stride > 0 ? static_cast<std::size_t>(stride) : 0U;
    const auto inputBytes = image.GetImageSize();
    Pylon::CImageFormatConverter converter;
    if (!direct && !Pylon::CImageFormatConverter::IsSupportedInputFormat(image.GetPixelType())) {
        source.pixelFormat = GraphicsImagePixelFormat::Mono8;
        source.bitsPerPixel = 8;
        direct = true;
        repaired = true;
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::UnsupportedFormat);
    }
    if (direct)
    {
        source.stride = stride;
        if (!planPreparedImage(*prepared, nullptr)) {
            const auto rowBytes = GraphicsImageValidation::minimumRowBytes(source.pixelFormat, source.width);
            if (!rowBytes || rowBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())
                || static_cast<std::size_t>(source.height) > (std::numeric_limits<std::size_t>::max)() / rowBytes) {
                if (storageError) *storageError = GraphicsFrameError::Capacity;
                return std::nullopt;
            }
            if (!inputStride) inputStride = rowBytes;
            source.stride = static_cast<int>(rowBytes);
            source.size = rowBytes * static_cast<std::size_t>(source.height);
            repaired = true;
            recordImageRejection(rejection, evidence,
                inputStride != 0U && static_cast<std::size_t>(source.height) > inputBytes / inputStride
                    ? PylonImageRejection::Reason::TruncatedBytes : PylonImageRejection::Reason::InvalidStride);
        }
    }
    else
    {
        const bool mono = shouldConvertToMono16(image.GetPixelType());
        converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;
        converter.OutputPixelFormat = mono ? Pylon::PixelType_Mono16 : Pylon::PixelType_BGR8packed;
        source.pixelFormat = mono ? GraphicsImagePixelFormat::Mono16 : GraphicsImagePixelFormat::BGR24;
        source.bitsPerPixel = mono ? 16 : 24;
        source.declaredMonoSourceBits = mono ? monoSourceBits(image.GetPixelType()) : 0U;
        source.size = converter.GetBufferSizeForConversion(image);
        if (source.size % image.GetHeight() != 0U
            || source.size / image.GetHeight() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidStride);
            return std::nullopt;
        }
        source.stride = static_cast<int>(source.size / image.GetHeight());
    }
    if (!planPreparedImage(*prepared, rejection)) return std::nullopt;

    std::size_t sampleBytes = 0U;
    if (!appendAligned(sampleBytes, prepared->writeLayout.bytes, prepared->writeLayout.alignment))
    {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        recordWriterFailure(rejection, prepared->evidenceContext, prepared->writerSource,
            GraphicsImageFailure::ExtentOverflow);
        return std::nullopt;
    }

    if (cancelled && cancelled->load(std::memory_order_acquire))
    {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }

    auto contextResult = contextSource.beginFrame(sampleBytes);
    if (!contextResult)
    {
        const GraphicsFrameError err = contextResult.error();
        if (storageError) *storageError = err;
        recordWriterFailure(rejection, prepared->evidenceContext, prepared->writerSource,
            err == GraphicsFrameError::Capacity
                ? GraphicsImageFailure::ExtentOverflow
                : GraphicsImageFailure::StorageRefused);
        return std::nullopt;
    }

    if (cancelled && cancelled->load(std::memory_order_acquire))
    {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }

    GraphicsFrameBuilder builder(contextResult.value());
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    GraphicsImage graphicsImage;
    if (direct && !repaired)
    {
        graphicsImage = writeGraphicsImageWithFailure(builder, source, &failure);
    }
    else if (direct)
    {
        auto reserved = builder.reserve<std::uint8_t>(prepared->writeLayout.bytes, prepared->writeLayout.alignment);
        if (!reserved) { if (storageError) *storageError = reserved.error(); return std::nullopt; }
        auto region = std::move(reserved).value();
        std::memset(region.data(), 0, region.size());
        const auto rowBytes = static_cast<std::size_t>(source.stride);
        for (std::size_t row = 0; row < static_cast<std::size_t>(source.height); ++row) {
            if (inputStride && row > (std::numeric_limits<std::size_t>::max)() / inputStride) break;
            const auto offset = row * inputStride;
            if (offset >= inputBytes) break;
            const auto available = (std::min)({rowBytes, inputStride, inputBytes - offset});
            std::memcpy(region.data() + row * rowBytes, source.data + offset, available);
        }
        graphicsImage.pixels = builder.seal(std::move(region));
        graphicsImage.width = source.width; graphicsImage.height = source.height;
        graphicsImage.stride = source.stride; graphicsImage.bitsPerPixel = source.bitsPerPixel;
        graphicsImage.sourceFormat = source.sourceFormat; graphicsImage.pixelFormat = source.pixelFormat;
        graphicsImage.bitAlignment = source.bitAlignment; graphicsImage.frameSequence = frameSequence;
    }
    else
    {
        auto reserved = builder.reserve<std::uint8_t>(prepared->writeLayout.bytes, prepared->writeLayout.alignment);
        if (!reserved)
        {
            if (storageError) *storageError = reserved.error();
            recordWriterFailure(rejection, evidence, source, GraphicsImageFailure::StorageRefused);
            return std::nullopt;
        }
        auto region = std::move(reserved).value();
        converter.Convert(region.data(), region.size(), image);
        if (source.declaredMonoSourceBits > 0U && source.declaredMonoSourceBits < 16U)
        {
            const int shift = 16 - source.declaredMonoSourceBits;
            for (std::size_t offset = 0; offset + 1U < region.size(); offset += 2U)
            {
                std::uint16_t value;
                std::memcpy(&value, region.data() + offset, sizeof(value));
                value = static_cast<std::uint16_t>(value >> shift);
                std::memcpy(region.data() + offset, &value, sizeof(value));
            }
        }
        graphicsImage.pixels = builder.seal(std::move(region));
        graphicsImage.width = source.width;
        graphicsImage.height = source.height;
        graphicsImage.stride = source.stride;
        graphicsImage.bitsPerPixel = source.bitsPerPixel;
        graphicsImage.declaredMonoSourceBits = source.declaredMonoSourceBits;
        graphicsImage.sourceFormat = source.sourceFormat;
        graphicsImage.pixelFormat = source.pixelFormat;
        graphicsImage.bitAlignment = source.bitAlignment;
        graphicsImage.frameSequence = frameSequence;
    }
    if (!graphicsImage.isValid())
    {
        if (storageError && failure == GraphicsImageFailure::StorageRefused)
        {
            *storageError = GraphicsFrameError::Allocation;
        }
        recordWriterFailure(rejection, prepared->evidenceContext, prepared->writerSource, failure);
        return std::nullopt;
    }

    if (cancelled && cancelled->load(std::memory_order_acquire))
    {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }

    builder.addImage("image", std::move(graphicsImage), "Image");
    auto published = std::move(builder).publish();
    if (!published)
    {
        if (storageError) *storageError = published.error();
        recordWriterFailure(rejection, prepared->evidenceContext, prepared->writerSource,
            GraphicsImageFailure::StorageRefused);
        return std::nullopt;
    }
    return std::move(published).value();
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection,
    GraphicsFrameContextSource* contextSource, GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled, PylonFrameAnomalies* anomalies) const
{
    std::optional<GraphicsFrame> result;
    switch (profile.family)
    {
    case PylonScene3DProfile::DeviceFamily::Blaze:
        if (!contextSource) { if (storageError) *storageError = GraphicsFrameError::Disconnected; break; }
        result = _blazeAdapter.convertGraphicsFrame(
            container, request, *contextSource, frameSequence, rangeRejection, storageError, cancelled, anomalies);
        break;
    case PylonScene3DProfile::DeviceFamily::StereoMini:
    case PylonScene3DProfile::DeviceFamily::StereoAce:
        if (!contextSource) { if (storageError) *storageError = GraphicsFrameError::Disconnected; break; }
        result = convertStereoParts(stereoParts(container), request, profile, *contextSource,
                                    frameSequence, imageRejection, rangeRejection, storageError, cancelled);
        break;
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }

    if (rangeRejection && *rangeRejection)
    {
        auto& evidence = **rangeRejection;
        evidence.frameRetained = result.has_value();
        evidence.imageRetained = result && result->hasImage();
        evidence.rangeRetained = result && result->hasRangeFrame();
        evidence.partCount = boundedExtent(container.GetDataComponentCount());
        if (evidence.reason != PylonRangeRejection::Reason::MissingRange
            && evidence.reason != PylonRangeRejection::Reason::MissingCalibration) {
            const auto type = profile.family == PylonScene3DProfile::DeviceFamily::StereoAce
                ? Pylon::ComponentType_Disparity : Pylon::ComponentType_Range;
            for (std::uint32_t index = 0; index < evidence.partCount; ++index) {
                const auto component = container.GetDataComponent(index);
                if (component.IsValid() && component.GetComponentType() == type) {
                    evidence.affectedPart = index;
                    break;
                }
            }
        }
    }
    return result;
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertStereoParts(
    const CameraStereoParts& parts, const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile, GraphicsFrameContextSource& contextSource,
    const std::uint64_t frameSequence, std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection, GraphicsFrameError* storageError,
    const std::atomic<bool>* cancelled) const
{
    const bool ace = profile.family == PylonScene3DProfile::DeviceFamily::StereoAce;
    if (!ace && profile.family != PylonScene3DProfile::DeviceFamily::StereoMini) {
        if (storageError) *storageError = GraphicsFrameError::InvalidInput;
        return std::nullopt;
    }
    auto selectedParts = parts;
    auto selectedRequest = request;
    const bool wantsImage = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image);
    const bool wantsRange = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range);
    GraphicsFrameRequest imageRequest;
    imageRequest.components = GraphicsFrameComponent::Image;
    imageRequest.includeRangeAuxiliaryChannels = false;
    imageRequest.includePointCloudColors = false;
    GraphicsFrameRequest rangeRequest;
    rangeRequest.components = GraphicsFrameComponent::Range;
    rangeRequest.includeRangeAuxiliaryChannels = false;
    rangeRequest.includePointCloudColors = false;
    std::size_t ignored = 0U;
    const bool imageValid = planStereo(parts, imageRequest, profile, ace, frameSequence, ignored);
    ignored = 0U;
    const bool rangeValid = planStereo(parts, rangeRequest, profile, ace, frameSequence, ignored);
    if (!imageValid) {
        if (wantsImage || (wantsRange && (request.includeRangeAuxiliaryChannels || request.includePointCloudColors))) {
            ImageEvidenceContext evidence;
            evidence.frameSequence = frameSequence;
            evidence.width = boundedExtent(parts.intensity.width);
            evidence.height = boundedExtent(parts.intensity.height);
            evidence.pixelFormat = static_cast<std::uint64_t>(parts.intensity.pixelType);
            evidence.readableBytes = parts.intensity.bytes;
            const auto reason = !parts.intensity.valid || !parts.intensity.data
                ? PylonImageRejection::Reason::MissingBytes
                : graphicsPixelFormat(parts.intensity.pixelType) == GraphicsImagePixelFormat::Unknown
                    ? PylonImageRejection::Reason::UnsupportedFormat : PylonImageRejection::Reason::InvalidStride;
            recordImageRejection(imageRejection, evidence, reason);
        }
        selectedParts.intensity = {};
        selectedRequest.includeRangeAuxiliaryChannels = false;
        selectedRequest.includePointCloudColors = false;
        selectedRequest.components = static_cast<GraphicsFrameComponent>(
            static_cast<std::uint8_t>(selectedRequest.components) & ~static_cast<std::uint8_t>(GraphicsFrameComponent::Image));
    }
    if (wantsRange && !rangeValid) {
        const auto& part = ace ? parts.disparity : parts.range;
        const auto reason = !part.valid ? PylonRangeRejection::Reason::MissingRange
            : !part.data ? PylonRangeRejection::Reason::MissingData
            : (ace && !profile.hasDisparityCalibration()) ? PylonRangeRejection::Reason::MissingCalibration
            : (ace ? part.pixelType != Pylon::PixelType_Coord3D_C16
                   : part.pixelType != Pylon::PixelType_Coord3D_C16 && part.pixelType != Pylon::PixelType_Coord3D_ABC32f)
                ? PylonRangeRejection::Reason::UnsupportedFormat : PylonRangeRejection::Reason::InvalidLayout;
        recordRangeRejection(rangeRejection, componentRangeContext(part, frameSequence), reason);
        selectedRequest.components = static_cast<GraphicsFrameComponent>(
            static_cast<std::uint8_t>(selectedRequest.components) & ~static_cast<std::uint8_t>(GraphicsFrameComponent::Range));
    }
    std::size_t sampleBytes = 0U;
    if (!planStereo(selectedParts, selectedRequest, profile, ace, frameSequence, sampleBytes)) {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        return std::nullopt;
    }
    if (sampleBytes == 0U) { if (storageError) *storageError = GraphicsFrameError::InvalidInput; return std::nullopt; }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    auto context = contextSource.beginFrame(sampleBytes);
    if (!context) { if (storageError) *storageError = context.error(); return std::nullopt; }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    if (ace) return buildStereoAceScene(selectedParts, selectedRequest, profile, context.value(), storageError,
                                        cancelled, frameSequence, imageRejection, rangeRejection);
    return buildDirectXyzScene(selectedParts, selectedRequest, profile, context.value(), storageError,
                               cancelled, "Basler Stereo mini", frameSequence, imageRejection, rangeRejection);
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertImageViewFrame(
    const CameraStereoPartView& part, GraphicsFrameContextSource& contextSource,
    std::uint64_t sequence, std::optional<PylonImageRejection>* rejection,
    GraphicsFrameError* storageError, const std::atomic<bool>* cancelled) const
{
    ImageEvidenceContext evidence;
    evidence.frameSequence = sequence; evidence.width = boundedExtent(part.width);
    evidence.height = boundedExtent(part.height); evidence.readableBytes = part.bytes;
    evidence.pixelFormat = static_cast<std::uint64_t>(part.pixelType); evidence.reportedStride = part.stride;
    if (!part.data || !part.bytes) {
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::MissingBytes);
        return std::nullopt;
    }
    if (!part.width || !part.height || part.width > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || part.height > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidDimensions);
        return std::nullopt;
    }
    auto format = graphicsPixelFormat(part.pixelType);
    if (format == GraphicsImagePixelFormat::Unknown) {
        std::uint32_t bits = 0U;
        try { bits = Pylon::BitPerPixel(part.pixelType); } catch (const Pylon::GenericException&) {}
        switch (bits) {
        case 16U: format = GraphicsImagePixelFormat::Mono16; break;
        case 24U: format = GraphicsImagePixelFormat::RGB24; break;
        case 32U: format = GraphicsImagePixelFormat::Mono32; break;
        default: format = GraphicsImagePixelFormat::Mono8; break;
        }
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::UnsupportedFormat);
    }
    const auto rowBytes = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(part.width));
    if (!rowBytes || rowBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || part.height > (std::numeric_limits<std::size_t>::max)() / rowBytes) {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        return std::nullopt;
    }
    std::size_t inputStride = part.hasStride ? part.stride : rowBytes;
    if (!part.hasStride && part.padding <= (std::numeric_limits<std::size_t>::max)() - rowBytes)
        inputStride += part.padding;
    if (inputStride < rowBytes)
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidStride);
    else if (inputStride && part.height > part.bytes / inputStride)
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::TruncatedBytes);
    GraphicsImageWriterSource layout;
    layout.data = static_cast<const std::uint8_t*>(part.data); layout.size = rowBytes * part.height;
    layout.width = static_cast<int>(part.width); layout.height = static_cast<int>(part.height);
    layout.stride = static_cast<int>(rowBytes); layout.pixelFormat = format;
    layout.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(format);
    layout.sourceFormat = static_cast<int>(part.pixelType); layout.frameSequence = sequence;
    auto plan = planGraphicsImageWrite(layout);
    if (!plan) { if (storageError) *storageError = plan.error(); return std::nullopt; }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected; return std::nullopt;
    }
    auto context = contextSource.beginFrame(plan.value().bytes);
    if (!context) {
        if (storageError) *storageError = context.error();
        if (context.error() == GraphicsFrameError::ClassRefused || context.error() == GraphicsFrameError::Capacity)
            recordImageRejection(rejection, evidence, PylonImageRejection::Reason::StorageRefused);
        return std::nullopt;
    }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected; return std::nullopt;
    }
    GraphicsFrameBuilder builder(context.value());
    auto reserved = builder.reserve<std::uint8_t>(plan.value().bytes, plan.value().alignment);
    if (!reserved) { if (storageError) *storageError = reserved.error(); return std::nullopt; }
    auto region = std::move(reserved).value();
    std::memset(region.data(), 0, region.size());
    const auto* input = static_cast<const std::uint8_t*>(part.data);
    for (std::size_t y = 0; y < part.height; ++y) {
        if (inputStride && y > (std::numeric_limits<std::size_t>::max)() / inputStride) break;
        const auto offset = y * inputStride;
        if (offset >= part.bytes) break;
        const auto count = (std::min)({rowBytes, inputStride, part.bytes - offset});
        std::memcpy(region.data() + y * rowBytes, input + offset, count);
    }
    GraphicsImage output;
    output.pixels = builder.seal(std::move(region)); output.width = layout.width; output.height = layout.height;
    output.stride = layout.stride; output.pixelFormat = format; output.bitsPerPixel = layout.bitsPerPixel;
    output.sourceFormat = layout.sourceFormat; output.frameSequence = sequence;
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected; return std::nullopt;
    }
    builder.addImage("image", std::move(output), "Image");
    auto result = std::move(builder).publish();
    if (!result) { if (storageError) *storageError = result.error(); return std::nullopt; }
    return std::move(result).value();
}
