#pragma once

/**
 * @file PylonAssemblyRejection.h
 * @brief Bounded, Qt-free evidence for failed Camera image and range assembly.
 *
 * The adapter records only the failed rule and fixed numeric layout values. It
 * never stores SDK strings, calibration coefficients or pixel payload. The
 * controller maps these neutral values to the host diagnostic contract.
 */

#include <cstdint>
#include <limits>

/** @brief First failed rule and layout observed while assembling a 2D image. */
struct PylonImageRejection {
    enum class Reason : std::uint32_t {
        MissingBytes, InvalidDimensions, UnsupportedFormat, InvalidStride,
        StrideBelowMinimum, ExtentOverflow, TruncatedBytes, OddWidth,
        UnsupportedConversion, StorageRefused, NoImage
    };
    enum class Conversion : std::uint32_t { None, Borrow, Copy, SwapByteOrder, JpegCopy };

    Reason reason = Reason::NoImage;
    Conversion conversion = Conversion::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameSequence = 0;
    std::uint64_t pixelFormat = 0;
    std::uint64_t reportedStride = 0;
    std::uint64_t effectiveStride = 0;
    std::uint64_t minimumRowBytes = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    bool directInput = false;
};

/** @brief First failed rule and layout observed while assembling an organized range. */
struct PylonRangeRejection {
    enum class Reason : std::uint32_t {
        MissingRange, MissingData, InvalidDimensions, UnsupportedFormat, InvalidLayout,
        StrideBelowMinimum, TruncatedData, MissingCalibration, InvalidCalibration,
        UnsupportedGeometry, UnsupportedUnit, SizeLimitExceeded, StorageRefused
    };

    Reason reason = Reason::MissingRange;
    std::uint32_t partCount = 0;
    std::uint32_t affectedPart = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool frameRetained = false;
    bool imageRetained = false;
    bool rangeRetained = false;
    std::uint64_t frameSequence = 0;
    std::uint64_t pixelFormat = 0;
    std::uint64_t reportedStride = 0;
    std::uint64_t effectiveStride = 0;
    std::uint64_t minimumRowBytes = 0;
    std::uint64_t readableBytes = 0;
    std::uint64_t requiredBytes = 0;
    std::uint64_t requestedStorageBytes = 0;
};
