#include "PylonGraphicsFrameAdapter.h"

#include <pylon/PylonBase.h>
#include <pylon/PylonDataContainer.h>
#include <pylon/PylonImage.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

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
    return 0;
}
