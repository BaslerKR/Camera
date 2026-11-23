#include "CameraSourceController.h"
#include "DeviceRuntime/Contract/DeviceImageDiagnostics.h"
#include "PylonAssemblyRejection.h"

#include <QObject>
#include <QtTest>
#include <mutex>
#include <vector>

namespace {
std::mutex observationMutex;
std::vector<DeviceImageRejectionObservation> imageObservations;
std::vector<DeviceRangeRejectionObservation> rangeObservations;

void collectImage(const DeviceImageRejectionObservation& observation) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(observationMutex);
        imageObservations.push_back(observation);
    } catch (...) {
    }
}

void collectRange(const DeviceRangeRejectionObservation& observation) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(observationMutex);
        rangeObservations.push_back(observation);
    } catch (...) {
    }
}

PylonImageRejection truncatedImage()
{
    PylonImageRejection evidence;
    evidence.reason = PylonImageRejection::Reason::TruncatedBytes;
    evidence.conversion = PylonImageRejection::Conversion::Copy;
    evidence.width = 64U;
    evidence.height = 48U;
    evidence.frameSequence = 412U;
    evidence.pixelFormat = 0x01100007ULL;
    evidence.reportedStride = 128U;
    evidence.effectiveStride = 128U;
    evidence.minimumRowBytes = 128U;
    evidence.readableBytes = 1000U;
    evidence.requiredBytes = 6144U;
    return evidence;
}
} // namespace

class CameraRejectionSamplingTest : public QObject
{
    Q_OBJECT
private slots:
    void init() {
        std::lock_guard<std::mutex> lock(observationMutex);
        imageObservations.clear();
        rangeObservations.clear();
    }

    void singleEmitsEveryFailure() {
        CameraSourceController controller(nullptr, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::Cached, &collectImage, &collectRange);
        controller.beginRejectionRun(true);
        const auto evidence = truncatedImage();
        for (int i = 0; i < 5; ++i) controller.reportImageRejection(evidence);
        std::lock_guard<std::mutex> lock(observationMutex);
        QCOMPARE(imageObservations.size(), std::size_t{5});
        const auto& first = imageObservations.front();
        QCOMPARE(first.reason, DeviceImageRejection::TruncatedBytes);
        QCOMPARE(first.conversion, DeviceImageConversion::Copy);
        QCOMPARE(first.frameSequence, std::uint64_t{412});
        QCOMPARE(first.width, std::uint32_t{64});
        QCOMPARE(first.height, std::uint32_t{48});
        QCOMPARE(first.pixelFormat, std::uint64_t{0x01100007});
        QCOMPARE(first.requiredBytes, std::uint64_t{6144});
        QCOMPARE(first.directInput, false);
    }

    void liveSamplesFirstAndEveryHundredthPerReason() {
        CameraSourceController controller(nullptr, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::Cached, &collectImage, nullptr);
        controller.beginRejectionRun(false);
        const auto evidence = truncatedImage();
        for (int i = 0; i < 250; ++i) controller.reportImageRejection(evidence);
        {
            std::lock_guard<std::mutex> lock(observationMutex);
            QCOMPARE(imageObservations.size(), std::size_t{3});
        }
        auto other = evidence;
        other.reason = PylonImageRejection::Reason::StorageRefused;
        controller.reportImageRejection(other);
        {
            std::lock_guard<std::mutex> lock(observationMutex);
            QCOMPARE(imageObservations.size(), std::size_t{4});
            QCOMPARE(imageObservations.back().reason, DeviceImageRejection::StorageRefused);
        }
        controller.beginRejectionRun(false);
        controller.reportImageRejection(evidence);
        std::lock_guard<std::mutex> lock(observationMutex);
        QCOMPARE(imageObservations.size(), std::size_t{5});
        QCOMPARE(imageObservations.back().reason, DeviceImageRejection::TruncatedBytes);
    }

    void mapsRangeEvidenceAndRetention() {
        CameraSourceController controller(nullptr, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::Cached, nullptr, &collectRange);
        controller.beginRejectionRun(true);
        PylonRangeRejection evidence;
        evidence.reason = PylonRangeRejection::Reason::StorageRefused;
        evidence.partCount = 3U;
        evidence.affectedPart = 2U;
        evidence.width = 320U;
        evidence.height = 240U;
        evidence.frameSequence = 77U;
        evidence.pixelFormat = 0x012000bdULL;
        evidence.requestedStorageBytes = 4096U;
        controller.reportRangeRejection(evidence);
        evidence.reason = PylonRangeRejection::Reason::InvalidLayout;
        evidence.frameRetained = true;
        evidence.imageRetained = true;
        controller.reportRangeRejection(evidence);

        std::lock_guard<std::mutex> lock(observationMutex);
        QCOMPARE(rangeObservations.size(), std::size_t{2});
        const auto& refused = rangeObservations.front();
        QCOMPARE(refused.reason, DeviceRangeRejection::StorageRefused);
        QCOMPARE(refused.frameSequence, std::uint64_t{77});
        QCOMPARE(refused.partCount, std::uint32_t{3});
        QCOMPARE(refused.affectedPart, std::uint32_t{2});
        QCOMPARE(refused.requestedStorageBytes, std::uint64_t{4096});
        QCOMPARE(refused.frameRetained, false);
        const auto& retained = rangeObservations.back();
        QCOMPARE(retained.reason, DeviceRangeRejection::InvalidLayout);
        QCOMPARE(retained.frameRetained, true);
        QCOMPARE(retained.imageRetained, true);
        QCOMPARE(retained.rangeRetained, false);
    }
};

QTEST_GUILESS_MAIN(CameraRejectionSamplingTest)
#include "RejectionSamplingTest.moc"
