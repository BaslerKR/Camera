#include "PylonGraphicsFrameAdapter.h"

#include <pylon/ImageFormatConverter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

struct Coord3DPoint
{
    float x;
    float y;
    float z;
};

[[nodiscard]] Pylon::CPylonDataComponent componentByType(
    const Pylon::CPylonDataContainer& container,
    const Pylon::EComponentType type)
{
    for (std::size_t index = 0; index < container.GetDataComponentCount(); ++index)
    {
        const auto component = container.GetDataComponent(index);
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

[[nodiscard]] bool componentStride(const Pylon::CPylonDataComponent& component,
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

    // Stereo mini reports multipart PaddingX that is not present in each
    // component's GetData() buffer. Use it only when the component size can
    // actually contain all padded rows.
    if (component.GetStride(reportedStride)
        && reportedStride >= packedStride
        && (height == 0U || reportedStride <= dataSize / height))
    {
        stride = reportedStride;
        return true;
    }

    if (height == 0U || packedStride <= dataSize / height)
    {
        stride = packedStride;
        return true;
    }

    return false;
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
    const Pylon::CPylonDataComponent& component,
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
    const GraphicsImageStorage& storage,
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
        writerSource, storage, rejection ? &failure : nullptr);
    if (!image.isValid() && rejection)
    {
        if (const auto reason = imageRejectionReason(failure))
        {
            const auto minimum = static_cast<std::uint64_t>(
                GraphicsImageValidation::minimumRowBytes(pixelFormat, width));
            const auto effective = stride > 0 ? static_cast<std::uint64_t>(stride) : minimum;
            recordImageRejection(rejection, evidenceContext, *reason,
                writerConversion(operation), effective, minimum,
                imageRequiredBytes(effective, minimum, evidenceContext.height));
        }
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
    const Pylon::CPylonDataComponent& component,
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
        {},
        GraphicsImageWriterOperation::Copy,
        context,
        rejection);
}

[[nodiscard]] bool pylonStride(const Pylon::CPylonImage& image, int& stride)
{
    std::size_t strideBytes = 0U;
    if (!image.GetStride(strideBytes)
        || strideBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
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

[[nodiscard]] GraphicsImage convertViaPylon(
    const Pylon::CPylonImage& pylonImage,
    const Pylon::EPixelType outputPixelType,
    const GraphicsImagePixelFormat pixelFormat,
    const int bitsPerPixel,
    const std::uint8_t declaredMonoSourceBits,
    const std::uint64_t frameSequence,
    const GraphicsImageStorage& storage,
    const ImageEvidenceContext& sourceContext,
    std::optional<PylonImageRejection>* rejection)
{
    Pylon::CPylonImage converted;
    Pylon::CImageFormatConverter converter;
    converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;
    converter.OutputPixelFormat = outputPixelType;
    converter.Convert(converted, pylonImage);

    ImageEvidenceContext context = sourceContext;
    context.readableBytes = converted.GetImageSize();
    int stride = 0;
    if (!pylonStride(converted, stride))
    {
        std::size_t reported = 0U;
        if (converted.GetStride(reported)) context.reportedStride = reported;
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidStride);
        return {};
    }
    const bool shiftMono16 = outputPixelType == Pylon::PixelType_Mono16
        && declaredMonoSourceBits > 0U
        && declaredMonoSourceBits < 16U;
    return writeImage(
        static_cast<int>(converted.GetWidth()),
        static_cast<int>(converted.GetHeight()),
        stride,
        bitsPerPixel,
        pixelFormat,
        static_cast<int>(pylonImage.GetPixelType()),
        converted.GetBuffer(),
        converted.GetImageSize(),
        declaredMonoSourceBits,
        frameSequence,
        storage,
        shiftMono16
            ? GraphicsImageWriterOperation::ShiftMono16
            : GraphicsImageWriterOperation::Copy,
        context,
        rejection);
}

[[nodiscard]] GraphicsImage pylonImageToGraphicsImage(
    const Pylon::CPylonImage& pylonImage,
    const std::uint64_t frameSequence,
    const GraphicsImageStorage& storage,
    std::optional<PylonImageRejection>* rejection)
{
    ImageEvidenceContext context;
    context.frameSequence = frameSequence;
    context.pixelFormat = static_cast<std::uint64_t>(pylonImage.GetPixelType());
    context.width = boundedExtent(pylonImage.GetWidth());
    context.height = boundedExtent(pylonImage.GetHeight());
    context.readableBytes = pylonImage.GetImageSize();
    std::size_t reportedStride = 0U;
    if (pylonImage.GetStride(reportedStride)) context.reportedStride = reportedStride;

    if (pylonImage.GetBuffer() == nullptr || pylonImage.GetImageSize() == 0U)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::MissingBytes);
        return {};
    }
    if (pylonImage.GetWidth() == 0 || pylonImage.GetHeight() == 0)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidDimensions);
        return {};
    }

    int stride = 0;
    if (pylonStride(pylonImage, stride))
    {
        const auto pixelType = pylonImage.GetPixelType();
        const auto pixelFormat = graphicsPixelFormat(pixelType);
        const int bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(pixelFormat);
        if (pixelFormat != GraphicsImagePixelFormat::Unknown && bitsPerPixel != 0)
        {
            return writeImage(
                static_cast<int>(pylonImage.GetWidth()),
                static_cast<int>(pylonImage.GetHeight()),
                stride,
                bitsPerPixel,
                pixelFormat,
                static_cast<int>(pixelType),
                pylonImage.GetBuffer(),
                pylonImage.GetImageSize(),
                0U,
                frameSequence,
                storage,
                GraphicsImageWriterOperation::Copy,
                context,
                rejection);
        }
    }
    else if (rejection && !*rejection)
    {
        recordImageRejection(rejection, context, PylonImageRejection::Reason::InvalidStride);
    }

    const auto pixelType = pylonImage.GetPixelType();
    if (shouldConvertToMono16(pixelType))
    {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_Mono16,
            GraphicsImagePixelFormat::Mono16,
            16,
            monoSourceBits(pixelType),
            frameSequence,
            storage,
            context,
            rejection);
    }

    // The conversion fallbacks keep their original order and outcomes. Only the
    // last generic failure is remembered when no attempt reached an actual write.
    bool conversionFailed = false;
    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_BGR8packed,
            GraphicsImagePixelFormat::BGR24,
            24,
            0U,
            frameSequence,
            storage,
            context,
            rejection);
    } catch (const Pylon::GenericException&) {
        conversionFailed = true;
    }
    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_Mono16,
            GraphicsImagePixelFormat::Mono16,
            16,
            0U,
            frameSequence,
            storage,
            context,
            rejection);
    } catch (const Pylon::GenericException&) {
        conversionFailed = true;
    }
    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_Mono8,
            GraphicsImagePixelFormat::Mono8,
            8,
            0U,
            frameSequence,
            storage,
            context,
            rejection);
    } catch (const Pylon::GenericException&) {
        conversionFailed = true;
    }
    if (conversionFailed)
    {
        recordImageRejection(rejection, context,
            PylonImageRejection::Reason::UnsupportedConversion);
    }
    else if (rejection && !*rejection)
    {
        recordImageRejection(rejection, context,
            PylonImageRejection::Reason::UnsupportedFormat);
    }
    return {};
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

