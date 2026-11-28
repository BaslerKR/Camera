#include "PylonGraphicsFrameAdapter.h"

#include "engine/detail/GraphicsWriteLayout.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"

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
        case Pylon::ComponentType_Intensity: if (!parts.intensity.IsValid()) { parts.intensity = stereoPartView(part); parts.intensity.index = static_cast<std::uint32_t>(i); } break;
        case Pylon::ComponentType_Range: if (!parts.range.IsValid()) { parts.range = stereoPartView(part); parts.range.index = static_cast<std::uint32_t>(i); } break;
        case Pylon::ComponentType_Disparity: if (!parts.disparity.IsValid()) { parts.disparity = stereoPartView(part); parts.disparity.index = static_cast<std::uint32_t>(i); } break;
        default: break;
        }
    }
    return parts;
}

template <typename T>
[[nodiscard]] T stereoSample(const CameraStereoPartView& part, std::size_t stride,
                             std::size_t x, std::size_t y) noexcept
{
    T value{};
    const auto max = (std::numeric_limits<std::size_t>::max)();
    if (!part.data || !stride || y > max / stride || x > max / sizeof(T)) return value;
    const auto row = y * stride;
    const auto column = x * sizeof(T);
    if (column >= stride || row > max - column) return value;
    const auto offset = row + column;
    if (offset >= part.bytes) return value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(part.data) + offset,
                (std::min)({sizeof(T), stride - column, part.bytes - offset}));
    return value;
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

constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
constexpr std::size_t kSizeMax = (std::numeric_limits<std::size_t>::max)();

/** Collects every Stereo part anomaly of one frame: the first one and the bounded per-frame list. */
struct StereoReport
{
    using Reason = PylonRangeRejection::Reason;
    std::optional<PylonRangeRejection>* first = nullptr;
    PylonFrameAnomalies* list = nullptr;
    std::uint64_t sequence = 0U;

    void add(const CameraStereoPartView& part, const Reason reason, const std::uint64_t minimumRow = 0U,
             const std::uint64_t effectiveStride = 0U, const std::uint64_t required = 0U) const
    {
        if (!first && !list) return;
        PylonRangeRejection evidence;
        evidence.reason = reason;
        evidence.affectedPart = part.index;
        evidence.width = boundedExtent(part.width);
        evidence.height = boundedExtent(part.height);
        evidence.frameSequence = sequence;
        evidence.pixelFormat = static_cast<std::uint64_t>(part.pixelType);
        evidence.reportedStride = part.hasStride ? part.stride : 0U;
        evidence.effectiveStride = effectiveStride;
        evidence.minimumRowBytes = minimumRow;
        evidence.readableBytes = part.bytes;
        evidence.requiredBytes = required;
        if (list) list->add(evidence);
        if (first && !*first) *first = evidence;
    }
};

/** Bytes a part needs to hold every row at its effective stride; saturates on overflow. */
[[nodiscard]] std::size_t stereoRequiredBytes(std::size_t rowBytes, std::size_t stride, std::size_t rows) noexcept
{
    if (!rows) return 0U;
    const auto last = (std::min)(rowBytes, stride);
    if (rows > 1U && stride > kSizeMax / (rows - 1U)) return kSizeMax;
    const auto lead = (rows - 1U) * stride;
    return last > kSizeMax - lead ? kSizeMax : lead + last;
}

/** SDK row pitch of a part: reported stride, else packed row plus padding. Zero falls back to the row size. */
[[nodiscard]] std::size_t stereoStride(const CameraStereoPartView& part, std::size_t rowBytes) noexcept
{
    std::size_t stride = rowBytes;
    if (part.hasStride) stride = part.stride;
    else if (part.padding <= kSizeMax - rowBytes) stride = rowBytes + part.padding;
    return stride ? stride : rowBytes;
}

