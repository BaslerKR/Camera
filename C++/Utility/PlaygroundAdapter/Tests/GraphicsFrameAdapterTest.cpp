#include "PylonGraphicsFrameAdapter.h"

#include <pylon/PylonBase.h>
#include <pylon/PylonDataContainer.h>
#include <pylon/PylonImage.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>

int main()
{
    Pylon::PylonAutoInitTerm autoInit;

    Pylon::CPylonImage source = Pylon::CPylonImage::Create(
        Pylon::PixelType_Mono8,
        2U,
        2U);
    std::size_t stride = 0U;
    assert(source.GetStride(stride));
    auto* const bytes = static_cast<std::uint8_t*>(source.GetBuffer());
    bytes[0] = 1U;
    bytes[1] = 2U;
    bytes[stride] = 3U;
    bytes[stride + 1U] = 4U;

    PylonGraphicsFrameAdapter adapter;
    const GraphicsImage image = adapter.convertGraphicsImage(source, 42U);
    assert(image.isValid());
    assert(image.width == 2);
    assert(image.height == 2);
    assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8);
    assert(image.frameSequence == 42U);
    assert(image.data() != source.GetBuffer());
    assert(image.data()[0] == 1U);
    assert(image.data()[1] == 2U);
    assert(image.data()[static_cast<std::size_t>(image.stride)] == 3U);
    assert(image.data()[static_cast<std::size_t>(image.stride) + 1U] == 4U);

    // Provided storage receives the direct-copy bytes without an adapter allocation.
    std::uint8_t providedBytes[16] = {};
    std::size_t providedSize = 0U;
    adapter.setFrameStorage([&](const std::size_t requested) {
        providedSize = requested;
        assert(requested <= sizeof(providedBytes));
        return std::shared_ptr<std::uint8_t>(providedBytes, [](std::uint8_t*) {});
    });
    const GraphicsImage stored = adapter.convertGraphicsImage(source, 43U);
    assert(stored.isValid());
    assert(stored.data() == providedBytes);
    assert(providedSize == source.GetImageSize());
    assert(providedBytes[0] == 1U);
    assert(providedBytes[1] == 2U);
    assert(providedBytes[stride] == 3U);
    assert(providedBytes[stride + 1U] == 4U);

    // An empty storage result drops the frame instead of falling back to a copy.
    adapter.setFrameStorage([](std::size_t) { return std::shared_ptr<std::uint8_t>(); });
    assert(!adapter.convertGraphicsImage(source, 44U).isValid());
    adapter.setFrameStorage({});

    // 2D rejections carry the SDK input sequence and bounded layout values only.
    {
        std::optional<PylonImageRejection> rejection;
        assert(!adapter.convertGraphicsImage(Pylon::CPylonImage{}, 71U, &rejection).isValid());
        assert(rejection.has_value());
        assert(rejection->reason == PylonImageRejection::Reason::MissingBytes);
        assert(rejection->frameSequence == 71U);
        assert(rejection->conversion == PylonImageRejection::Conversion::None);

        adapter.setFrameStorage([](std::size_t) { return std::shared_ptr<std::uint8_t>(); });
        rejection.reset();
        assert(!adapter.convertGraphicsImage(source, 72U, &rejection).isValid());
        assert(rejection.has_value());
        assert(rejection->reason == PylonImageRejection::Reason::StorageRefused);
        assert(rejection->frameSequence == 72U);
        assert(rejection->width == 2U && rejection->height == 2U);
        assert(rejection->pixelFormat == static_cast<std::uint64_t>(Pylon::PixelType_Mono8));
        assert(rejection->minimumRowBytes == 2U);
        assert(rejection->readableBytes == source.GetImageSize());
        assert(rejection->requiredBytes == rejection->effectiveStride * 2U);
        adapter.setFrameStorage({});

        rejection.reset();
        const GraphicsImage retained = adapter.convertGraphicsImage(source, 73U, &rejection);
        assert(retained.isValid() && !rejection.has_value());
    }

    // The pylon converter path keeps its bytes and shifts Mono16 back by the source bit depth.
    Pylon::CPylonImage mono12 = Pylon::CPylonImage::Create(Pylon::PixelType_Mono12, 2U, 2U);
    std::size_t mono12Stride = 0U;
    assert(mono12.GetStride(mono12Stride));
    const std::uint16_t mono12Samples[4] = {1U, 0x456U, 0xABCU, 0xFFFU};
    auto* const mono12Bytes = static_cast<std::uint8_t*>(mono12.GetBuffer());
    std::memcpy(mono12Bytes, mono12Samples, 2U * sizeof(std::uint16_t));
    std::memcpy(mono12Bytes + mono12Stride, mono12Samples + 2, 2U * sizeof(std::uint16_t));
    const GraphicsImage shifted = adapter.convertGraphicsImage(mono12, 7U);
    assert(shifted.isValid());
    assert(shifted.pixelFormat == GraphicsImagePixelFormat::Mono16);
    assert(shifted.declaredMonoSourceBits == 12U);
    assert(shifted.frameSequence == 7U);
    std::uint16_t topLeft = 0U;
    std::uint16_t bottomLeft = 0U;
    std::memcpy(&topLeft, shifted.data(), sizeof(topLeft));
    std::memcpy(&bottomLeft, shifted.data() + shifted.stride, sizeof(bottomLeft));
    assert(topLeft == 1U);
    assert(bottomLeft == 0xABCU);

    // The pylon converter copy path keeps its BGR byte order.
    Pylon::CPylonImage planar = Pylon::CPylonImage::Create(Pylon::PixelType_RGB8planar, 2U, 2U);
    const std::uint8_t planarBytes[12] = {
        10U, 11U, 12U, 13U,
        20U, 21U, 22U, 23U,
        30U, 31U, 32U, 33U};
    std::memcpy(planar.GetBuffer(), planarBytes, sizeof(planarBytes));
    const GraphicsImage bgr = adapter.convertGraphicsImage(planar, 9U);
    assert(bgr.isValid());
    assert(bgr.width == 2);
    assert(bgr.height == 2);
    assert(bgr.pixelFormat == GraphicsImagePixelFormat::BGR24);
    assert(bgr.frameSequence == 9U);
    assert(bgr.data()[0] == 30U);
    assert(bgr.data()[1] == 20U);
    assert(bgr.data()[2] == 10U);
    assert(bgr.data()[3] == 31U);
    assert(bgr.data()[4] == 21U);
    assert(bgr.data()[5] == 11U);
    assert(bgr.data()[static_cast<std::size_t>(bgr.stride)] == 32U);
    assert(bgr.data()[static_cast<std::size_t>(bgr.stride) + 1U] == 22U);
    assert(bgr.data()[static_cast<std::size_t>(bgr.stride) + 2U] == 12U);

    const Pylon::CPylonImage empty;
    assert(!adapter.convertGraphicsImage(empty).isValid());

    Pylon::CPylonDataContainer emptyContainer;
    PylonScene3DProfile profile;
    profile.family = PylonScene3DProfile::DeviceFamily::Blaze;
    assert(!adapter.convertGraphicsFrame(emptyContainer, {}, profile).has_value());
    profile.family = PylonScene3DProfile::DeviceFamily::StereoMini;
    assert(!adapter.convertGraphicsFrame(emptyContainer, {}, profile).has_value());
    profile.family = PylonScene3DProfile::DeviceFamily::StereoAce;
    assert(!adapter.convertGraphicsFrame(emptyContainer, {}, profile).has_value());

    // Blaze conversion through the neutral source seam: the range keeps the
    // previous XYZ values, validity mask, auxiliary channels and cloud colours,
    // a provided storage function receives the range block, and no explicit
    // point cloud is produced for the same points.
    {
        const float coordinates[] = {
            1.0F, 2.0F, 3.0F,
            4.0F, 5.0F, 6.0F,
            7.0F, 8.0F, 9.0F,
            std::numeric_limits<float>::quiet_NaN(), 11.0F, 12.0F};
        const std::uint16_t intensitySamples[] = {100U, 200U, 300U, 400U};
        const std::uint16_t confidenceSamples[] = {7U, 8U, 9U, 10U};

        BlazeRangeSource source;
        source.coordinates = reinterpret_cast<const std::uint8_t*>(coordinates);
        source.coordinateStride = sizeof(float) * 3U * 2U;
        source.width = 2;
        source.height = 2;
        source.intensity.data = reinterpret_cast<const std::uint8_t*>(intensitySamples);
        source.intensity.stride = sizeof(std::uint16_t) * 2U;
        source.intensity.pixelType = Pylon::PixelType_Mono16;
        source.hasIntensity = true;
        source.confidence.data = reinterpret_cast<const std::uint8_t*>(confidenceSamples);
        source.confidence.stride = sizeof(std::uint16_t) * 2U;
        source.confidence.pixelType = Pylon::PixelType_Confidence16;
        source.hasConfidence = true;

        std::size_t storageCalls = 0U;
        std::shared_ptr<std::uint8_t> storageBlock;
        GraphicsImageStorage storage = [&](const std::size_t bytes) {
            ++storageCalls;
            storageBlock = std::shared_ptr<std::uint8_t>(
                new std::uint8_t[bytes], std::default_delete<std::uint8_t[]>());
            return storageBlock;
        };

        BlazeGraphicsFrameAdapter blaze;
        blaze.setFrameStorage(storage);
        const auto scene = blaze.convertSource(source, {});
        assert(scene.has_value());
        assert(storageCalls == 1U && storageBlock != nullptr);
        assert(scene->ranges.size() == 1U && scene->pointClouds.empty());

        const RangeFrame& range = scene->ranges.front().payload;
        assert(range.width == 2 && range.height == 2);
        assert(range.zValues.isBorrowed());
        assert(range.zValues.data() == reinterpret_cast<const float*>(storageBlock.get()));
        assert(range.xValues[0] == 1.0F && range.yValues[0] == 2.0F && range.zValues[0] == 3.0F);
        assert(range.xValues[1] == 4.0F && range.yValues[1] == 5.0F && range.zValues[1] == 6.0F);
        assert(range.xValues[2] == 7.0F && range.zValues[2] == 9.0F);
        assert(range.validMask.size() == 4U);
        assert(range.validMask[0] == 1U && range.validMask[1] == 1U
            && range.validMask[2] == 1U && range.validMask[3] == 0U);
        assert(range.intensity.size() == 4U && range.intensity[2] == 300.0F);
        assert(range.intensityBits == 16U);
        assert(range.confidence.size() == 4U && range.confidence[3] == 10.0F);
        assert(range.confidenceBits == 16U);
        assert(range.xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
        assert(range.zScale == 1.0);
        assert(range.canDerivePointCloud());
        // The range RGB channel keeps the explicit cloud's grayscale colours
        // and its row-major sample order.
        assert(range.rgb.size() == 12U);
        assert(range.rgb[0] == 0U && range.rgb[1] == 0U && range.rgb[2] == 0U);
        assert(range.rgb[3] == 0U && range.rgb[4] == 0U && range.rgb[5] == 0U);
        assert(range.rgb[6] == 1U && range.rgb[7] == 1U && range.rgb[8] == 1U);
        assert(range.rgb[9] == 1U && range.rgb[10] == 1U && range.rgb[11] == 1U);

        // A PointCloud-only request still emits the range that carries the
        // derived cloud, without unrelated auxiliary channels.
        GraphicsFrameRequest cloudOnlyRequest;
        cloudOnlyRequest.components = GraphicsFrameComponent::PointCloud;
        const auto cloudOnly = blaze.convertSource(source, cloudOnlyRequest);
        assert(cloudOnly.has_value() && cloudOnly->pointClouds.empty());
        assert(cloudOnly->ranges.size() == 1U);
        const RangeFrame& cloudOnlyRange = cloudOnly->ranges.front().payload;
        assert(cloudOnlyRange.canDerivePointCloud());
        assert(cloudOnlyRange.intensity.empty() && cloudOnlyRange.confidence.empty());
        assert(cloudOnlyRange.rgb.size() == 12U);

        // A Range-only request keeps no cloud colours.
        GraphicsFrameRequest rangeOnlyRequest;
        rangeOnlyRequest.components = GraphicsFrameComponent::Range;
        const auto rangeOnly = blaze.convertSource(source, rangeOnlyRequest);
        assert(rangeOnly.has_value() && rangeOnly->pointClouds.empty());
        assert(rangeOnly->ranges.size() == 1U);
        assert(rangeOnly->ranges.front().payload.rgb.empty());

        // Range evidence names the real failed rule, keeps the SDK sequence, and
        // stays empty when a valid range (and its intensity) is retained.
        std::optional<PylonRangeRejection> rejection;
        const auto retainedRange = blaze.convertSource(source, {}, 82U, &rejection);
        assert(retainedRange.has_value() && !rejection.has_value());

        BlazeRangeSource missing;
        missing.width = 2;
        missing.height = 2;
        rejection.reset();
        assert(!blaze.convertSource(missing, {}, 83U, &rejection).has_value());
        assert(rejection.has_value());
        assert(rejection->reason == PylonRangeRejection::Reason::MissingData);
        assert(rejection->frameSequence == 83U);
        assert(rejection->width == 2U && rejection->height == 2U);

        blaze.setFrameStorage([](const std::size_t) { return std::shared_ptr<std::uint8_t>(); });
        rejection.reset();
        assert(!blaze.convertSource(source, {}, 84U, &rejection).has_value());
        assert(rejection.has_value());
        assert(rejection->reason == PylonRangeRejection::Reason::StorageRefused);
        assert(rejection->frameSequence == 84U);
        assert(rejection->requestedStorageBytes > 0U);

        blaze.setFrameStorage({});
        auto shortStride = source;
        shortStride.coordinateStride = source.coordinateStride / 2U;
        rejection.reset();
        assert(!blaze.convertSource(shortStride, {}, 85U, &rejection).has_value());
        assert(rejection && rejection->reason == PylonRangeRejection::Reason::StrideBelowMinimum);
        assert(rejection->effectiveStride == shortStride.coordinateStride);
        assert(rejection->minimumRowBytes == source.coordinateStride);

        GraphicsFrameRequest unrequested;
        unrequested.components = GraphicsFrameComponent::Image;
        rejection.reset();
        assert(!blaze.convertSource(source, unrequested, 86U, &rejection).has_value());
        assert(!rejection.has_value());
    }
    return 0;
}