void copyScalarIntensity(const Pylon::CPylonDataComponent& component, RangeFrame& frame)
{
    if (!component.IsValid() || component.GetData() == nullptr
        || (component.GetPixelType() != Pylon::PixelType_Mono8
            && component.GetPixelType() != Pylon::PixelType_Mono16))
    {
        return;
    }

    const int sourceWidth = static_cast<int>(component.GetWidth());
    const int sourceHeight = static_cast<int>(component.GetHeight());
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return;
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride))
    {
        return;
    }

    const auto* data = static_cast<const std::uint8_t*>(component.GetData());
    frame.intensity.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
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
            frame.intensity[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                          + static_cast<std::size_t>(x)] = value;
        }
    }
}

void copyPointCloudRgb(const Pylon::CPylonDataComponent& component, RangeFrame& frame)
{
    if (!component.IsValid() || component.GetData() == nullptr
        || (component.GetPixelType() != Pylon::PixelType_RGB8packed
            && component.GetPixelType() != Pylon::PixelType_RGBA8packed))
    {
        return;
    }

    const int sourceWidth = static_cast<int>(component.GetWidth());
    const int sourceHeight = static_cast<int>(component.GetHeight());
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return;
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride))
    {
        return;
    }

    const std::size_t channels = component.GetPixelType() == Pylon::PixelType_RGB8packed ? 3U : 4U;
    const auto* data = static_cast<const std::uint8_t*>(component.GetData());
    frame.rgb.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U);
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
            frame.rgb[target] = source[0];
            frame.rgb[target + 1U] = source[1];
            frame.rgb[target + 2U] = source[2];
        }
    }
}

