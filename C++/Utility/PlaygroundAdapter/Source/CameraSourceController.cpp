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


// Host Debug enables diagnostics.* output; the INFO mode line remains in the normal log.
Q_LOGGING_CATEGORY(cameraSourceDiagnosticLog, "diagnostics.Camera", QtInfoMsg)

CameraSourceController::CameraSourceController(Camera* camera, IDevicePluginResourceClaims* claims,
    std::shared_ptr<IDevicePluginResourceClaim>* claim, QObject* parent)
    : AbstractSourceController(parent), _camera(camera), _claims(claims), _claim(claim) {
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
    _parameterClosing = true;
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

void CameraSourceController::start() {
    if (!_camera) return;
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
        });
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
