#include "PylonGraphicsFrameAdapter.h"
#include "CameraSystem.h"

#include <pylon/ImageFormatConverter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

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

[[nodiscard]] GraphicsImage makeOwnedImage(
    const int width,
    const int height,
    const int stride,
    const int bitsPerPixel,
    const GraphicsImagePixelFormat pixelFormat,
    const int sourceFormat,
    const void* source,
    const std::size_t size,
    const std::uint8_t declaredMonoSourceBits = 0U,
    const std::uint64_t frameSequence = 0U)
{
    GraphicsImage image;
    if (source == nullptr || width <= 0 || height <= 0 || stride <= 0 || size == 0U)
    {
        return image;
    }

    auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
    std::memcpy(bytes->data(), source, size);
    image.storage = std::shared_ptr<const std::uint8_t>(bytes, bytes->data());
    image.size = size;
    image.width = width;
    image.height = height;
    image.stride = stride;
    image.bitsPerPixel = bitsPerPixel;
    image.declaredMonoSourceBits = declaredMonoSourceBits;
    image.sourceFormat = sourceFormat;
    image.pixelFormat = pixelFormat;
    image.frameSequence = frameSequence;
    return image.isValid() ? image : GraphicsImage{};
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
    const std::uint64_t frameSequence = 0U)
{
    if (!component.IsValid() || component.GetData() == nullptr)
    {
        return {};
    }

    const GraphicsImagePixelFormat pixelFormat = graphicsPixelFormat(component.GetPixelType());
    const int bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(pixelFormat);
    if (pixelFormat == GraphicsImagePixelFormat::Unknown || bitsPerPixel == 0)
    {
        return {};
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride)
        || stride > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || component.GetWidth() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || component.GetHeight() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return {};
    }

    return makeOwnedImage(
        static_cast<int>(component.GetWidth()),
        static_cast<int>(component.GetHeight()),
        static_cast<int>(stride),
        bitsPerPixel,
        pixelFormat,
        static_cast<int>(component.GetPixelType()),
        component.GetData(),
        component.GetDataSize(),
        0U,
        frameSequence);
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
    const std::uint64_t frameSequence)
{
    Pylon::CPylonImage converted;
    Pylon::CImageFormatConverter converter;
    converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;
    converter.OutputPixelFormat = outputPixelType;
    converter.Convert(converted, pylonImage);

    int stride = 0;
    if (!pylonStride(converted, stride))
    {
        return {};
    }
    GraphicsImage image = makeOwnedImage(
        static_cast<int>(converted.GetWidth()),
        static_cast<int>(converted.GetHeight()),
        stride,
        bitsPerPixel,
        pixelFormat,
        static_cast<int>(pylonImage.GetPixelType()),
        converted.GetBuffer(),
        converted.GetImageSize(),
        declaredMonoSourceBits,
        frameSequence);

    if (image.isValid()
        && outputPixelType == Pylon::PixelType_Mono16
        && declaredMonoSourceBits > 0U
        && declaredMonoSourceBits < 16U)
    {
        const int storageShift = 16 - static_cast<int>(declaredMonoSourceBits);
        auto* samples = reinterpret_cast<std::uint16_t*>(
            const_cast<std::uint8_t*>(image.storage.get()));
        const std::size_t sampleCount = image.size / sizeof(std::uint16_t);
        for (std::size_t index = 0; index < sampleCount; ++index)
        {
            samples[index] = static_cast<std::uint16_t>(samples[index] >> storageShift);
        }
    }

    return image;
}

[[nodiscard]] GraphicsImage pylonImageToGraphicsImage(
    const Pylon::CPylonImage& pylonImage,
    const std::uint64_t frameSequence)
{
    if (pylonImage.GetWidth() == 0
        || pylonImage.GetHeight() == 0
        || pylonImage.GetBuffer() == nullptr)
    {
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
            return makeOwnedImage(
                static_cast<int>(pylonImage.GetWidth()),
                static_cast<int>(pylonImage.GetHeight()),
                stride,
                bitsPerPixel,
                pixelFormat,
                static_cast<int>(pixelType),
                pylonImage.GetBuffer(),
                pylonImage.GetImageSize(),
                0U,
                frameSequence);
        }
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
            frameSequence);
    }

    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_BGR8packed,
            GraphicsImagePixelFormat::BGR24,
            24,
            0U,
            frameSequence);
    } catch (const Pylon::GenericException&) {
    }
    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_Mono16,
            GraphicsImagePixelFormat::Mono16,
            16,
            0U,
            frameSequence);
    } catch (const Pylon::GenericException&) {
    }
    try {
        return convertViaPylon(
            pylonImage,
            Pylon::PixelType_Mono8,
            GraphicsImagePixelFormat::Mono8,
            8,
            0U,
            frameSequence);
    } catch (const Pylon::GenericException&) {
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
                      GraphicsFrame& scene)
{
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Image))
    {
        return;
    }

    GraphicsImage image = componentToGraphicsImage(intensity);
    if (!image.isValid())
    {
        return;
    }

    scene.image = std::move(image);
    scene.metadata.colorRegistration = profile.colorRegisteredToRange
        ? GraphicsImageRegistration::RegisteredToRange
        : GraphicsImageRegistration::Unregistered;
}