/** Records the stride and truncation anomalies of a part; the copy that follows never reads past its bytes. */
void diagnoseStereoLayout(const StereoReport& report, const CameraStereoPartView& part,
                          std::size_t rowBytes, std::size_t stride, std::size_t rows)
{
    const auto required = stereoRequiredBytes(rowBytes, stride, rows);
    if (part.hasStride && part.stride < rowBytes)
        report.add(part, StereoReport::Reason::StrideBelowMinimum, rowBytes, stride, required);
    if (part.bytes < required)
        report.add(part, StereoReport::Reason::TruncatedData, rowBytes, stride, required);
}

[[nodiscard]] std::size_t stereoElementBytes(const Pylon::EPixelType type) noexcept
{
    switch (graphicsPixelFormat(type)) {
    case GraphicsImagePixelFormat::Mono8: return 1U;
    case GraphicsImagePixelFormat::Mono16: return 2U;
    case GraphicsImagePixelFormat::RGB24:
    case GraphicsImagePixelFormat::BGR24: return 3U;
    case GraphicsImagePixelFormat::RGBA32:
    case GraphicsImagePixelFormat::BGRA32: return 4U;
    default: break;
    }
    std::uint32_t bits = 0U;
    try { bits = Pylon::BitPerPixel(type); } catch (...) {}
    return bits != 0U && bits % 8U == 0U ? bits / 8U : 1U;
}

[[nodiscard]] GraphicsImagePixelFormat stereoFormatForBytes(const std::size_t bytes) noexcept
{
    switch (bytes) {
    case 2U: return GraphicsImagePixelFormat::Mono16;
    case 3U: return GraphicsImagePixelFormat::RGB24;
    case 4U: return GraphicsImagePixelFormat::Mono32;
    default: return GraphicsImagePixelFormat::Mono8;
    }
}

/** One planned image part: its own declared extent, a valid GE stride, and its reserved layout. */
struct StereoImagePlan
{
    bool present = false;
    bool degenerate = false;
    GraphicsImagePixelFormat format = GraphicsImagePixelFormat::Mono8;
    int width = 0, height = 0;
    std::size_t rowBytes = 0U, stride = 0U;
    GraphicsImageWriteLayout layout;
};

/**
 * Plans a part as an image at its own width, height and element size. A format GE does not know
 * maps to the GE type with the same bytes per element, else to raw bytes. `because` names the
 * reason a range part is published as an image instead of a range, recorded once for the part.
 */
[[nodiscard]] StereoImagePlan planStereoImage(const CameraStereoPartView& part, const StereoReport& report,
    const PylonRangeRejection::Reason absentReason, const std::optional<PylonRangeRejection::Reason> because)
{
    using Reason = StereoReport::Reason;
    StereoImagePlan plan;
    if (!part.IsValid()) { report.add(part, absentReason); return plan; }
    if (!part.data || !part.bytes) { report.add(part, Reason::MissingData); return plan; }
    auto format = graphicsPixelFormat(part.pixelType);
    const bool unexpected = format == GraphicsImagePixelFormat::Unknown;
    const auto elementBytes = stereoElementBytes(part.pixelType);
    if (unexpected) format = stereoFormatForBytes(elementBytes);
    if (because) report.add(part, *because);
    else if (unexpected) report.add(part, Reason::UnsupportedFormat);

    std::size_t imageWidth = part.width;
    plan.degenerate = !part.width || !part.height || part.width > kIntMax || part.height > kIntMax;
    if (plan.degenerate) {
        report.add(part, Reason::InvalidDimensions);
        if (part.bytes > kIntMax) { report.add(part, Reason::SizeLimitExceeded); return plan; }
        format = GraphicsImagePixelFormat::Mono8; imageWidth = part.bytes;
        plan.height = 1;
    } else {
        plan.height = static_cast<int>(part.height);
        if (unexpected && elementBytes > 4U) {
            if (part.width > kIntMax / elementBytes) { report.add(part, Reason::SizeLimitExceeded); return plan; }
            imageWidth = part.width * elementBytes;
        }
    }
    const auto rowBytes = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(imageWidth));
    if (!rowBytes || rowBytes > kIntMax || static_cast<std::size_t>(plan.height) > kSizeMax / rowBytes) {
        report.add(part, Reason::SizeLimitExceeded); return plan;
    }
    plan.format = format; plan.width = static_cast<int>(imageWidth); plan.rowBytes = rowBytes;
    plan.stride = plan.degenerate ? rowBytes : stereoStride(part, rowBytes);
    if (!plan.degenerate) diagnoseStereoLayout(report, part, rowBytes, plan.stride, part.height);
    GraphicsImageWriterSource source;
    source.data = static_cast<const std::uint8_t*>(part.data);
    source.width = plan.width; source.height = plan.height; source.pixelFormat = format;
    source.stride = static_cast<int>(rowBytes); source.size = rowBytes * static_cast<std::size_t>(plan.height);
    const auto layout = planGraphicsImageWrite(source);
    if (!layout) { report.add(part, Reason::SizeLimitExceeded); return plan; }
    plan.layout = layout.value();
    plan.present = true;
    return plan;
}

