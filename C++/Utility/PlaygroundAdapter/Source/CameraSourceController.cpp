#include "CameraSourceController.h"
#include "engine/GraphicsFrame.h"
#include "SessionFrame.h"
#include "Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h"


CameraSourceController::CameraSourceController(Camera* camera, QObject* parent)
    : AbstractSourceController(parent), _camera(camera) {
    registerCallbacks();
}

CameraSourceController::~CameraSourceController() {
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
        }
    });

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
