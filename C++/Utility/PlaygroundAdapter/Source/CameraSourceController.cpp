#include "CameraSourceController.h"
#include "DeviceRuntime/Contract/HardwareResourceKey.h"
#include "engine/GraphicsFrame.h"
#include "DeviceRuntime/Contract/SessionFrame.h"
#include "Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>
#include <QUuid>
#include <condition_variable>
#include <optional>


// Host Debug enables diagnostics.* output; the INFO mode line remains in the normal log.
Q_LOGGING_CATEGORY(cameraSourceDiagnosticLog, "diagnostics.Camera", QtInfoMsg)

namespace {

std::optional<DeviceImageRejection> deviceImageRejection(
    const PylonImageRejection& evidence) noexcept
{
    using Reason = PylonImageRejection::Reason;
    switch (evidence.reason)
    {
    case Reason::MissingBytes: return DeviceImageRejection::MissingBytes;
    case Reason::InvalidDimensions: return DeviceImageRejection::InvalidDimensions;
    case Reason::UnsupportedFormat: return DeviceImageRejection::UnsupportedFormat;
    case Reason::InvalidStride: return DeviceImageRejection::InvalidStride;
    case Reason::StrideBelowMinimum: return DeviceImageRejection::StrideBelowMinimum;
    case Reason::ExtentOverflow: return DeviceImageRejection::ExtentOverflow;
    case Reason::TruncatedBytes: return DeviceImageRejection::TruncatedBytes;
    case Reason::OddWidth: return DeviceImageRejection::OddWidth;
    case Reason::UnsupportedConversion: return DeviceImageRejection::UnsupportedConversion;
    case Reason::StorageRefused: return DeviceImageRejection::StorageRefused;
    case Reason::NoImage: return std::nullopt;
    }
    return std::nullopt;
}

DeviceImageConversion deviceImageConversion(
    const PylonImageRejection::Conversion conversion) noexcept
{
    using Conversion = PylonImageRejection::Conversion;
    switch (conversion)
    {
    case Conversion::None: return DeviceImageConversion::None;
    case Conversion::Borrow: return DeviceImageConversion::Borrow;
    case Conversion::Copy: return DeviceImageConversion::Copy;
    case Conversion::SwapByteOrder: return DeviceImageConversion::SwapByteOrder;
    case Conversion::JpegCopy: return DeviceImageConversion::JpegCopy;
    }
    return DeviceImageConversion::None;
}

DeviceRangeRejection deviceRangeRejection(const PylonRangeRejection::Reason reason) noexcept
{
    using Reason = PylonRangeRejection::Reason;
    switch (reason)
    {
    case Reason::MissingRange: return DeviceRangeRejection::MissingRange;
    case Reason::MissingData: return DeviceRangeRejection::MissingData;
    case Reason::InvalidDimensions: return DeviceRangeRejection::InvalidDimensions;
    case Reason::UnsupportedFormat: return DeviceRangeRejection::UnsupportedFormat;
    case Reason::InvalidLayout: return DeviceRangeRejection::InvalidLayout;
    case Reason::StrideBelowMinimum: return DeviceRangeRejection::StrideBelowMinimum;
    case Reason::TruncatedData: return DeviceRangeRejection::TruncatedData;
    case Reason::MissingCalibration: return DeviceRangeRejection::MissingCalibration;
    case Reason::InvalidCalibration: return DeviceRangeRejection::InvalidCalibration;
    case Reason::UnsupportedGeometry: return DeviceRangeRejection::UnsupportedGeometry;
    case Reason::UnsupportedUnit: return DeviceRangeRejection::UnsupportedUnit;
    case Reason::SizeLimitExceeded: return DeviceRangeRejection::SizeLimitExceeded;
    case Reason::StorageRefused: return DeviceRangeRejection::StorageRefused;
    case Reason::StorageMisaligned: return DeviceRangeRejection::StorageMisaligned;
    }
    return DeviceRangeRejection::InvalidLayout;
}

/** Single always emits; Live emits the first and every 100th failure of one reason. */
bool sampleRejection(std::atomic<std::uint64_t>& counter, const bool single) noexcept
{
    if (single) return true;
    const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1U;
    return count == 1U || count % 100U == 0U;
}

} // namespace

