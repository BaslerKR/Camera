#include "CameraSourceController.h"
#include "engine/GraphicsFrame.h"
#include "SessionFrame.h"
#include "Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h"
#include <QUuid>


CameraSourceController::CameraSourceController(Camera* camera, QObject* parent)
    : AbstractSourceController(parent), _camera(camera) {
    _parameterInstance = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    registerCallbacks();
}

CameraSourceController::~CameraSourceController() {
    _parameterClosing = true;
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

void CameraSourceController::start() {
    if (_camera) {
        _isGrabbing = true;
        _camera->grab();
    }
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
            emit diagnosticAcquisitionChanged(on, QStringLiteral("camera"));
        }
    });

    _graphicsStream = std::make_unique<PylonGraphicsFrameStream>(
        _camera,
        [this](GraphicsFrame&& payload, const unsigned int sourceIndex,
               const CameraFrameCapture& capture) {
            if (!_frameConsumer) return;
            SessionFrame frame;
            frame.payload = std::move(payload);
            frame.frameSeq = _frameSeq.fetch_add(1, std::memory_order_relaxed);
            frame.capture.valid = capture.valid;
            frame.capture.generation = capture.generation;
            frame.capture.sequence = capture.sequence;
            frame.capture.triggerId = capture.triggerId;
            _frameConsumer(std::move(frame), sourceIndex);
        });
}

void CameraSourceController::deregisterCallbacks() {
    if (!_camera) return;

    _graphicsStream.reset();

    if (_statusCallbackId != 0) {
        _camera->deregisterStatusCallback(_statusCallbackId);
        _statusCallbackId = 0;
    }
}

SourceProducerCapabilities CameraSourceController::producerCapabilities(const unsigned sourceIndex) const {
    static_cast<void>(sourceIndex);
    SourceProducerCapabilities capabilities;
    capabilities.captureIdentity = true;
    capabilities.captureCounter = true;
    capabilities.deviceDropCount = true;
    if (_camera) {
        capabilities.trigger = _camera->softwareTriggerAvailable();
        capabilities.poolSize = _camera->streamBufferCount();
    }
    return capabilities;
}

std::optional<SourceCaptureMark> CameraSourceController::captureCounter(const unsigned sourceIndex) const {
    if (!_camera || sourceIndex != 0U) return std::nullopt;
    const auto mark = _camera->captureCounter();
    if (!mark) return std::nullopt;
    return SourceCaptureMark{mark->generation, mark->value};
}

std::optional<SourceCaptureMark> CameraSourceController::requestTrigger(const unsigned sourceIndex) {
    if (!_camera || sourceIndex != 0U) return std::nullopt;
    const auto mark = _camera->requestSoftwareTrigger();
    if (!mark) return std::nullopt;
    return SourceCaptureMark{mark->generation, mark->value};
}

std::uint64_t CameraSourceController::deviceDropCount(const unsigned sourceIndex) const {
    if (!_camera || sourceIndex != 0U) return 0;
    return _camera->deviceDropCount();
}

bool CameraSourceController::supports3D() const {
    if (!_camera) {
        return false;
    }
    const PylonScene3DProfile profile = _camera->scene3DProfile();
    return profile.family != PylonScene3DProfile::DeviceFamily::Image2D;
}
