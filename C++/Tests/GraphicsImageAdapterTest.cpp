#include "PylonGraphicsFrameAdapter.h"

#include <pylon/PylonImage.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

int main()
{
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
    assert(image.data()[static_cast<std::size_t>(image.stride) + 1U] == 4U);

    const Pylon::CPylonImage empty;
    assert(!adapter.convertGraphicsImage(empty).isValid());
    return 0;
}
