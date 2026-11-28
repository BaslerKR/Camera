#include "CameraSourceController.h"
#include "CameraSystem.h"
#include <pylon/PylonImage.h>
#include "DeviceRuntime/Contract/DeviceImageDiagnostics.h"
#include "PylonAssemblyRejection.h"
#include "DeviceRuntime/Contract/SessionFrame.h"

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

class ScriptedSource final : public GraphicsFrameContextSource
{
public:
    GraphicsFrameError error = GraphicsFrameError::None;
    int calls = 0;
    GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t) noexcept override {
        ++calls;
        if (error != GraphicsFrameError::None) return error;
        return GraphicsFrameContext::local();
    }
};

class CameraRejectionSamplingTest : public QObject
{
    Q_OBJECT
private slots:
    void init() {
        std::lock_guard<std::mutex> lock(observationMutex);
        imageObservations.clear();
        rangeObservations.clear();
    }

    void readinessAndOldRunFencing() {
        CameraSystem system([](Pylon::DeviceInfoList_t&) {});
        Camera camera(&system);
        CameraSourceController controller(&camera, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::Cached, &collectImage, nullptr);
        auto source = std::make_shared<ScriptedSource>();
        controller.setFrameContextSource(source);
        int published = 0;
        controller.setFrameConsumer([&](SessionFrame&&, unsigned) { ++published; });
        controller.beginRejectionRun(false);
        QVERIFY(!camera._grabCallbacks.empty());
        auto oldCallback = camera._grabCallbacks.begin()->second;
        auto image = Pylon::CPylonImage::Create(Pylon::PixelType_Mono8, 2U, 2U);
        source->error = GraphicsFrameError::NoReadyCredit;
        oldCallback(image, 1U);
        QCOMPARE(source->calls, 1);
        QCOMPARE(controller._droppedFrames.load(), std::uint64_t{1});
        QCOMPARE(published, 0);
        source->error = GraphicsFrameError::ClassRefused;
        oldCallback(image, 2U);
        QCOMPARE(source->calls, 2);
        QCOMPARE(controller._refusedFrames.load(), std::uint64_t{1});
        QCOMPARE(controller._droppedFrames.load(), std::uint64_t{2});
        {
            std::lock_guard<std::mutex> lock(observationMutex);
            QVERIFY(!imageObservations.empty());
            QCOMPARE(imageObservations.back().reason, DeviceImageRejection::StorageRefused);
        }
        // A refusal drops that frame only: the next frame tries again and is counted.
        oldCallback(image, 3U);
        QCOMPARE(source->calls, 3);
        QCOMPARE(controller._refusedFrames.load(), std::uint64_t{2});
        QCOMPARE(controller._droppedFrames.load(), std::uint64_t{3});
        QCOMPARE(published, 0);
        source->error = GraphicsFrameError::None;
        oldCallback(image, 4U);
        QCOMPARE(source->calls, 4);
        QCOMPARE(published, 1);
        controller.cancelFrameRun();
        controller.beginRejectionRun(false);
        oldCallback(image, 5U);
        QCOMPARE(source->calls, 4);
        QCOMPARE(published, 1);
        QVERIFY(!camera._grabCallbacks.empty());
        auto newCallback = camera._grabCallbacks.begin()->second;
        newCallback(image, 6U);
        QCOMPARE(source->calls, 5);
        QCOMPARE(published, 2);
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

    void countsEveryMultipartAnomaly() {
        CameraSourceController controller(nullptr, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::Cached, nullptr, &collectRange);
        controller.beginRejectionRun(true);
        PylonRangeRejection first;
        first.reason = PylonRangeRejection::Reason::TruncatedData;
        first.affectedPart = 1U;
        PylonRangeRejection second;
        second.reason = PylonRangeRejection::Reason::InvalidDimensions;
        second.affectedPart = 2U;
        controller.reportRangeRejection(first);
        controller.reportRangeRejection(second);
        QCOMPARE(controller._rangeRejectionCounts[static_cast<std::size_t>(DeviceRangeRejection::TruncatedData)].load(), std::uint64_t{1});
        QCOMPARE(controller._rangeRejectionCounts[static_cast<std::size_t>(DeviceRangeRejection::InvalidDimensions)].load(), std::uint64_t{1});
        std::lock_guard<std::mutex> lock(observationMutex);
        QCOMPARE(rangeObservations.size(), std::size_t{2});
        QCOMPARE(rangeObservations[0].affectedPart, std::uint32_t{1});
        QCOMPARE(rangeObservations[1].affectedPart, std::uint32_t{2});
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