void appendColorImage(const Pylon::CPylonDataComponent& intensity,
                      const GraphicsFrameRequest& request,
                      const PylonScene3DProfile& profile,
                      GraphicsFrame& scene,
                      const std::uint64_t frameSequence = 0U,
                      std::optional<PylonImageRejection>* rejection = nullptr)
{
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image))
    {
        return;
    }

    GraphicsImage image = componentToGraphicsImage(intensity, frameSequence, rejection);
    if (!image.isValid())
    {
        return;
    }

    scene.addImage("image", std::move(image), "Image", "primary");
    scene.images.back().descriptor.registration = GraphicsImageRegistration::Unregistered;
}

void registerColorImageToRange(
    const PylonScene3DProfile& profile,
    GraphicsFrame& scene)
{
    if (!profile.colorRegisteredToRange || scene.images.empty() || scene.ranges.empty())
    {
        return;
    }
    auto& descriptor = scene.images.front().descriptor;
    descriptor.registeredResourceId = scene.ranges.front().descriptor.id;
    descriptor.registration = GraphicsImageRegistration::RegisteredToRange;
}

[[nodiscard]] std::optional<GraphicsFrame> buildDirectXyzScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    const char* sourceName,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection)
{
    GraphicsFrame scene;
    scene.metadata.sourceName = sourceName;
    InitialView3D view;
    view.lookDirection = {0.0, 0.0, 1.0};
    view.viewUp = {0.0, -1.0, 0.0};
    view.parallelProjection = false;
    view.distanceScale = 1.15;

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene, frameSequence, imageRejection);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range))
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto range = componentByType(container, Pylon::ComponentType_Range);
    const auto rangeContext = componentRangeContext(range, frameSequence);
    if (!range.IsValid())
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::MissingRange);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }
    if (range.GetData() == nullptr)
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::MissingData);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto pixelType = range.GetPixelType();
    if (pixelType != Pylon::PixelType_Coord3D_ABC32f && pixelType != Pylon::PixelType_Coord3D_C16)
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::UnsupportedFormat);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    std::size_t stride = 0U;
    if (!componentStride(range, stride))
    {
        recordRangeRejection(rangeRejection, rangeContext,
                             PylonRangeRejection::Reason::InvalidLayout);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
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
    frame.xValues.resize(count);
    frame.yValues.resize(count);
    frame.zValues.resize(count);
    frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    frame.validMask.resize(count);
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
                frame.xValues[index] = point.x;
                frame.yValues[index] = point.y;
                frame.zValues[index] = point.z;
                frame.validMask[index] = std::isfinite(point.x)
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
                    frame.xValues[index] = std::numeric_limits<float>::quiet_NaN();
                    frame.yValues[index] = std::numeric_limits<float>::quiet_NaN();
                    frame.zValues[index] = std::numeric_limits<float>::quiet_NaN();
                    frame.validMask[index] = 0U;
                    continue;
                }

                frame.xValues[index] = static_cast<float>((static_cast<double>(x) - profile.principalPointU)
                                                           * z / profile.focalLength);
                frame.yValues[index] = static_cast<float>((static_cast<double>(y) - profile.principalPointV)
                                                           * z / profile.focalLength);
                frame.zValues[index] = static_cast<float>(z);
                frame.validMask[index] = 1U;
            }
        }
    }

    if (request.includeRangeAuxiliaryChannels)
    {
        copyScalarIntensity(intensity, frame);
        frame.intensityField.bitsPerSample = frame.intensityBits;
    }
    if (request.includePointCloudColors)
    {
        copyPointCloudRgb(intensity, frame);
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
    return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
}

