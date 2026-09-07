#include "PylonGraphicsFrameStream.h"

#include "CameraSystem.h"
#include "PylonGraphicsFrameAdapter.h"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace {

[[nodiscard]] GraphicsFrameRequest cameraGraphicsFrameRequest() noexcept
{
    GraphicsFrameRequest request;
    request.components = GraphicsFrameComponent::Range
        | GraphicsFrameComponent::PointCloud
        | GraphicsFrameComponent::Image;
    request.includeRangeAuxiliaryChannels = true;
    request.includePointCloudColors = true;
    return request;
}

class CameraReadyGuard final
{
public:
    explicit CameraReadyGuard(Camera* camera) noexcept : _camera(camera) {}
    ~CameraReadyGuard() { if (_camera) _camera->ready(); }

private:
    Camera* _camera;
};

} // namespace

class PylonGraphicsFrameStream::Impl final
{
public:
    Impl(Camera* camera, GraphicsFrameCallback callback);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

private:
    Camera* _camera = nullptr;
    GraphicsFrameCallback _callback;
    PylonGraphicsFrameAdapter _adapter;
    GraphicsFrameCallbackGate _callbackGate;
    Camera::CallbackId _grabCallbackId = 0;
    Camera::CallbackId _grab3DCallbackId = 0;
};

PylonGraphicsFrameStream::Impl::Impl(
    Camera* camera,
    GraphicsFrameCallback callback)
    : _camera(camera), _callback(std::move(callback))
{
    if (!_camera || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _camera->registerGrabCallback(
        [this, callbackToken](const Pylon::CPylonImage& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            CameraReadyGuard ready(_camera);
            try
            {
                GraphicsImage graphicsImage = _adapter.convertGraphicsImage(image, sequence);
                if (!graphicsImage.isValid()) return;

                GraphicsFrame frame;
                frame.setImage(std::move(graphicsImage));
                _callback(std::move(frame), 0U);
            }
            catch (const std::exception& error)
            {
                CameraSystem::syslog(
                    std::string("Camera GraphicsFrame callback failed: ") + error.what(), true);
            }
            catch (...)
            {
                CameraSystem::syslog("Camera GraphicsFrame callback failed with an unknown exception.", true);
            }
        });

    _grab3DCallbackId = _camera->registerGrab3DCallback(
        [this, callbackToken](const Pylon::CPylonDataContainer& container, const std::size_t) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            CameraReadyGuard ready(_camera);
            try
            {
                auto frame = _adapter.convertGraphicsFrame(
                    container, cameraGraphicsFrameRequest(), _camera->scene3DProfile());
                if (frame.has_value()) _callback(std::move(*frame), 0U);
            }
            catch (const std::exception& error)
            {
                CameraSystem::syslog(
                    std::string("Camera GraphicsFrame 3D callback failed: ") + error.what(), true);
            }
            catch (...)
            {
                CameraSystem::syslog("Camera GraphicsFrame 3D callback failed with an unknown exception.", true);
            }
        });
}

PylonGraphicsFrameStream::Impl::~Impl()
{
    _callbackGate.beginShutdown();
    if (!_camera) return;
    if (_grabCallbackId != 0U)
    {
        _camera->deregisterGrabCallback(_grabCallbackId);
    }
    if (_grab3DCallbackId != 0U)
    {
        _camera->deregisterGrab3DCallback(_grab3DCallbackId);
    }
    _callbackGate.waitForDrain();
}

PylonGraphicsFrameStream::PylonGraphicsFrameStream(
    Camera* camera,
    GraphicsFrameCallback callback)
    : _impl(std::make_unique<Impl>(camera, std::move(callback)))
{
}

PylonGraphicsFrameStream::~PylonGraphicsFrameStream() = default;