[[nodiscard]] std::optional<GraphicsFrame> buildDirectXyzScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile,
    const char* sourceName)
{
    GraphicsFrame scene;
    scene.metadata.sourceName = sourceName;
    InitialView3D view;
    view.lookDirection = {0.0, 0.0, 1.0};
    view.viewUp = {0.0, -1.0, 0.0};
    view.parallelProjection = false;
    view.distanceScale = 1.15;
    scene.surfaceInitialView = view;
    scene.pointCloudInitialView = view;

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range))
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto range = componentByType(container, Pylon::ComponentType_Range);
    if (!range.IsValid() || range.GetData() == nullptr)
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto pixelType = range.GetPixelType();
    if (pixelType != Pylon::PixelType_Coord3D_ABC32f && pixelType != Pylon::PixelType_Coord3D_C16)
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    std::size_t stride = 0U;
    if (!componentStride(range, stride))
    {
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
        scene.rangeFrame = std::move(frame);
    }
    return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
}

[[nodiscard]] std::optional<GraphicsFrame> buildStereoAceScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile)
{
    GraphicsFrame scene;
    scene.metadata.sourceName = "Basler Stereo ace";

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene);
    if (!hasGraphicsFrameComponent(request.components, GraphicsFrameComponent::Range)
        || !profile.hasDisparityCalibration())
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    const auto disparity = componentByType(container, Pylon::ComponentType_Disparity);
    if (!disparity.IsValid() || disparity.GetPixelType() != Pylon::PixelType_Coord3D_C16
        || disparity.GetData() == nullptr)
    {
        return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
    }

    std::size_t stride = 0U;
    if (!componentStride(disparity, stride))
    {
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
        scene.rangeFrame = std::move(frame);
    }
    return scene.isValid() ? std::optional<GraphicsFrame>(std::move(scene)) : std::nullopt;
}

} // namespace

GraphicsImage PylonGraphicsFrameAdapter::convertGraphicsImage(
    const Pylon::CPylonImage& image,
    const std::uint64_t frameSequence) const
{
    return pylonImageToGraphicsImage(image, frameSequence);
}

namespace {

[[nodiscard]] GraphicsFrameRequest cameraGraphicsFrameRequest() noexcept
{
    GraphicsFrameRequest request;
    request.components = GraphicsFrameComponent::Range
        | GraphicsFrameComponent::PointCloud
        | GraphicsFrameComponent::Image;
    request.includeRangeAuxiliaryChannels = true;
    request.includePointCloudColors = true;
    return request;
}

class CameraReadyGuard final
{
public:
    explicit CameraReadyGuard(Camera* camera) noexcept : _camera(camera) {}
    ~CameraReadyGuard() { if (_camera) _camera->ready(); }

private:
    Camera* _camera;
};

} // namespace

PylonGraphicsFrameStream::PylonGraphicsFrameStream(
    Camera* camera,
    GraphicsFrameCallback callback)
    : _camera(camera), _callback(std::move(callback))
{
    if (!_camera || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _camera->registerGrabCallback(
        [this, callbackToken](const Pylon::CPylonImage& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            CameraReadyGuard ready(_camera);
            try
            {
                GraphicsImage graphicsImage = _adapter.convertGraphicsImage(image, sequence);
                if (!graphicsImage.isValid()) return;

                GraphicsFrame frame;
                frame.setImage(std::move(graphicsImage));
                _callback(std::move(frame), 0U);
            }
            catch (const std::exception& error)
            {
                CameraSystem::syslog(
                    std::string("Camera GraphicsFrame callback failed: ") + error.what(), true);
            }
            catch (...)
            {
                CameraSystem::syslog("Camera GraphicsFrame callback failed with an unknown exception.", true);
            }
        });

    _grab3DCallbackId = _camera->registerGrab3DCallback(
        [this, callbackToken](const Pylon::CPylonDataContainer& container, const std::size_t) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            CameraReadyGuard ready(_camera);
            try
            {
                auto frame = _adapter.convertGraphicsFrame(
                    container, cameraGraphicsFrameRequest(), _camera->scene3DProfile());
                if (frame.has_value()) _callback(std::move(*frame), 0U);
            }
            catch (const std::exception& error)
            {
                CameraSystem::syslog(
                    std::string("Camera GraphicsFrame 3D callback failed: ") + error.what(), true);
            }
            catch (...)
            {
                CameraSystem::syslog("Camera GraphicsFrame 3D callback failed with an unknown exception.", true);
            }
        });
}

PylonGraphicsFrameStream::~PylonGraphicsFrameStream()
{
    _callbackGate.beginShutdown();
    if (!_camera) return;
    if (_grabCallbackId != 0U)
    {
        _camera->deregisterGrabCallback(_grabCallbackId);
    }
    if (_grab3DCallbackId != 0U)
    {
        _camera->deregisterGrab3DCallback(_grab3DCallbackId);
    }
    _callbackGate.waitForDrain();
}

std::optional<GraphicsFrame> PylonGraphicsFrameAdapter::convertGraphicsFrame(
    const Pylon::CPylonDataContainer& container,
    const GraphicsFrameRequest& request,
    const PylonScene3DProfile& profile) const
{
    switch (profile.family)
    {
    case PylonScene3DProfile::DeviceFamily::Blaze:
        return _blazeAdapter.convertFrame(container, request);
    case PylonScene3DProfile::DeviceFamily::StereoMini:
        return buildDirectXyzScene(container, request, profile, "Basler Stereo mini");
    case PylonScene3DProfile::DeviceFamily::StereoAce:
        return buildStereoAceScene(container, request, profile);
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }

    return std::nullopt;
}