/** Copies what the SDK buffer holds, row by row, never past its end; the rest stays zero. */
void copyStereoRows(std::uint8_t* target, const std::size_t rowBytes, const std::size_t rows,
                    const CameraStereoPartView& part, const std::size_t stride) noexcept
{
    const auto* data = static_cast<const std::uint8_t*>(part.data);
    for (std::size_t y = 0; y < rows; ++y) {
        if (stride && y > kSizeMax / stride) break;
        const auto offset = y * stride;
        if (offset >= part.bytes) break;
        std::memcpy(target + y * rowBytes, data + offset, (std::min)({rowBytes, stride, part.bytes - offset}));
    }
}

[[nodiscard]] GraphicsResult<GraphicsImage> writeStereoImage(
    GraphicsFrameBuilder& builder, const CameraStereoPartView& part, const StereoImagePlan& plan,
    const std::uint64_t frameSequence)
{
    auto reserved = builder.reserve<std::uint8_t>(plan.layout.bytes, plan.layout.alignment);
    if (!reserved) return reserved.error();
    auto region = std::move(reserved).value();
    std::memset(region.data(), 0, region.size());
    copyStereoRows(region.data(), plan.rowBytes, static_cast<std::size_t>(plan.height), part, plan.stride);
    GraphicsImage image;
    image.pixels = builder.seal(std::move(region));
    image.width = plan.width; image.height = plan.height;
    image.stride = plan.layout.strideBytes; image.pixelFormat = plan.layout.format;
    image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(plan.layout.format);
    image.sourceFormat = static_cast<int>(part.pixelType); image.frameSequence = frameSequence;
    return image;
}

[[nodiscard]] std::size_t mappedIndex(const int targetX, const int targetY, const int targetWidth,
                                      const int targetHeight, const int sourceWidth, const int sourceHeight) noexcept
{
    const int sourceX = std::min(sourceWidth - 1, targetX * sourceWidth / targetWidth);
    const int sourceY = std::min(sourceHeight - 1, targetY * sourceHeight / targetHeight);
    return static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(sourceWidth)
         + static_cast<std::size_t>(sourceX);
}

enum class StereoRangeKind { None, Xyz, Depth, Disparity, Raw };

/** One planned range part: a range frame built from the samples, or the same bytes as a raw image. */
struct StereoRangePlan
{
    StereoRangeKind kind = StereoRangeKind::None;
    int width = 0, height = 0;
    std::size_t count = 0U, stride = 0U;
    StereoImagePlan raw;
    bool intensity = false, rgb = false;
    // One GE range block carries every channel, so a frame reserves a small, fixed number of regions.
    std::size_t blockBytes = 0U, blockAlignment = 1U;
};

struct StereoPlan
{
    StereoImagePlan image;
    StereoRangePlan range;
    std::size_t bytes = 0U;
};

