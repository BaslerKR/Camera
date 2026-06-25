#include "PylonScene3DAdapter.h"

#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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
        return 3U;
    case Pylon::PixelType_RGBA8packed:
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

[[nodiscard]] QImage componentToImage(const Pylon::CPylonDataComponent& component)
{
    if (!component.IsValid() || component.GetData() == nullptr)
    {
        return {};
    }

    std::size_t stride = 0U;
    if (!componentStride(component, stride))
    {
        return {};
    }

    if (component.GetPixelType() == Pylon::PixelType_RGBA8packed)
    {
        // Stereo mini samples use RGB only; its A byte is not display opacity.
        const int width = static_cast<int>(component.GetWidth());
        const int height = static_cast<int>(component.GetHeight());
        const auto* data = static_cast<const std::uint8_t*>(component.GetData());
        QImage image(width, height, QImage::Format_RGB888);
        for (int y = 0; y < height; ++y)
        {
            const auto* source = data + static_cast<std::size_t>(y) * stride;
            auto* target = image.scanLine(y);
            for (int x = 0; x < width; ++x)
            {
                const std::size_t sourceOffset = static_cast<std::size_t>(x) * 4U;
                const std::size_t targetOffset = static_cast<std::size_t>(x) * 3U;
                target[targetOffset] = source[sourceOffset];
                target[targetOffset + 1U] = source[sourceOffset + 1U];
                target[targetOffset + 2U] = source[sourceOffset + 2U];
            }
        }
        return image;
    }

    QImage::Format format = QImage::Format_Invalid;
    switch (component.GetPixelType())
    {
    case Pylon::PixelType_Mono8:
        format = QImage::Format_Grayscale8;
        break;
    case Pylon::PixelType_Mono16:
        format = QImage::Format_Grayscale16;
        break;
    case Pylon::PixelType_RGB8packed:
        format = QImage::Format_RGB888;
        break;
    default:
        return {};
    }

    const auto* data = static_cast<const uchar*>(component.GetData());
    return QImage(data,
                  static_cast<int>(component.GetWidth()),
                  static_cast<int>(component.GetHeight()),
                  static_cast<int>(stride),
                  format).copy();
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
                      const GraphicsScene3DRequest& request,
                      const PylonScene3DProfile& profile,
                      GraphicsScene3D& scene)
{
    if (!hasScene3DContent(request.content, GraphicsScene3DContent::ColorImage))
    {
        return;
    }

    QImage image = componentToImage(intensity);
    if (image.isNull())
    {
        return;
    }

    scene.content = scene.content | GraphicsScene3DContent::ColorImage;
    scene.colorImage = std::move(image);
    scene.meta.colorRegistration = profile.colorRegisteredToRange
        ? GraphicsImageRegistration::RegisteredToRange
        : GraphicsImageRegistration::Unregistered;
}

[[nodiscard]] std::optional<GraphicsScene3D> buildDirectXyzScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsScene3DRequest& request,
    const PylonScene3DProfile& profile,
    const char* sourceName)
{
    GraphicsScene3D scene;
    scene.content = GraphicsScene3DContent::None;
    scene.meta.sourceName = sourceName;
    InitialView3D view;
    view.lookDirection = {0.0, 0.0, 1.0};
    view.viewUp = {0.0, -1.0, 0.0};
    view.parallelProjection = false;
    view.distanceScale = 1.15;
    scene.surfaceInitialView = view;
    scene.pointCloudInitialView = view;

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene);
    if (!hasScene3DContent(request.content, GraphicsScene3DContent::RangeFrame))
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    const auto range = componentByType(container, Pylon::ComponentType_Range);
    if (!range.IsValid() || range.GetData() == nullptr)
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    const auto pixelType = range.GetPixelType();
    if (pixelType != Pylon::PixelType_Coord3D_ABC32f && pixelType != Pylon::PixelType_Coord3D_C16)
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    std::size_t stride = 0U;
    if (!componentStride(range, stride))
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    RangeFrame frame;
    frame.width = static_cast<int>(range.GetWidth());
    frame.height = static_cast<int>(range.GetHeight());
    frame.sensorType = sourceName;
    const std::size_t count = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    frame.xValues.resize(count);
    frame.yValues.resize(count);
    frame.zValues.resize(count);
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

                if (raw == 0U || z <= 0.0 || profile.focalLength == 0.0)
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
    }
    if (request.includePointCloudColors)
    {
        copyPointCloudRgb(intensity, frame);
    }

    if (frame.isValid())
    {
        scene.content = scene.content | GraphicsScene3DContent::RangeFrame;
        scene.rangeFrame = std::move(frame);
    }
    return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
}

[[nodiscard]] std::optional<GraphicsScene3D> buildStereoAceScene(
    const Pylon::CPylonDataContainer& container,
    const GraphicsScene3DRequest& request,
    const PylonScene3DProfile& profile)
{
    GraphicsScene3D scene;
    scene.content = GraphicsScene3DContent::None;
    scene.meta.sourceName = "Basler Stereo ace";

    const auto intensity = componentByType(container, Pylon::ComponentType_Intensity);
    appendColorImage(intensity, request, profile, scene);
    if (!hasScene3DContent(request.content, GraphicsScene3DContent::RangeFrame)
        || !profile.hasDisparityCalibration())
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    const auto disparity = componentByType(container, Pylon::ComponentType_Disparity);
    if (!disparity.IsValid() || disparity.GetPixelType() != Pylon::PixelType_Coord3D_C16
        || disparity.GetData() == nullptr)
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    std::size_t stride = 0U;
    if (!componentStride(disparity, stride))
    {
        return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
    }

    RangeFrame frame;
    frame.width = static_cast<int>(disparity.GetWidth());
    frame.height = static_cast<int>(disparity.GetHeight());
    frame.sensorType = "Basler Stereo ace";
    const std::size_t count = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    frame.xValues.resize(count);
    frame.yValues.resize(count);
    frame.zValues.resize(count);
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
    }
    if (request.includePointCloudColors)
    {
        copyPointCloudRgb(intensity, frame);
    }

    if (frame.isValid())
    {
        scene.content = scene.content | GraphicsScene3DContent::RangeFrame;
        scene.rangeFrame = std::move(frame);
    }
    return scene.content == GraphicsScene3DContent::None ? std::nullopt : std::optional<GraphicsScene3D>(std::move(scene));
}

} // namespace

std::optional<GraphicsScene3D> PylonScene3DAdapter::convert(
    const Pylon::CPylonDataContainer& container,
    const GraphicsScene3DRequest& request,
    const PylonScene3DProfile& profile) const
{
    switch (profile.family)
    {
    case PylonScene3DProfile::DeviceFamily::Blaze:
        return _blazeAdapter.convert(container, request);
    case PylonScene3DProfile::DeviceFamily::StereoMini:
        return buildDirectXyzScene(container, request, profile, "Basler Stereo mini");
    case PylonScene3DProfile::DeviceFamily::StereoAce:
        return buildStereoAceScene(container, request, profile);
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }

    return std::nullopt;
}