CameraSourceController::CameraSourceController(Camera* camera, IDevicePluginResourceClaims* claims,
    std::shared_ptr<IDevicePluginResourceClaim>* claim, QObject* parent,
    InitialTargetsPolicy initialTargetsPolicy, DeviceImageRejectionSink imageRejectionSink,
    DeviceRangeRejectionSink rangeRejectionSink)
    : AbstractSourceController(parent), _camera(camera), _initialTargetsPolicy(initialTargetsPolicy),
      _claims(claims), _claim(claim), _imageRejectionSink(imageRejectionSink),
      _rangeRejectionSink(rangeRejectionSink) {
    _parameterInstance = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    registerCallbacks();
    startChangeWorker();
}

void CameraSourceController::setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider)
{
    if (!_graphicsStream) return;
    _graphicsStream->setFrameStorage(frameStorageFunction(std::move(provider)));
}

void CameraSourceController::releaseClaim() {
    if (_claim) _claim->reset();
    _claimKey.clear();
}

CameraSourceController::~CameraSourceController() {
    _parameterClosing.store(true, std::memory_order_release);
    if (_camera) _camera->wakeInitialDiscoveryWaiters();
    if (_camera && _nodeCallbackId != 0) {
        _camera->deregisterNodeUpdatedCallback(_nodeCallbackId);
        _nodeCallbackId = 0;
    }
    stopChangeWorker();
    const auto workers = _parameterWorkers;
    for (auto* worker : workers) {
        disconnect(worker, nullptr, this, nullptr);
        worker->wait();
        delete worker;
    }
    _parameterWorkers.clear();
    stop();
    deregisterCallbacks();
}

void CameraSourceController::beginRejectionRun(const bool single) {
    _rejectionRunSingle.store(single, std::memory_order_relaxed);
    for (auto& count : _imageRejectionCounts) count.store(0, std::memory_order_relaxed);
    for (auto& count : _rangeRejectionCounts) count.store(0, std::memory_order_relaxed);
}

void CameraSourceController::reportImageRejection(const PylonImageRejection& evidence)
{
    if (!_imageRejectionSink) return;
    const auto reason = deviceImageRejection(evidence);
    if (!reason) return;
    const bool single = _rejectionRunSingle.load(std::memory_order_relaxed);
    if (!sampleRejection(_imageRejectionCounts[static_cast<std::size_t>(*reason)], single)) return;

    DeviceImageRejectionObservation observation;
    observation.reason = *reason;
    observation.conversion = deviceImageConversion(evidence.conversion);
    observation.sourceIndex = 0U;
    observation.width = evidence.width;
    observation.height = evidence.height;
    observation.directInput = evidence.directInput;
    observation.frameSequence = evidence.frameSequence;
    observation.pixelFormat = evidence.pixelFormat;
    observation.reportedStride = evidence.reportedStride;
    observation.effectiveStride = evidence.effectiveStride;
    observation.minimumRowBytes = evidence.minimumRowBytes;
    observation.readableBytes = evidence.readableBytes;
    observation.requiredBytes = evidence.requiredBytes;
    _imageRejectionSink(observation);
}

void CameraSourceController::reportRangeRejection(const PylonRangeRejection& evidence)
{
    if (!_rangeRejectionSink) return;
    const auto reason = deviceRangeRejection(evidence.reason);
    const bool single = _rejectionRunSingle.load(std::memory_order_relaxed);
    if (!sampleRejection(_rangeRejectionCounts[static_cast<std::size_t>(reason)], single)) return;

    DeviceRangeRejectionObservation observation;
    observation.reason = reason;
    observation.sourceIndex = 0U;
    observation.partCount = evidence.partCount;
    observation.affectedPart = evidence.affectedPart;
    observation.width = evidence.width;
    observation.height = evidence.height;
    observation.frameRetained = evidence.frameRetained;
    observation.imageRetained = evidence.imageRetained;
    observation.rangeRetained = evidence.rangeRetained;
    observation.frameSequence = evidence.frameSequence;
    observation.pixelFormat = evidence.pixelFormat;
    observation.reportedStride = evidence.reportedStride;
    observation.effectiveStride = evidence.effectiveStride;
    observation.minimumRowBytes = evidence.minimumRowBytes;
    observation.readableBytes = evidence.readableBytes;
    observation.requiredBytes = evidence.requiredBytes;
    observation.requestedStorageBytes = evidence.requestedStorageBytes;
    _rangeRejectionSink(observation);
}

