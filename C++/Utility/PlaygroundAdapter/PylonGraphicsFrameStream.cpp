#include "PylonGraphicsFrameStream.h"

#include "CameraSystem.h"
#include "PylonGraphicsFrameAdapter.h"

#include <exception>
#include <memory>
#include <optional>
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

} // namespace

class PylonGraphicsFrameStream::Impl final
{
public:
    Impl(Camera* camera, GraphicsFrameCallback callback,
         CameraImageRejectionCallback imageRejection,
         CameraRangeRejectionCallback rangeRejection);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void setFrameStorage(GraphicsFrameContext storage)
    {
        {
            std::lock_guard<std::mutex> lock(_storageMutex);
            _storage = storage;
        }
        _adapter.setFrameStorage(std::move(storage));
    }

private:
    Camera* _camera = nullptr;
    GraphicsFrameCallback _callback;
    CameraImageRejectionCallback _imageRejection;
    CameraRangeRejectionCallback _rangeRejection;
    PylonGraphicsFrameAdapter _adapter;
    std::mutex _storageMutex;
    GraphicsFrameContext _storage = GraphicsFrameContext::local();
    GraphicsFrameCallbackGate _callbackGate;
    Camera::CallbackId _grabCallbackId = 0;
    Camera::CallbackId _grab3DCallbackId = 0;
};

PylonGraphicsFrameStream::Impl::Impl(
    Camera* camera,
    GraphicsFrameCallback callback,
    CameraImageRejectionCallback imageRejection,
    CameraRangeRejectionCallback rangeRejection)
    : _camera(camera), _callback(std::move(callback)),
      _imageRejection(std::move(imageRejection)),
      _rangeRejection(std::move(rangeRejection))
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
            try
            {
                GraphicsFrameContext context = GraphicsFrameContext::local();
                {
                    std::lock_guard<std::mutex> lock(_storageMutex);
                    context = _storage;
                }
                GraphicsFrameBuilder builder(context);
                std::optional<PylonImageRejection> rejection;
                GraphicsImage graphicsImage = _adapter.convertGraphicsImage(
                    image, builder, sequence, _imageRejection ? &rejection : nullptr);
                if (!graphicsImage.isValid())
                {
                    if (_imageRejection && rejection) _imageRejection(*rejection);
                    return;
                }

                builder.addImage("image", std::move(graphicsImage), "Image");
                auto frame = std::move(builder).publish();
                if (!frame)
                {
                    CameraSystem::syslog("Camera GraphicsFrame publication failed.", true);
                    return;
                }
                _callback(std::move(frame).value(), 0U);
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
        [this, callbackToken](const Pylon::CPylonDataContainer& container, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease) return;
            try
            {
                std::optional<PylonImageRejection> imageRejection;
                std::optional<PylonRangeRejection> rangeRejection;
                auto frame = _adapter.convertGraphicsFrame(
                    container, cameraGraphicsFrameRequest(), _camera->scene3DProfile(),
                    sequence, _imageRejection ? &imageRejection : nullptr,
                    _rangeRejection ? &rangeRejection : nullptr);
                // Ordered, best-effort evidence: 2D first, then the range rule.
                // A fallback that produced an image is not an assembly rejection.
                if (_imageRejection && imageRejection && (!frame || !frame->hasImage()))
                    _imageRejection(*imageRejection);
                if (_rangeRejection && rangeRejection) _rangeRejection(*rangeRejection);
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
    GraphicsFrameCallback callback,
    CameraImageRejectionCallback imageRejection,
    CameraRangeRejectionCallback rangeRejection)
    : _impl(std::make_unique<Impl>(camera, std::move(callback),
                                   std::move(imageRejection), std::move(rangeRejection)))
{
}

void PylonGraphicsFrameStream::setFrameStorage(GraphicsFrameContext storage)
{
    if (_impl) _impl->setFrameStorage(std::move(storage));
}

PylonGraphicsFrameStream::~PylonGraphicsFrameStream() = default;