[[nodiscard]] bool depthCalibrated(const PylonScene3DProfile& profile) noexcept
{
    return std::isfinite(profile.coordinateScale) && profile.coordinateScale > 0.0
        && std::isfinite(profile.coordinateOffset)
        && std::isfinite(profile.focalLength) && profile.focalLength > 0.0
        && std::isfinite(profile.principalPointU) && std::isfinite(profile.principalPointV);
}

[[nodiscard]] PylonRangeRejection::Reason calibrationReason(const PylonScene3DProfile& profile) noexcept
{
    return profile.coordinateScale == 0.0 && profile.focalLength == 0.0 && profile.baseline == 0.0
        ? PylonRangeRejection::Reason::MissingCalibration : PylonRangeRejection::Reason::InvalidCalibration;
}

[[nodiscard]] StereoRangePlan planStereoRange(const CameraStereoPartView& part, const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile, const bool ace, const StereoImagePlan& intensity,
    const CameraStereoPartView& intensityPart, const StereoReport& report)
{
    using Reason = StereoReport::Reason;
    StereoRangePlan plan;
    if (!part.IsValid()) { report.add(part, Reason::MissingRange); return plan; }
    if (!part.data || !part.bytes) { report.add(part, Reason::MissingData); return plan; }
    std::optional<Reason> because;
    const auto type = part.pixelType;
    if (type == Pylon::PixelType_Coord3D_ABC32f) {
        plan.kind = StereoRangeKind::Xyz;
        if (ace) report.add(part, Reason::UnsupportedFormat);
    } else if (type == Pylon::PixelType_Coord3D_C16) {
        if (ace) {
            if (profile.hasDisparityCalibration()) plan.kind = StereoRangeKind::Disparity;
            else because = calibrationReason(profile);
        } else {
            if (depthCalibrated(profile)) plan.kind = StereoRangeKind::Depth;
            else because = calibrationReason(profile);
        }
    } else {
        because = Reason::UnsupportedFormat;
    }
    const auto elementBytes = type == Pylon::PixelType_Coord3D_ABC32f ? sizeof(Coord3DPoint) : sizeof(std::uint16_t);
    if (plan.kind != StereoRangeKind::None) {
        const bool sound = part.width && part.height && part.width <= kIntMax && part.height <= kIntMax
            && part.height <= kSizeMax / part.width && part.width * part.height <= kSizeMax / sizeof(float) / 3U
            && part.width <= kSizeMax / elementBytes;
        if (!sound) {
            plan.kind = StereoRangeKind::None;
            because = std::nullopt;
        }
    }
    if (plan.kind == StereoRangeKind::None) {
        plan.raw = planStereoImage(part, report, Reason::MissingRange, because);
        if (plan.raw.present) plan.kind = StereoRangeKind::Raw;
        return plan;
    }
    plan.width = static_cast<int>(part.width); plan.height = static_cast<int>(part.height);
    plan.count = part.width * part.height;
    const auto rowBytes = part.width * elementBytes;
    plan.stride = stereoStride(part, rowBytes);
    if (intensity.present && !intensity.degenerate) {
        const auto imageType = intensityPart.pixelType;
        plan.intensity = request.includeRangeAuxiliaryChannels
            && (imageType == Pylon::PixelType_Mono8 || imageType == Pylon::PixelType_Mono16);
        plan.rgb = request.includePointCloudColors
            && (imageType == Pylon::PixelType_RGB8packed || imageType == Pylon::PixelType_RGBA8packed);
    }
    GraphicsRangeFrameLayout layout;
    layout.width = plan.width; layout.height = plan.height;
    layout.channels.x = true; layout.channels.y = true; layout.channels.validMask = true;
    layout.channels.intensity = plan.intensity; layout.channels.rgb = plan.rgb;
    const auto block = planGraphicsRangeWrite(layout);
    if (!block) {
        report.add(part, Reason::SizeLimitExceeded);
        plan.raw = planStereoImage(part, report, Reason::MissingRange, std::nullopt);
        plan.kind = plan.raw.present ? StereoRangeKind::Raw : StereoRangeKind::None;
        return plan;
    }
    plan.blockBytes = block.value().totalBytes; plan.blockAlignment = block.value().alignment;
    diagnoseStereoLayout(report, part, rowBytes, plan.stride, part.height);
    return plan;
}