void CameraSourceController::start() {
    if (!_camera) return;
    beginRejectionRun(false);
    const auto started = _camera->startAcquisition(CameraAcquisitionAction::Live);
    reportAcquisitionStart(started, "live");
    if (!started.started) throw std::runtime_error(started.reason);
    _isGrabbing = true;
}

void CameraSourceController::stop() {
    if (_camera) {
        _camera->stop();
        _isGrabbing = false;
    }
}

bool CameraSourceController::isGrabbing() const {
    return _isGrabbing;
}

void CameraSourceController::setFrameConsumer(FrameConsumer consumer) {
    _frameConsumer = std::move(consumer);
}

void CameraSourceController::registerCallbacks() {
    if (!_camera) return;

    // 1. Status callback
    _statusCallbackId = _camera->registerStatusCallback([this](Camera::Status status, bool on) {
        if (status == Camera::GrabbingStatus) {
            _isGrabbing = on;
            // Acquisition start/stop re-observes writability, because the device locks
            // transport-layer parameters only while the pylon TLParamsLocked value is set.
            enqueueParameterChange("Root");
            emit diagnosticAcquisitionChanged(on, QStringLiteral("camera"));
        } else if (status == Camera::ConnectionStatus && !on) {
            enqueueParameterConnectionLost("The camera is disconnected.");
        }
    });
    _nodeCallbackId = _camera->registerNodeUpdatedCallback(
        [this](const std::string& nodeName) { enqueueParameterChange(nodeName); });

    _graphicsStream = std::make_unique<PylonGraphicsFrameStream>(
        _camera,
        [this](GraphicsFrame&& payload, const unsigned int sourceIndex) {
            if (!_frameConsumer) return;
            SessionFrame frame;
            frame.payload = std::move(payload);
            frame.frameSeq = _frameSeq.fetch_add(1, std::memory_order_relaxed);
            _frameConsumer(std::move(frame), sourceIndex);
        },
        [this](const PylonImageRejection& evidence) { reportImageRejection(evidence); },
        [this](const PylonRangeRejection& evidence) { reportRangeRejection(evidence); });
}

void CameraSourceController::deregisterCallbacks() {
    if (!_camera) return;

    _graphicsStream.reset();

    if (_nodeCallbackId != 0) {
        _camera->deregisterNodeUpdatedCallback(_nodeCallbackId);
        _nodeCallbackId = 0;
    }
    if (_statusCallbackId != 0) {
        _camera->deregisterStatusCallback(_statusCallbackId);
        _statusCallbackId = 0;
    }
}

bool CameraSourceController::supports3D() const {
    if (!_camera) {
        return false;
    }
    const PylonScene3DProfile profile = _camera->scene3DProfile();
    return profile.family != PylonScene3DProfile::DeviceFamily::Image2D;
}


std::optional<SourceRuntimeDiagnostics> CameraSourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const
{
    if (sourceIndex != 0 || !_camera) return std::nullopt;
    const auto value = _camera->runtimeDiagnostics();
    return SourceRuntimeDiagnostics{value.grabbing, value.delivered, value.failed};
}

std::vector<GraphicsSourceDescriptor> CameraSourceController::ownedSourceDescriptors(
    const CameraConnectionState& state) const
{
    // One Camera connection owns one native stream. The source id is the exact connected device
    // identity, so source.sources, the producer counters and the connection state address the
    // same source; the index is the fixed single-stream slot.
    if (!state.connected) return {};
    return {{0U, state.deviceId, state.displayName}};
}

std::vector<GraphicsSourceDescriptor> CameraSourceController::sourceDescriptors() const
{
    if (!_camera) return {};
    return ownedSourceDescriptors(_camera->connectionState());
}
