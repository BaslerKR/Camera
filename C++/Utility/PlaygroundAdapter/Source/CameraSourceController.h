#pragma once
#include "AbstractSourceController.h"
#include "Camera.h"
#include <atomic>
#include <memory>

class PylonGraphicsFrameStream;

class CameraSourceController : public AbstractSourceController {
    Q_OBJECT
public:
    explicit CameraSourceController(Camera* camera, QObject* parent = nullptr);
    ~CameraSourceController() override;

    void start() override;
    void stop() override;
    bool isGrabbing() const override;
    void setFrameConsumer(FrameConsumer consumer) override;
    bool supports3D() const override;

private:
    void registerCallbacks();
    void deregisterCallbacks();

    Camera* _camera;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<uint64_t> _frameSeq{0};

    Camera::CallbackId _statusCallbackId = 0;
    std::unique_ptr<PylonGraphicsFrameStream> _graphicsStream;
};
