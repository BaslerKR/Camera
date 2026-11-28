#ifdef NDEBUG
#undef NDEBUG
#endif
#include "PylonGraphicsFrameAdapter.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"

#include <pylon/PylonBase.h>
#include <pylon/PylonDataContainer.h>
#include <pylon/PylonImage.h>

#include <cassert>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

namespace {

std::atomic<bool> gCountNew{false};
std::atomic<std::size_t> gNewCount{0};

void* allocUnaligned(const std::size_t size) noexcept
{
    if (gCountNew.load(std::memory_order_relaxed))
    {
        gNewCount.fetch_add(1U, std::memory_order_relaxed);
    }
    return std::malloc(size == 0U ? 1U : size);
}

void* allocAligned(const std::size_t size, const std::align_val_t alignment) noexcept
{
    if (gCountNew.load(std::memory_order_relaxed))
    {
        gNewCount.fetch_add(1U, std::memory_order_relaxed);
    }
    const std::size_t alignVal = static_cast<std::size_t>(alignment);
#ifdef _WIN32
    return _aligned_malloc(size == 0U ? 1U : size, alignVal < sizeof(void*) ? sizeof(void*) : alignVal);
#else
    const std::size_t effectiveAlign = alignVal < sizeof(void*) ? sizeof(void*) : alignVal;
    const std::size_t requested = size == 0U ? 1U : size;
    const std::size_t padded = ((requested + effectiveAlign - 1U) / effectiveAlign) * effectiveAlign;
    void* ptr = nullptr;
    if (posix_memalign(&ptr, effectiveAlign, padded) != 0) return nullptr;
    return ptr;
#endif
}

void freeAligned(void* ptr) noexcept
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace

void* operator new(const std::size_t size)
{
    if (void* ptr = allocUnaligned(size)) return ptr;
    throw std::bad_alloc();
}

void* operator new[](const std::size_t size)
{
    return ::operator new(size);
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept
{
    return allocUnaligned(size);
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept
{
    return allocUnaligned(size);
}

void* operator new(const std::size_t size, const std::align_val_t alignment)
{
    if (void* ptr = allocAligned(size, alignment)) return ptr;
    throw std::bad_alloc();
}

void* operator new[](const std::size_t size, const std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocAligned(size, alignment);
}

void* operator new[](const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocAligned(size, alignment);
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { freeAligned(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(ptr); }

class TestAllocation final : public GraphicsAllocation
{
public:
    TestAllocation(std::shared_ptr<std::uint8_t> data, std::size_t bytes)
        : GraphicsAllocation(reinterpret_cast<std::byte*>(data.get()), bytes,
                             GraphicsAllocationOrigin::Managed, 0U, GraphicsAllocationState::Writable),
          _data(std::move(data)) {}
private:
    void destroy() noexcept override { delete this; }
    std::shared_ptr<std::uint8_t> _data;
};

class TestStorageProvider final : public GraphicsFrameAllocator
{
public:
    explicit TestStorageProvider(std::function<std::shared_ptr<std::uint8_t>(std::size_t)> acquire)
        : _acquire(std::move(acquire)) {}
    GraphicsResult<GraphicsAllocationSlice> allocate(std::size_t bytes, std::size_t alignment) noexcept override
    {
        auto data = _acquire(bytes);
        if (!data || reinterpret_cast<std::uintptr_t>(data.get()) % alignment != 0U)
            return GraphicsFrameError::Allocation;
        auto* allocation = new (std::nothrow) TestAllocation(std::move(data), bytes);
        if (!allocation) return GraphicsFrameError::Allocation;
        return GraphicsAllocationSlice{GraphicsAllocationRef::adoptNew(allocation), 0U, bytes};
    }
private:
    std::function<std::shared_ptr<std::uint8_t>(std::size_t)> _acquire;
};

static GraphicsFrameContext testContext(
    std::function<std::shared_ptr<std::uint8_t>(std::size_t)> acquire)
{
    return GraphicsFrameContext(std::make_shared<TestStorageProvider>(std::move(acquire)));
}

// Isolate producer and content work from the Local test allocator's backing allocation.
class UncountedLocalBacking final : public GraphicsFrameAllocator
{
public:
    GraphicsResult<GraphicsAllocationSlice> allocate(std::size_t bytes, std::size_t alignment) noexcept override {
        const bool counting = gCountNew.exchange(false, std::memory_order_acq_rel);
        auto result = GraphicsFrameContext::local().allocate(bytes, alignment);
        gCountNew.store(counting, std::memory_order_release);
        return result;
    }
};

class TrackedContextSource final : public GraphicsFrameContextSource
{
public:
    explicit TrackedContextSource(GraphicsFrameContext context) : context(std::move(context)) {}
    GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t bytes) noexcept override
    {
        ++calls;
        requestedBytes = bytes;
        if (error != GraphicsFrameError::None) return error;
        return context;
    }
    GraphicsFrameContext context;
    GraphicsFrameError error = GraphicsFrameError::None;
    std::size_t calls = 0;
    std::size_t requestedBytes = 0;
};

class BudgetAllocator final : public GraphicsFrameAllocator
{
public:
    std::size_t limit = 0U, used = 0U, reserves = 0U;
    GraphicsResult<GraphicsAllocationSlice> allocate(std::size_t bytes, std::size_t alignment) noexcept override {
        std::size_t next = used;
        if (!appendAligned(next, bytes, alignment) || next > limit) return GraphicsFrameError::Capacity;
        auto result = GraphicsFrameContext::local().allocate(bytes, alignment);
        if (result) { used = next; ++reserves; }
        return result;
    }
};
class BudgetSource final : public GraphicsFrameContextSource
{
public:
    std::shared_ptr<BudgetAllocator> allocator = std::make_shared<BudgetAllocator>();
    std::size_t calls = 0U, requested = 0U, shortage = 0U;
    GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t bytes) noexcept override {
        ++calls; requested = bytes; allocator->used = 0U; allocator->reserves = 0U;
        allocator->limit = bytes > shortage ? bytes - shortage : 0U;
        return GraphicsFrameContext(allocator);
    }
};

static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<GraphicsSpan<std::uint8_t>>().data())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<GraphicsFrame>().ranges().front())>>);

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
    GraphicsFrameBuilder imageBuilder;
    const GraphicsImage image = adapter.convertGraphicsImage(source, imageBuilder, 42U);
    imageBuilder.addImage("image", image, "Image");
    auto publishedImage = std::move(imageBuilder).publish();
    assert(publishedImage);
    assert(image.pixels.allocation()->state() == GraphicsAllocationState::Published);
    assert(image.isValid());
    assert(image.width == 2);
    assert(image.height == 2);
    assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8);
    assert(image.frameSequence == 42U);
    assert(image.pixels.size() == source.GetImageSize());
    assert(image.data() != source.GetBuffer());
    assert(image.data()[0] == 1U);
    assert(image.data()[1] == 2U);
    assert(image.data()[static_cast<std::size_t>(image.stride)] == 3U);
    assert(image.data()[static_cast<std::size_t>(image.stride) + 1U] == 4U);

    // Provided storage receives the direct-copy bytes without an adapter allocation.
    std::uint8_t providedBytes[16] = {};
    std::size_t providedSize = 0U;
    auto providedContext = testContext([&](const std::size_t requested) {
        providedSize = requested;
        assert(requested <= sizeof(providedBytes));
        return std::shared_ptr<std::uint8_t>(providedBytes, [](std::uint8_t*) {});
    });
    GraphicsFrameBuilder storedBuilder(providedContext);
    const GraphicsImage stored = adapter.convertGraphicsImage(source, storedBuilder, 43U);
    assert(stored.isValid());
    assert(stored.data() == providedBytes);
    assert(providedSize == source.GetImageSize());
    assert(providedBytes[0] == 1U);
    assert(providedBytes[1] == 2U);
    assert(providedBytes[stride] == 3U);
    assert(providedBytes[stride + 1U] == 4U);

    // An empty storage result drops the frame instead of falling back to a copy.
    auto refusedContext = testContext([](std::size_t) { return std::shared_ptr<std::uint8_t>(); });
    GraphicsFrameBuilder refusedBuilder(refusedContext);
    assert(!adapter.convertGraphicsImage(source, refusedBuilder, 44U).isValid());

    // 2D rejections carry the SDK input sequence and bounded layout values only.
    {
        GraphicsFrameBuilder rejectionBuilder;
        std::optional<PylonImageRejection> rejection;
        assert(!adapter.convertGraphicsImage(Pylon::CPylonImage{}, rejectionBuilder, 71U, &rejection).isValid());
        assert(rejection.has_value());
        assert(rejection->reason == PylonImageRejection::Reason::MissingBytes);
        assert(rejection->frameSequence == 71U);
        assert(rejection->conversion == PylonImageRejection::Conversion::None);

        GraphicsFrameBuilder refusedRejectionBuilder(refusedContext);
        rejection.reset();
        assert(!adapter.convertGraphicsImage(source, refusedRejectionBuilder, 72U, &rejection).isValid());
        assert(rejection.has_value());
        assert(rejection->reason == PylonImageRejection::Reason::StorageRefused);
        assert(rejection->frameSequence == 72U);
        assert(rejection->width == 2U && rejection->height == 2U);
        assert(rejection->pixelFormat == static_cast<std::uint64_t>(Pylon::PixelType_Mono8));
        assert(rejection->minimumRowBytes == 2U);
        assert(rejection->readableBytes == source.GetImageSize());
        assert(rejection->requiredBytes == rejection->effectiveStride * 2U);

        rejection.reset();
        const GraphicsImage retained = adapter.convertGraphicsImage(source, rejectionBuilder, 73U, &rejection);
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
    GraphicsFrameBuilder shiftedBuilder;
    const GraphicsImage shifted = adapter.convertGraphicsImage(mono12, shiftedBuilder, 7U);
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
    GraphicsFrameBuilder bgrBuilder;
    const GraphicsImage bgr = adapter.convertGraphicsImage(planar, bgrBuilder, 9U);
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

    // Whole-frame admission happens once before the SDK conversion path.
    {
        TrackedContextSource tracked(GraphicsFrameContext::local());
        GraphicsFrameError error = GraphicsFrameError::None;
        auto converted = adapter.convertGraphicsImageFrame(planar, tracked, 90U, nullptr, &error);
        assert(converted && converted->hasImage());
        assert(tracked.calls == 1U && tracked.requestedBytes == 12U);
        assert(converted->images().front().payload.data()[0] == 30U);
        tracked.error = GraphicsFrameError::NoReadyCredit;
        error = GraphicsFrameError::None;
        assert(!adapter.convertGraphicsImageFrame(planar, tracked, 91U, nullptr, &error));
        assert(error == GraphicsFrameError::NoReadyCredit && tracked.calls == 2U);
        tracked.error = GraphicsFrameError::Disconnected;
        error = GraphicsFrameError::None;
        assert(!adapter.convertGraphicsImageFrame(planar, tracked, 92U, nullptr, &error));
        assert(error == GraphicsFrameError::Disconnected && tracked.calls == 3U);
        std::atomic<bool> cancelled{true};
        error = GraphicsFrameError::None;
        assert(!adapter.convertGraphicsImageFrame(planar, tracked, 93U, nullptr, &error, &cancelled));
        assert(error == GraphicsFrameError::Disconnected && tracked.calls == 3U);
    }

    // Cancellation during reserve abandons the open transaction and its backing.
    {
        std::atomic<bool> cancelled{false};
        std::weak_ptr<std::uint8_t> released;
        auto context = testContext([&](std::size_t size) {
            auto memory = std::shared_ptr<std::uint8_t>(new std::uint8_t[size], std::default_delete<std::uint8_t[]>());
            released = memory;
            cancelled.store(true, std::memory_order_release);
            return memory;
        });
        TrackedContextSource tracked(context);
        GraphicsFrameError error = GraphicsFrameError::None;
        auto frame = adapter.convertGraphicsImageFrame(source, tracked, 94U, nullptr, &error, &cancelled);
        assert(!frame && error == GraphicsFrameError::Disconnected);
        assert(tracked.calls == 1U && released.expired());
    }

    // Count C++ allocations over warmed-up frames, including GE Local context work.
    {
        auto allocator = std::make_shared<UncountedLocalBacking>();
        auto pool = GraphicsFrameContentPool::create(8U);
        TrackedContextSource tracked(GraphicsFrameContext(allocator, pool));
        for (int i = 0; i < 10; ++i) {
            auto frame = adapter.convertGraphicsImageFrame(source, tracked, i);
            assert(frame && frame->hasImage());
        }
        gNewCount.store(0U, std::memory_order_relaxed);
        gCountNew.store(true, std::memory_order_release);
        for (int i = 0; i < 100; ++i) {
            auto frame = adapter.convertGraphicsImageFrame(source, tracked, i);
            assert(frame && frame->hasImage());
        }
        gCountNew.store(false, std::memory_order_release);
        std::printf("Camera 2D steady-state operator-new calls excluding backing allocator: %zu/100 frames\n",
                    gNewCount.load(std::memory_order_acquire));
        assert(gNewCount.load(std::memory_order_acquire) == 0U);
    }

    // Malformed but readable 2D buffers retain their samples and receive diagnostics.
    {
        std::uint8_t pixels[] = {1, 2, 3, 4, 5, 6};
        CameraStereoPartView part;
        part.valid = true; part.data = pixels; part.width = 2; part.height = 2;
        part.pixelType = Pylon::PixelType_Mono8; part.hasStride = true;
        TrackedContextSource tracked(GraphicsFrameContext::local());
        std::size_t diagnosticCount = 0U;
        auto verify = [&](std::size_t bytes, std::size_t stride, Pylon::EPixelType type,
                          const std::uint8_t (&expected)[4], std::optional<PylonImageRejection::Reason> reason) {
            part.bytes = bytes; part.stride = stride; part.pixelType = type;
            std::optional<PylonImageRejection> diagnostic;
            GraphicsFrameError error = GraphicsFrameError::None;
            auto frame = adapter.convertImageViewFrame(part, tracked, 100U, &diagnostic, &error);
            assert(frame && frame->hasImage() && error == GraphicsFrameError::None);
            const auto& out = frame->images().front().payload;
            assert(out.width == 2 && out.height == 2 && out.stride == 2);
            assert(std::memcmp(out.data(), expected, 4) == 0);
            assert(diagnostic.has_value() == reason.has_value());
            if (reason) { assert(diagnostic->reason == *reason); ++diagnosticCount; }
        };
        const std::uint8_t shortExpected[] = {1,2,3,0};
        verify(3, 2, Pylon::PixelType_Mono8, shortExpected, PylonImageRejection::Reason::TruncatedBytes);
        const std::uint8_t narrowExpected[] = {1,0,2,0};
        verify(2, 1, Pylon::PixelType_Mono8, narrowExpected, PylonImageRejection::Reason::InvalidStride);
        const std::uint8_t paddedExpected[] = {1,2,4,5};
        verify(6, 3, Pylon::PixelType_Mono8, paddedExpected, std::nullopt);
        const std::uint8_t unknownExpected[] = {1,2,3,4};
        verify(4, 2, Pylon::PixelType_BayerGR8, unknownExpected, PylonImageRejection::Reason::UnsupportedFormat);
        part.bytes = 0;
        std::optional<PylonImageRejection> missing;
        assert(!adapter.convertImageViewFrame(part, tracked, 101U, &missing));
        assert(missing && missing->reason == PylonImageRejection::Reason::MissingBytes);
        ++diagnosticCount;
        assert(diagnosticCount == 4U);
        // Unusable declared extents publish the held bytes as one raw Mono8 row, with a diagnostic.
        part.width = 0; part.height = 0; part.bytes = 4; part.hasStride = false; part.stride = 0;
        part.pixelType = Pylon::PixelType_Mono8;
        std::optional<PylonImageRejection> degenerate;
        GraphicsFrameError degenerateError = GraphicsFrameError::None;
        auto raw = adapter.convertImageViewFrame(part, tracked, 102U, &degenerate, &degenerateError);
        assert(raw && raw->hasImage() && degenerateError == GraphicsFrameError::None);
        assert(raw->images().front().payload.width == 4 && raw->images().front().payload.height == 1);
        assert(raw->images().front().payload.data()[3] == 4U);
        assert(degenerate && degenerate->reason == PylonImageRejection::Reason::InvalidDimensions);
        // A zero SDK stride reads packed rows instead of dropping the part.
        part.width = 2; part.height = 2; part.hasStride = true; part.stride = 0; part.bytes = 4;
        auto zeroStride = adapter.convertImageViewFrame(part, tracked, 103U);
        assert(zeroStride && zeroStride->images().front().payload.data()[3] == 4U);
    }

    // Every published span lives in a region reserved from the frame context: overwriting the SDK
    // buffer after conversion leaves the published bytes intact, on the view and the image paths.
    {
        std::uint8_t sdk[4] = {1, 2, 3, 4};
        CameraStereoPartView view;
        view.valid = true; view.data = sdk; view.bytes = sizeof(sdk); view.pixelType = Pylon::PixelType_Mono8;
        TrackedContextSource owned(GraphicsFrameContext::local());
        auto fromView = adapter.convertImageViewFrame(view, owned, 120U);   // zero extents: raw row
        auto own = Pylon::CPylonImage::Create(Pylon::PixelType_Mono8, 2U, 2U);
        std::size_t ownStride = 0U;
        assert(own.GetStride(ownStride));
        auto* ownBytes = static_cast<std::uint8_t*>(own.GetBuffer());
        ownBytes[0] = 1U; ownBytes[1] = 2U; ownBytes[ownStride] = 3U; ownBytes[ownStride + 1U] = 4U;
        auto fromImage = adapter.convertGraphicsImageFrame(own, owned, 121U);
        assert(fromView && fromImage);
        const auto* viewPixels = fromView->images().front().payload.data();
        const auto* imagePixels = fromImage->images().front().payload.data();
        assert(viewPixels != sdk && imagePixels != ownBytes);
        std::memset(sdk, 0xEE, sizeof(sdk));
        std::memset(ownBytes, 0xEE, own.GetImageSize());
        assert(viewPixels[0] == 1U && viewPixels[3] == 4U);
        assert(imagePixels[0] == 1U && imagePixels[static_cast<std::size_t>(fromImage->images().front().payload.stride) + 1U] == 4U);
    }

    const Pylon::CPylonImage empty;
    assert(!adapter.convertGraphicsImage(empty, bgrBuilder).isValid());

    Pylon::CPylonDataContainer emptyContainer;
    PylonScene3DProfile profile;
    TrackedContextSource emptySource(GraphicsFrameContext::local());
    GraphicsFrameError emptyError = GraphicsFrameError::None;
    std::optional<PylonRangeRejection> emptyRejection;
    profile.family = PylonScene3DProfile::DeviceFamily::Blaze;
    assert(!adapter.convertGraphicsFrame(emptyContainer, {}, profile, 0U, nullptr,
        &emptyRejection, &emptySource, &emptyError).has_value());
    for (const auto family : {PylonScene3DProfile::DeviceFamily::StereoMini,
                              PylonScene3DProfile::DeviceFamily::StereoAce}) {
        profile.family = family;
        emptyError = GraphicsFrameError::None;
        emptyRejection.reset();
        assert(!adapter.convertGraphicsFrame(emptyContainer, {}, profile, 0U, nullptr,
            &emptyRejection, &emptySource, &emptyError).has_value());
        assert(emptySource.calls == 0U);
        assert(emptyError == GraphicsFrameError::InvalidInput);
        assert(emptyRejection && emptyRejection->reason == PylonRangeRejection::Reason::MissingRange);
    }

    // Synthetic SDK views use exactly the same production Stereo transaction.
    {
        float xyz[12] = {1,2,3, 4,5,6, 7,8,9, 10,11,12};
        std::uint16_t disparity[4] = {1,2,3,4};
        std::uint8_t mono[4] = {1,2,3,4};
        std::uint8_t rgb[12] = {};
        auto part = [](const void* data, std::size_t bytes, Pylon::EPixelType type, std::size_t stride) {
            CameraStereoPartView value;
            value.data = data; value.bytes = bytes; value.width = 2; value.height = 2;
            value.pixelType = type; value.stride = stride; value.hasStride = true; value.valid = true;
            return value;
        };
        CameraStereoParts parts;
        parts.range = part(xyz, sizeof(xyz), Pylon::PixelType_Coord3D_ABC32f, 24);
        parts.disparity = part(disparity, sizeof(disparity), Pylon::PixelType_Coord3D_C16, 4);
        PylonScene3DProfile stereo;
        stereo.geometry = PylonScene3DProfile::GeometryKind::DisparityReconstruction;
        stereo.coordinateScale = 1; stereo.baseline = 1; stereo.focalLength = 1;
        for (const auto family : {PylonScene3DProfile::DeviceFamily::StereoMini,
                                  PylonScene3DProfile::DeviceFamily::StereoAce}) {
            stereo.family = family;
            for (int mode = 0; mode < 2; ++mode) {
                parts.intensity = mode == 0
                    ? part(mono, sizeof(mono), Pylon::PixelType_Mono8, 2)
                    : part(rgb, sizeof(rgb), Pylon::PixelType_RGB8packed, 6);
                BudgetSource budget;
                GraphicsFrameError error = GraphicsFrameError::None;
                auto frame = adapter.convertStereoParts(parts, {}, stereo, budget, 100U, nullptr, &error);
                assert(frame && frame->hasImage() && frame->hasRangeFrame());
                assert(error == GraphicsFrameError::None && budget.calls == 1U);
                assert(budget.requested == budget.allocator->used);
                assert(budget.allocator->reserves == 2U);
            }
            parts.intensity = part(mono, sizeof(mono), Pylon::PixelType_Mono8, 2);
            parts.intensity.index = 0U; parts.range.index = 1U; parts.disparity.index = 2U;
            const bool ace = family == PylonScene3DProfile::DeviceFamily::StereoAce;
            const std::uint32_t selectedIndex = ace ? 2U : 1U;
            struct Outcome {
                std::optional<GraphicsFrame> frame;
                PylonFrameAnomalies anomalies;
                std::optional<PylonRangeRejection> first;
                GraphicsFrameError error = GraphicsFrameError::None;
                std::size_t calls = 0U;
            };
            // Every case publishes with the exact planned reserve sum and a single frame admission.
            const auto run = [&](const CameraStereoParts& input, std::uint64_t sequence,
                                 const PylonScene3DProfile& profileUsed) {
                Outcome out;
                BudgetSource budget;
                out.frame = adapter.convertStereoParts(input, {}, profileUsed, budget, sequence, &out.first,
                    &out.error, nullptr, &out.anomalies);
                out.calls = budget.calls;
                if (out.frame) assert(budget.calls == 1U && budget.requested == budget.allocator->used);
                return out;
            };
            const auto has = [](const Outcome& out, PylonRangeRejection::Reason reason, std::uint32_t partIndex) {
                for (std::size_t i = 0; i < out.anomalies.count; ++i)
                    if (out.anomalies.entries[i].reason == reason && out.anomalies.entries[i].affectedPart == partIndex)
                        return true;
                return false;
            };
            const auto selectedPart = [&](CameraStereoParts& input) -> CameraStereoPartView& {
                return ace ? input.disparity : input.range;
            };
            {   // Clean parts report nothing.
                const auto out = run(parts, 200U, stereo);
                assert(out.frame && out.frame->hasImage() && out.frame->hasRangeFrame());
                assert(out.anomalies.count == 0U && !out.first);
            }
            {   // Steady Stereo frames, clean and malformed, allocate nothing after warmup.
                auto backing = std::make_shared<UncountedLocalBacking>();
                auto pool = GraphicsFrameContentPool::create(8U);
                TrackedContextSource tracked(GraphicsFrameContext(backing, pool));
                auto malformed = parts;
                selectedPart(malformed).bytes = ace ? 6U : 36U;
                malformed.intensity.stride = 1U;
                for (const auto* input : {&parts, &malformed}) {
                    PylonFrameAnomalies anomalies;
                    for (int i = 0; i < 10; ++i) {
                        anomalies = {};
                        auto frame = adapter.convertStereoParts(*input, {}, stereo, tracked, i, nullptr, nullptr, nullptr, &anomalies);
                        assert(frame && frame->hasImage() && frame->hasRangeFrame());
                    }
                    gNewCount.store(0U, std::memory_order_relaxed);
                    gCountNew.store(true, std::memory_order_release);
                    for (int i = 0; i < 100; ++i) {
                        anomalies = {};
                        auto frame = adapter.convertStereoParts(*input, {}, stereo, tracked, i, nullptr, nullptr, nullptr, &anomalies);
                        assert(frame && frame->hasRangeFrame());
                    }
                    gCountNew.store(false, std::memory_order_release);
                    std::printf("Camera Stereo %s steady-state operator-new calls excluding backing allocator: %zu/100 frames\n",
                                ace ? "ace" : "mini", gNewCount.load(std::memory_order_acquire));
                    assert(gNewCount.load(std::memory_order_acquire) == 0U);
                }
            }
            {   // Short buffer: the held samples are copied, the rest is zero, and the part stays.
                auto input = parts;
                auto& sel = selectedPart(input);
                sel.bytes = ace ? 6U : 36U;
                const auto out = run(input, 201U, stereo);
                assert(out.frame && out.frame->hasImage() && out.frame->hasRangeFrame());
                assert(has(out, PylonRangeRejection::Reason::TruncatedData, selectedIndex));
                const auto& range = out.frame->ranges().front().payload;
                assert(range.validMask[0] == 1U && range.validMask[2] == 1U && range.validMask[3] == 0U);
                if (ace) assert(std::isnan(range.zValues[3]) && range.zValues[2] > 0.0F);
                else assert(range.zValues[2] == 9.0F && range.zValues[3] == 0.0F);
            }
            {   // Small stride: min(sdkStride, row) is copied per row and the part stays.
                auto input = parts;
                selectedPart(input).stride = 2U;
                const auto out = run(input, 202U, stereo);
                assert(out.frame && out.frame->hasRangeFrame());
                assert(has(out, PylonRangeRejection::Reason::StrideBelowMinimum, selectedIndex));
            }
            {   // Padded stride: padding is skipped and nothing is reported.
                auto input = parts;
                float paddedXyz[14] = {1,2,3, 4,5,6, 0, 7,8,9, 10,11,12, 0};
                std::uint16_t paddedDisparity[6] = {1,2,0, 3,4,0};
                auto& sel = selectedPart(input);
                sel.data = ace ? static_cast<const void*>(paddedDisparity) : static_cast<const void*>(paddedXyz);
                sel.bytes = ace ? sizeof(paddedDisparity) : sizeof(paddedXyz);
                sel.stride = ace ? 6U : 28U;
                const auto out = run(input, 203U, stereo);
                assert(out.frame && out.frame->hasRangeFrame() && out.anomalies.count == 0U);
                const auto& range = out.frame->ranges().front().payload;
                if (ace) assert(std::fabs(range.zValues[2] - 1000.0F / 3.0F) < 0.01F);
                else assert(range.zValues[2] == 9.0F && range.zValues[3] == 12.0F);
            }
            {   // Unexpected formats map to the same bytes per element, or stay raw bytes.
                auto input = parts;
                input.intensity.pixelType = Pylon::PixelType_BayerGR8;
                const std::uint16_t expectedRange[4] = {11, 12, 13, 14};
                std::uint16_t rawRange[4] = {11, 12, 13, 14};
                auto& sel = selectedPart(input);
                sel.data = rawRange; sel.bytes = sizeof(rawRange); sel.stride = 4U;
                sel.pixelType = Pylon::PixelType_Mono16;
                const auto out = run(input, 204U, stereo);
                std::memset(rawRange, 0xEE, sizeof(rawRange));   // the published bytes are a copy
                assert(out.frame && !out.frame->hasRangeFrame() && out.frame->images().size() == 2U);
                assert(has(out, PylonRangeRejection::Reason::UnsupportedFormat, 0U));
                assert(has(out, PylonRangeRejection::Reason::UnsupportedFormat, selectedIndex));
                const auto& image = out.frame->images()[0].payload;
                assert(image.pixelFormat == GraphicsImagePixelFormat::Mono8 && image.data()[3] == 4U);
                const auto& raw = out.frame->images()[1].payload;
                assert(raw.pixelFormat == GraphicsImagePixelFormat::Mono16 && raw.width == 2 && raw.height == 2);
                assert(std::memcmp(raw.data(), expectedRange, sizeof(expectedRange)) == 0);
            }
            {   // A missing part is absent with a diagnostic and never removes another part.
                auto input = parts;
                selectedPart(input) = {};
                auto out = run(input, 205U, stereo);
                assert(out.frame && out.frame->hasImage() && !out.frame->hasRangeFrame());
                assert(out.first && out.first->reason == PylonRangeRejection::Reason::MissingRange);
                input = parts;
                input.intensity = {};
                out = run(input, 206U, stereo);
                assert(out.frame && out.frame->hasRangeFrame() && !out.frame->hasImage());
                assert(out.anomalies.count >= 1U);
            }
            {   // Anomalies in two parts are all counted and both parts are published.
                auto input = parts;
                input.intensity.stride = 1U;
                selectedPart(input).bytes = ace ? 6U : 36U;
                const auto out = run(input, 207U, stereo);
                assert(out.frame && out.frame->hasImage() && out.frame->hasRangeFrame());
                assert(has(out, PylonRangeRejection::Reason::StrideBelowMinimum, 0U));
                assert(has(out, PylonRangeRejection::Reason::TruncatedData, selectedIndex));
                assert(out.anomalies.count >= 2U);
            }
            {   // Every selected part empty: nothing is published and no frame is admitted.
                CameraStereoParts none;
                auto out = run(none, 208U, stereo);
                assert(!out.frame && out.calls == 0U && out.error == GraphicsFrameError::InvalidInput);
                assert(out.anomalies.count >= 1U);
                auto input = parts;
                input.intensity.bytes = 0U; selectedPart(input).data = nullptr;
                out = run(input, 209U, stereo);
                assert(!out.frame && out.calls == 0U && out.error == GraphicsFrameError::InvalidInput);
                assert(has(out, PylonRangeRejection::Reason::MissingData, 0U));
                assert(has(out, PylonRangeRejection::Reason::MissingData, selectedIndex));
            }
            {   // Missing or invalid calibration publishes the samples as a raw range image.
                auto uncalibrated = stereo;
                if (ace) uncalibrated.baseline = 0.0; else uncalibrated.focalLength = 0.0;
                auto input = parts;
                auto& sel = selectedPart(input);
                const std::uint16_t depth[4] = {5, 6, 7, 8};
                sel.data = depth; sel.bytes = sizeof(depth); sel.stride = 4U; sel.pixelType = Pylon::PixelType_Coord3D_C16;
                auto out = run(input, 210U, uncalibrated);
                assert(out.frame && !out.frame->hasRangeFrame() && out.frame->images().size() == 2U);
                assert(has(out, PylonRangeRejection::Reason::InvalidCalibration, selectedIndex));
                assert(std::memcmp(out.frame->images()[1].payload.data(), depth, sizeof(depth)) == 0);
                uncalibrated.coordinateScale = 0.0; uncalibrated.baseline = 0.0; uncalibrated.focalLength = 0.0;
                out = run(input, 211U, uncalibrated);
                assert(out.frame && has(out, PylonRangeRejection::Reason::MissingCalibration, selectedIndex));
            }
            BudgetSource shortBudget;
            shortBudget.shortage = 1U;
            GraphicsFrameError error = GraphicsFrameError::None;
            auto frame = adapter.convertStereoParts(parts, {}, stereo, shortBudget, 102U, nullptr, &error);
            assert(!frame && error == GraphicsFrameError::Allocation);
            assert(shortBudget.calls == 1U && shortBudget.allocator->used < shortBudget.requested);
        }
    }

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
        source.coordinateReadableBytes = sizeof(coordinates);
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
        source.intensity.readableBytes = sizeof(intensitySamples);
        source.confidence.readableBytes = sizeof(confidenceSamples);

        std::size_t storageCalls = 0U;
        std::shared_ptr<std::uint8_t> storageBlock;
        auto storage = testContext([&](const std::size_t bytes) {
            ++storageCalls;
            storageBlock = std::shared_ptr<std::uint8_t>(
                new std::uint8_t[bytes], std::default_delete<std::uint8_t[]>());
            return storageBlock;
        });

        BlazeGraphicsFrameAdapter blaze;
        TrackedContextSource frameSource(storage);
        GraphicsFrameRequest rangeAndCloud;
        rangeAndCloud.components = GraphicsFrameComponent::Range | GraphicsFrameComponent::PointCloud;
        const auto scene = blaze.convertSource(source, rangeAndCloud, frameSource);
        assert(scene.has_value());
        GraphicsRangeFrameLayout expected;
        expected.width = 2; expected.height = 2;
        expected.channels.x = true; expected.channels.y = true; expected.channels.validMask = true;
        expected.channels.intensity = true; expected.channels.confidence = true; expected.channels.rgb = true;
        const auto expectedPlan = planGraphicsRangeWrite(expected);
        assert(expectedPlan && frameSource.calls == 1U);
        assert(frameSource.requestedBytes == expectedPlan.value().totalBytes);
        const GraphicsFrame sharedScene = *scene;
        assert(sharedScene.ranges().front().payload.zValues.data()
            == scene->ranges().front().payload.zValues.data());
        assert(storageCalls == 1U && storageBlock != nullptr);
        assert(scene->ranges().size() == 1U && scene->pointClouds().empty());

        const RangeFrame& range = scene->ranges().front().payload;
        assert(range.width == 2 && range.height == 2);
        assert(range.zValues.size() == 4U);
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
        const auto cloudOnly = blaze.convertSource(source, cloudOnlyRequest, frameSource);
        assert(cloudOnly.has_value() && cloudOnly->pointClouds().empty());
        assert(cloudOnly->ranges().size() == 1U);
        const RangeFrame& cloudOnlyRange = cloudOnly->ranges().front().payload;
        assert(cloudOnlyRange.canDerivePointCloud());
        assert(cloudOnlyRange.intensity.empty() && cloudOnlyRange.confidence.empty());
        assert(cloudOnlyRange.rgb.size() == 12U);

        // A Range-only request keeps no cloud colours.
        GraphicsFrameRequest rangeOnlyRequest;
        rangeOnlyRequest.components = GraphicsFrameComponent::Range;
        const auto rangeOnly = blaze.convertSource(source, rangeOnlyRequest, frameSource);
        assert(rangeOnly.has_value() && rangeOnly->pointClouds().empty());
        assert(rangeOnly->ranges().size() == 1U);
        assert(rangeOnly->ranges().front().payload.rgb.empty());

        // Range evidence names the real failed rule, keeps the SDK sequence, and
        // stays empty when a valid range (and its intensity) is retained.
        std::optional<PylonRangeRejection> rejection;
        const auto retainedRange = blaze.convertSource(source, rangeAndCloud, frameSource, 82U, &rejection);
        assert(retainedRange.has_value() && !rejection.has_value());

        BlazeRangeSource missing;
        missing.width = 2;
        missing.height = 2;
        rejection.reset();
        assert(!blaze.convertSource(missing, {}, frameSource, 83U, &rejection).has_value());
        assert(rejection.has_value());
        assert(rejection->reason == PylonRangeRejection::Reason::MissingData);
        assert(rejection->frameSequence == 83U);
        assert(rejection->width == 2U && rejection->height == 2U);

        frameSource.context = testContext([](const std::size_t) { return std::shared_ptr<std::uint8_t>(); });
        rejection.reset();
        assert(!blaze.convertSource(source, rangeAndCloud, frameSource, 84U, &rejection).has_value());
        assert(rejection.has_value());
        assert(rejection->reason == PylonRangeRejection::Reason::StorageRefused);
        assert(rejection->frameSequence == 84U);
        assert(rejection->requestedStorageBytes > 0U);
        assert(rejection->reportedStride == 0U && rejection->effectiveStride == source.coordinateStride);
        assert(rejection->readableBytes == sizeof(coordinates));
        assert(rejection->requiredBytes == sizeof(coordinates));

        frameSource.context = GraphicsFrameContext::local();
        auto shortStride = source;
        shortStride.coordinateStride = source.coordinateStride / 2U;
        shortStride.reportedCoordinateStride = shortStride.coordinateStride;
        rejection.reset();
        const auto fittedRange = blaze.convertSource(shortStride, rangeAndCloud, frameSource, 85U, &rejection);
        assert(fittedRange && fittedRange->hasRangeFrame());
        assert(fittedRange->ranges().front().payload.zValues[1] == 0.0F);
        assert(rejection && rejection->reason == PylonRangeRejection::Reason::StrideBelowMinimum);
        assert(rejection->reportedStride == shortStride.reportedCoordinateStride);
        assert(rejection->effectiveStride == shortStride.coordinateStride);
        assert(rejection->minimumRowBytes == source.coordinateStride);

        auto shortData = source;
        shortData.coordinateReadableBytes = sizeof(float) * 4U;
        shortData.intensity.readableBytes = sizeof(std::uint16_t);
        rejection.reset();
        const auto fittedShort = blaze.convertSource(shortData, rangeAndCloud, frameSource, 86U, &rejection);
        assert(fittedShort && fittedShort->hasRangeFrame());
        assert(rejection && rejection->reason == PylonRangeRejection::Reason::TruncatedData);
        assert(fittedShort->ranges().front().payload.zValues[3] == 0.0F);
        assert(fittedShort->ranges().front().payload.intensity[1] == 0.0F);

        auto different = source;
        different.intensity.width = 1; different.intensity.height = 2;
        different.intensity.stride = 2; different.intensity.readableBytes = 2;
        different.confidence.width = 3; different.confidence.height = 1;
        different.confidence.stride = 2; different.confidence.readableBytes = 2;
        PylonFrameAnomalies anomalies;
        rejection.reset();
        const auto independent = blaze.convertSource(different, {}, frameSource, 88U, &rejection, &anomalies);
        assert(independent && independent->hasRangeFrame() && independent->images().size() == 2U);
        assert(independent->images()[0].payload.width == 1 && independent->images()[0].payload.height == 2);
        assert(independent->images()[1].payload.width == 3 && independent->images()[1].payload.height == 1);
        bool sawIntensity = false, sawConfidence = false;
        for (std::size_t i = 0; i < anomalies.count; ++i) {
            sawIntensity |= anomalies.entries[i].affectedPart == 1U;
            sawConfidence |= anomalies.entries[i].affectedPart == 2U;
        }
        assert(anomalies.count >= 2U && sawIntensity && sawConfidence);
        assert(independent->ranges().front().payload.zValues[0] == 3.0F);

        // A listed part without bytes is absent with a diagnostic and adds no zero-filled channel.
        auto emptyIntensity = source;
        emptyIntensity.intensity.readableBytes = 0U;
        PylonFrameAnomalies emptyAnomalies;
        rejection.reset();
        const auto withoutIntensity = blaze.convertSource(
            emptyIntensity, rangeAndCloud, frameSource, 89U, &rejection, &emptyAnomalies);
        assert(withoutIntensity && withoutIntensity->hasRangeFrame());
        assert(withoutIntensity->ranges().front().payload.intensity.empty()
            && withoutIntensity->ranges().front().payload.rgb.empty());
        assert(withoutIntensity->ranges().front().payload.confidence.size() == 4U);
        assert(emptyAnomalies.count == 1U && emptyAnomalies.entries[0].affectedPart == 1U
            && emptyAnomalies.entries[0].reason == PylonRangeRejection::Reason::MissingData);

        auto noRange = source;
        noRange.coordinates = nullptr; noRange.coordinateReadableBytes = 0U;
        rejection.reset();
        const auto auxOnly = blaze.convertSource(noRange, {}, frameSource, 87U, &rejection);
        assert(auxOnly && auxOnly->images().size() == 2U && !auxOnly->hasRangeFrame());
        assert(rejection && rejection->reason == PylonRangeRejection::Reason::MissingRange);
        assert(auxOnly->images().front().payload.data()[0] == 100U);

        auto oddRange = source;
        oddRange.rangeAsImage = true;
        std::uint8_t rawBytes[] = {1U, 2U, 3U};
        oddRange.rawRange = {rawBytes, 2U, Pylon::PixelType_BayerGR8, sizeof(rawBytes)};
        rejection.reset();
        const auto retainedRaw = blaze.convertSource(oddRange, {}, frameSource, 88U, &rejection);
        std::memset(rawBytes, 0xEE, sizeof(rawBytes));   // the published bytes are a copy
        assert(retainedRaw && retainedRaw->images().size() == 3U && !retainedRaw->hasRangeFrame());
        assert(rejection);
        const auto& raw = retainedRaw->images().back().payload;
        assert(raw.data()[0] == 1U && raw.data()[1] == 2U && raw.data()[2] == 3U && raw.data()[3] == 0U);

        GraphicsFrameRequest unrequested;
        unrequested.components = GraphicsFrameComponent::Image;
        rejection.reset();
        const auto imageOnly = blaze.convertSource(source, unrequested, frameSource, 86U, &rejection);
        assert(imageOnly && imageOnly->hasImage() && !imageOnly->hasRangeFrame());
        assert(!rejection.has_value());

        // Steady blaze frames, clean and malformed, allocate nothing after warmup.
        {
            auto backing = std::make_shared<UncountedLocalBacking>();
            auto pool = GraphicsFrameContentPool::create(8U);
            TrackedContextSource steady(GraphicsFrameContext(backing, pool));
            auto malformed = source;
            malformed.coordinateStride = source.coordinateStride / 2U;
            malformed.reportedCoordinateStride = malformed.coordinateStride;
            malformed.coordinateReadableBytes = sizeof(float) * 4U;
            malformed.intensity.readableBytes = sizeof(std::uint16_t);
            malformed.confidence.stride = 1U;
            for (const auto* input : {&source, &malformed}) {
                PylonFrameAnomalies steadyAnomalies;
                for (int i = 0; i < 10; ++i) {
                    steadyAnomalies = {};
                    auto frame = blaze.convertSource(*input, {}, steady, i, nullptr, &steadyAnomalies);
                    assert(frame && frame->hasRangeFrame());
                }
                gNewCount.store(0U, std::memory_order_relaxed);
                gCountNew.store(true, std::memory_order_release);
                for (int i = 0; i < 100; ++i) {
                    steadyAnomalies = {};
                    auto frame = blaze.convertSource(*input, {}, steady, i, nullptr, &steadyAnomalies);
                    assert(frame && frame->hasRangeFrame());
                }
                gCountNew.store(false, std::memory_order_release);
                std::printf("Camera blaze %s steady-state operator-new calls excluding backing allocator: %zu/100 frames\n",
                            input == &source ? "clean" : "malformed", gNewCount.load(std::memory_order_acquire));
                assert(gNewCount.load(std::memory_order_acquire) == 0U);
            }
        }
    }
    return 0;
}