[[nodiscard]] std::optional<GraphicsFrame> buildStereoAceScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection)
{
    GraphicsFrame scene;
    scene.metadata.sourceName = "Basler Stereo ace";

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene, frameSequence, imageRejection);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range))
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }
    if (!profile.hasDisparityCalibration())
    {
        RangeEvidenceContext noRange;
        noRange.frameSequence = frameSequence;
        recordRangeRejection(rangeRejection, noRange,
                             PylonRangeRejection::Reason::MissingCalibration);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto disparity = componentByType(container, Pylon::ComponentType_Disparity);
    const auto disparityContext = componentRangeContext(disparity, frameSequence);
    if (!disparity.IsValid())
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::MissingRange);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }
    if (disparity.GetPixelType() != Pylon::PixelType_Coord3D_C16)
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::UnsupportedFormat);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }
    if (disparity.GetData() == nullptr)
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::MissingData);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    std::size_t stride = 0U;
    if (!componentStride(disparity, stride))
    {
        recordRangeRejection(rangeRejection, disparityContext,
                             PylonRangeRejection::Reason::InvalidLayout);
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
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
    frame.xValues.resize(count);
    frame.yValues.resize(count);
    frame.zValues.resize(count);
    frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    frame.validMask.resize(count);
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
                frame.xValues[index] = std::numeric_limits<float>::quiet_NaN();
                frame.yValues[index] = std::numeric_limits<float>::quiet_NaN();
                frame.zValues[index] = std::numeric_limits<float>::quiet_NaN();
                frame.validMask[index] = 0U;
                continue;
            }

            const float z = static_cast<float>(1000.0 * profile.baseline * profile.focalLength / calibrated);
            frame.xValues[index] = static_cast<float>((static_cast<double>(x) - profile.principalPointU)
                                                       * z / profile.focalLength);
            frame.yValues[index] = static_cast<float>((static_cast<double>(y) - profile.principalPointV)
                                                       * z / profile.focalLength);
            frame.zValues[index] = z;
            frame.validMask[index] = 1U;
        }
    }

    if (request.includeRangeAuxiliaryChannels)
    {
        copyScalarIntensity(intensity, frame);
        frame.intensityField.bitsPerSample = frame.intensityBits;
    }
    if (request.includePointCloudColors)
    {
        copyPointCloudRgb(intensity, frame);
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
    return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
}

} // namespace

void PylonGraphicsFrameAdapter::setFrameStorage(GraphicsImageStorage storage)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _storage = storage;
    _blazeAdapter.setFrameStorage(std::move(storage));
}

GraphicsImage PylonGraphicsFrameAdapter::convertGraphicsImage(
    const Pylon::CPylonImage& image,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* rejection) const
{
    GraphicsImageStorage storage;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        storage = _storage;
    }
    return pylonImageToGraphicsImage(image, frameSequence, storage, rejection);
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    const std::uint64_t frameSequence,
    std::optional<PylonImageRejection>* imageRejection,
    std::optional<PylonRangeRejection>* rangeRejection) const
{
    std::optional<GraphicsFrame> result;
    switch (profile.family)
    {
    case PylonScene3DProfile::DeviceFamily::Blaze:
        result = _blazeAdapter.convertGraphicsFrame(
            container, request, frameSequence, rangeRejection);
        break;
    case PylonScene3DProfile::DeviceFamily::StereoMini:
        result = buildDirectXyzScene(container, request, profile, "Basler Stereo mini",
                                     frameSequence, imageRejection, rangeRejection);
        break;
    case PylonScene3DProfile::DeviceFamily::StereoAce:
        result = buildStereoAceScene(container, request, profile, frameSequence,
                                     imageRejection, rangeRejection);
        break;
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }

    if (rangeRejection && *rangeRejection)
    {
        (*rangeRejection)->frameRetained = result.has_value();
        (*rangeRejection)->imageRetained = result && result->hasImage();
        (*rangeRejection)->rangeRetained = result && result->hasRangeFrame();
    }
    return result;
}