/** Plans every selected part; the byte sum follows the order in which the build reserves. */
[[nodiscard]] bool planStereo(const StereoParts& parts, const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile, const bool ace, const StereoReport& report, StereoPlan& plan)
{
    const bool wantsImage = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image);
    const bool wantsRange = hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range);
    const bool planIntensity = wantsImage
        || (wantsRange && (request.includeRangeAuxiliaryChannels || request.includePointCloudColors));
    const auto& rangePart = ace ? parts.disparity : parts.range;
    // Quiet plan first: the range part reports first and needs the intensity layout for its channels.
    const StereoReport quiet;
    StereoImagePlan intensity;
    if (planIntensity)
        intensity = planStereoImage(parts.intensity, quiet, StereoReport::Reason::MissingData, std::nullopt);
    if (wantsRange) plan.range = planStereoRange(rangePart, request, profile, ace, intensity, parts.intensity, report);
    if (planIntensity)
        (void)planStereoImage(parts.intensity, report, StereoReport::Reason::MissingData, std::nullopt);
    if (wantsImage && intensity.present) {
        plan.image = intensity;
        if (!appendAligned(plan.bytes, plan.image.layout.bytes, plan.image.layout.alignment)) return false;
    }
    const auto& range = plan.range;
    switch (range.kind) {
    case StereoRangeKind::None: break;
    case StereoRangeKind::Raw:
        if (!appendAligned(plan.bytes, range.raw.layout.bytes, range.raw.layout.alignment)) return false;
        break;
    default:
        if (!appendAligned(plan.bytes, range.blockBytes, range.blockAlignment)) return false;
        break;
    }
    return true;
}

void fillIntensity(const CameraStereoPartView& part, const std::size_t stride, const int sourceWidth,
                   const int sourceHeight, const int width, const int height, float* out) noexcept
{
    const bool mono8 = part.pixelType == Pylon::PixelType_Mono8;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto index = mappedIndex(x, y, width, height, sourceWidth, sourceHeight);
            const auto sx = index % static_cast<std::size_t>(sourceWidth);
            const auto sy = index / static_cast<std::size_t>(sourceWidth);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = mono8
                ? static_cast<float>(stereoSample<std::uint8_t>(part, stride, sx, sy))
                : static_cast<float>(stereoSample<std::uint16_t>(part, stride, sx, sy));
        }
}

void fillRgb(const CameraStereoPartView& part, const std::size_t stride, const int sourceWidth,
             const int sourceHeight, const int width, const int height, std::uint8_t* out) noexcept
{
    const std::size_t channels = part.pixelType == Pylon::PixelType_RGB8packed ? 3U : 4U;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto index = mappedIndex(x, y, width, height, sourceWidth, sourceHeight);
            const auto sx = index % static_cast<std::size_t>(sourceWidth);
            const auto sy = index / static_cast<std::size_t>(sourceWidth);
            const auto target = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                                 + static_cast<std::size_t>(x)) * 3U;
            for (std::size_t c = 0; c < 3U; ++c)
                out[target + c] = stereoSample<std::uint8_t>(part, stride, sx * channels + c, sy);
        }
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

/** Builds one frame from the planned parts, in the order the plan summed their reservations. */
[[nodiscard]] std::optional<GraphicsFrame> buildStereoFrame(
    const StereoParts& parts, const StereoPlan& plan, const PylonScene3DProfile& profile, const bool ace,
    GraphicsFrameContext& context, GraphicsFrameError* storageError, const std::atomic<bool>* cancelled,
    const std::uint64_t frameSequence)
{
    GraphicsFrameBuilder builder(context);
    auto& scene = builder.content();
    scene.metadata.sourceName = ace ? "Basler Stereo ace" : "Basler Stereo mini";
    // A reserve larger than the plan breaks the producer contract and is reported explicitly.
    const auto refused = [&](const GraphicsFrameError error) -> std::optional<GraphicsFrame> {
        if (storageError) *storageError = error == GraphicsFrameError::Capacity ? GraphicsFrameError::Allocation : error;
        return std::nullopt;
    };
    if (plan.image.present) {
        auto image = writeStereoImage(builder, parts.intensity, plan.image, frameSequence);
        if (!image) return refused(image.error());
        scene.addImage("image", std::move(image).value(), "Image", "primary");
        scene.images.back().descriptor.registration = GraphicsImageRegistration::Unregistered;
    }
    const auto& range = plan.range;
    const auto& part = ace ? parts.disparity : parts.range;
    if (range.kind == StereoRangeKind::Raw) {
        auto image = writeStereoImage(builder, part, range.raw, frameSequence);
        if (!image) return refused(image.error());
        scene.addImage("range-raw", std::move(image).value(), "Range", "primary");
    } else if (range.kind != StereoRangeKind::None) {
        const bool xyz = range.kind == StereoRangeKind::Xyz;
        // The descriptor strings live in a per-thread frame, so a warm producer allocates nothing.
        thread_local RangeFrame frame;
        struct ReleaseSpans {
            RangeFrame& value;
            ~ReleaseSpans() {
                value.xValues = {}; value.yValues = {}; value.zValues = {}; value.validMask = {};
                value.intensity = {}; value.rgb = {};
            }
        } releaseSpans{frame};
        const auto describe = [](MeasurementFieldDescriptor& field, const char* id, const char* name,
                                 const char* unit, const MeasurementValueDomain domain, const std::uint8_t bits) {
            field.id.assign(id); field.displayName.assign(name); field.unitSymbol.assign(unit);
            field.domain = domain; field.sampleKind = MeasurementSampleKind::GridSample; field.bitsPerSample = bits;
        };
        frame.width = range.width; frame.height = range.height;
        frame.lengthUnit = GraphicsLengthUnit::Millimeter;
        if (xyz) describe(frame.rangeField, "basler.xyz.z-coordinate", "Z coordinate", "mm",
                          MeasurementValueDomain::Calibrated, 32U);
        else describe(frame.rangeField, ace ? "basler.stereo-ace.depth" : "basler.depth", "Depth", "mm",
                      MeasurementValueDomain::Calibrated, 16U);
        describe(frame.intensityField, ace ? "basler.stereo-ace.intensity" : "basler.intensity", "Intensity", "",
                 MeasurementValueDomain::Native, 0U);
        frame.sensorType.assign(ace ? "Basler Stereo ace" : "Basler Stereo mini");
        frame.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
        frame.intensityBits = 0U;
        GraphicsRangeFrameLayout layout;
        layout.width = range.width; layout.height = range.height;
        layout.channels.x = true; layout.channels.y = true; layout.channels.validMask = true;
        layout.channels.intensity = range.intensity; layout.channels.rgb = range.rgb;
        GraphicsRangeFrameWriter writer(builder, layout);
        if (!writer.isValid()) return refused(GraphicsFrameError::Allocation);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        float* const xv = writer.xData(); float* const yv = writer.yData(); float* const zv = writer.zData();
        std::uint8_t* const mv = writer.validMaskData();
        const auto width = static_cast<std::size_t>(range.width);
        for (std::size_t y = 0; y < static_cast<std::size_t>(range.height); ++y)
            for (std::size_t x = 0; x < width; ++x) {
                const auto index = y * width + x;
                if (xyz) {
                    const auto point = stereoSample<Coord3DPoint>(part, range.stride, x, y);
                    xv[index] = point.x; yv[index] = point.y; zv[index] = point.z;
                    mv[index] = std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
                        && point.z > 0.0F ? 1U : 0U;
                    continue;
                }
                const auto raw = stereoSample<std::uint16_t>(part, range.stride, x, y);
                const double scaled = static_cast<double>(raw) * profile.coordinateScale + profile.coordinateOffset;
                const double depth = range.kind == StereoRangeKind::Disparity
                    ? 1000.0 * profile.baseline * profile.focalLength / scaled : scaled;
                if (raw == 0U || scaled <= 0.0 || !std::isfinite(depth) || depth <= 0.0) {
                    xv[index] = nan; yv[index] = nan; zv[index] = nan; mv[index] = 0U;
                    continue;
                }
                const float z = static_cast<float>(depth);
                xv[index] = static_cast<float>((static_cast<double>(x) - profile.principalPointU) * z / profile.focalLength);
                yv[index] = static_cast<float>((static_cast<double>(y) - profile.principalPointV) * z / profile.focalLength);
                zv[index] = z; mv[index] = 1U;
            }
        const auto& intensityPart = parts.intensity;
        const auto sourceWidth = static_cast<int>(intensityPart.width);
        const auto sourceHeight = static_cast<int>(intensityPart.height);
        const auto intensityStride = stereoStride(intensityPart,
            intensityPart.width * stereoElementBytes(intensityPart.pixelType));
        if (range.intensity) {
            fillIntensity(intensityPart, intensityStride, sourceWidth, sourceHeight,
                          range.width, range.height, writer.intensityData());
            frame.intensityBits = intensityPart.pixelType == Pylon::PixelType_Mono8 ? 8U : 16U;
            frame.intensityField.bitsPerSample = frame.intensityBits;
        }
        if (range.rgb)
            fillRgb(intensityPart, intensityStride, sourceWidth, sourceHeight,
                    range.width, range.height, writer.rgbData());
        const RangeFrame taken = writer.takeFrame();
        frame.xValues = taken.xValues; frame.yValues = taken.yValues; frame.zValues = taken.zValues;
        frame.validMask = taken.validMask; frame.intensity = taken.intensity; frame.rgb = taken.rgb;
        if (frame.isValid()) {
            scene.addRange("range", frame, "Range", "primary");
            if (!ace) {
                InitialView3D view;
                view.lookDirection = {0.0, 0.0, 1.0};
                view.viewUp = {0.0, -1.0, 0.0};
                view.parallelProjection = false;
                view.distanceScale = 1.15;
                scene.ranges.back().initialView = view;
            }
            if (profile.colorRegisteredToRange && plan.image.present && !scene.images.empty()) {
                auto& descriptor = scene.images.front().descriptor;
                descriptor.registeredResourceId = scene.ranges.front().descriptor.id;
                descriptor.registration = GraphicsImageRegistration::RegisteredToRange;
            }
        }
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
    // Whatever the converter cannot take is fitted from the SDK bytes as they are.
    const auto asView = [&]() {
        CameraStereoPartView part;
        part.data = image.GetBuffer(); part.bytes = image.GetImageSize();
        part.width = image.GetWidth(); part.height = image.GetHeight();
        part.pixelType = image.GetPixelType(); part.valid = true;
        part.hasStride = image.GetStride(part.stride);
        return convertImageViewFrame(part, contextSource, frameSequence, rejection, storageError, cancelled);
    };
    const auto directFormat = graphicsPixelFormat(image.GetPixelType());
    if (directFormat != GraphicsImagePixelFormat::Unknown
        || (!shouldConvertToMono16(image.GetPixelType())
            && image.GetPixelType() != Pylon::PixelType_RGB8planar)) {
        return asView();
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
        return asView();
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
            return asView();
        }
        source.stride = static_cast<int>(source.size / image.GetHeight());
    }
    if (!planPreparedImage(*prepared, rejection)) return asView();

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
        std::memset(region.data(), 0, region.size());
        try {
            converter.Convert(region.data(), region.size(), image);
        } catch (const Pylon::GenericException&) {
            // The conversion failed; the zero-filled image is published and the failure is counted.
            recordImageRejection(rejection, evidence, PylonImageRejection::Reason::UnsupportedConversion);
        }
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
                                    frameSequence, rangeRejection, storageError, cancelled, anomalies);
        break;
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }

    const auto partCount = boundedExtent(container.GetDataComponentCount());
    const auto annotate = [&](PylonRangeRejection& evidence) {
        evidence.frameRetained = result.has_value();
        evidence.imageRetained = result && result->hasImage();
        evidence.rangeRetained = result && result->hasRangeFrame();
        evidence.partCount = partCount;
    };
    if (rangeRejection && *rangeRejection) annotate(**rangeRejection);
    if (anomalies)
        for (std::size_t i = 0; i < anomalies->count; ++i) annotate(anomalies->entries[i]);
    return result;
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertStereoParts(
    const CameraStereoParts& parts, const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile, GraphicsFrameContextSource& contextSource,
    const std::uint64_t frameSequence, std::optional<PylonRangeRejection>* rangeRejection,
    GraphicsFrameError* storageError, const std::atomic<bool>* cancelled,
    PylonFrameAnomalies* anomalies) const
{
    const bool ace = profile.family == PylonScene3DProfile::DeviceFamily::StereoAce;
    if (!ace && profile.family != PylonScene3DProfile::DeviceFamily::StereoMini) {
        if (storageError) *storageError = GraphicsFrameError::InvalidInput;
        return std::nullopt;
    }
    StereoReport report;
    report.first = rangeRejection; report.list = anomalies; report.sequence = frameSequence;
    StereoPlan plan;
    if (!planStereo(parts, request, profile, ace, report, plan)) {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        return std::nullopt;
    }
    // Nothing is published only when every selected part is empty.
    if (plan.bytes == 0U) { if (storageError) *storageError = GraphicsFrameError::InvalidInput; return std::nullopt; }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    auto context = contextSource.beginFrame(plan.bytes);
    if (!context) { if (storageError) *storageError = context.error(); return std::nullopt; }
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (storageError) *storageError = GraphicsFrameError::Disconnected;
        return std::nullopt;
    }
    return buildStereoFrame(parts, plan, profile, ace, context.value(), storageError, cancelled, frameSequence);
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
    const auto intMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    std::size_t imageWidth = part.width, imageHeight = part.height;
    const bool degenerate = !part.width || !part.height || part.width > intMax || part.height > intMax;
    if (degenerate) {
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidDimensions);
        if (part.bytes > intMax) {
            if (storageError) *storageError = GraphicsFrameError::Capacity;
            return std::nullopt;
        }
        imageWidth = part.bytes; imageHeight = 1U;
    }
    auto format = degenerate ? GraphicsImagePixelFormat::Mono8 : graphicsPixelFormat(part.pixelType);
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
    const auto rowBytes = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(imageWidth));
    if (!rowBytes || rowBytes > intMax || imageHeight > (std::numeric_limits<std::size_t>::max)() / rowBytes) {
        if (storageError) *storageError = GraphicsFrameError::Capacity;
        return std::nullopt;
    }
    std::size_t inputStride = part.hasStride ? part.stride : rowBytes;
    if (!part.hasStride && part.padding <= (std::numeric_limits<std::size_t>::max)() - rowBytes)
        inputStride += part.padding;
    if (degenerate || inputStride == 0U) inputStride = rowBytes;
    if (!degenerate && inputStride < rowBytes)
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::InvalidStride);
    else if (!degenerate && inputStride && imageHeight > part.bytes / inputStride)
        recordImageRejection(rejection, evidence, PylonImageRejection::Reason::TruncatedBytes);
    GraphicsImageWriterSource layout;
    layout.data = static_cast<const std::uint8_t*>(part.data); layout.size = rowBytes * imageHeight;
    layout.width = static_cast<int>(imageWidth); layout.height = static_cast<int>(imageHeight);
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
    for (std::size_t y = 0; y < imageHeight; ++y) {
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
