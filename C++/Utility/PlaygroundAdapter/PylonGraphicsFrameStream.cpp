#include "PylonGraphicsFrameStream.h"

#include "CameraSystem.h"
#include "PylonGraphicsFrameAdapter.h"

#include <atomic>
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
         CameraRangeRejectionCallback rangeRejection, CameraStorageOutcomeCallback storageOutcome);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void cancel() noexcept { _cancelled.store(true, std::memory_order_release); }

    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _contextSource = std::move(source);
    }

private:
    Camera* _camera = nullptr;
    GraphicsFrameCallback _callback;
    CameraImageRejectionCallback _imageRejection;
    CameraRangeRejectionCallback _rangeRejection;
    PylonGraphicsFrameAdapter _adapter;
    std::mutex _storageMutex;
    std::atomic<bool> _cancelled{false};
    std::atomic<bool> _refused{false};
    CameraStorageOutcomeCallback _storageOutcome;
    std::shared_ptr<GraphicsFrameContextSource> _contextSource;
    GraphicsFrameCallbackGate _callbackGate;
    Camera::CallbackId _grabCallbackId = 0;
    Camera::CallbackId _grab3DCallbackId = 0;
};

PylonGraphicsFrameStream::Impl::Impl(
    Camera* camera,
    GraphicsFrameCallback callback,
    CameraImageRejectionCallback imageRejection,
    CameraRangeRejectionCallback rangeRejection, CameraStorageOutcomeCallback storageOutcome)
    : _camera(camera), _callback(std::move(callback)),
      _imageRejection(std::move(imageRejection)),
      _rangeRejection(std::move(rangeRejection)), _storageOutcome(std::move(storageOutcome))
{
    if (!_camera || !_callback)
    {
        return;
    }

    const auto callbackToken = _callbackGate.token();
    _grabCallbackId = _camera->registerGrabCallback(
        [this, callbackToken](const Pylon::CPylonImage& image, const std::size_t sequence) {
            GraphicsFrameCallbackGate::Lease lease(callbackToken);
            if (!lease || _cancelled.load(std::memory_order_acquire)
                || _refused.load(std::memory_order_acquire)) return;
            try
            {
                std::shared_ptr<GraphicsFrameContextSource> contextSource;
                {
                    std::lock_guard<std::mutex> lock(_storageMutex);
                    contextSource = _contextSource;
                }
                if (!contextSource)
                {
                    CameraSystem::syslog("Camera GraphicsFrame context source is missing.", true);
                    return;
                }
                std::optional<PylonImageRejection> rejection;
                GraphicsFrameError storageError = GraphicsFrameError::None;
                auto frame = _adapter.convertGraphicsImageFrame(
                    image, *contextSource, sequence,
                    _imageRejection ? &rejection : nullptr,
                    &storageError, &_cancelled);
                if (!frame)
                {
                    if (storageError == GraphicsFrameError::ClassRefused)
                        _refused.store(true, std::memory_order_release);
                    if (_storageOutcome && storageError != GraphicsFrameError::None)
                        _storageOutcome(storageError);
                    if (_imageRejection && rejection && storageError != GraphicsFrameError::Disconnected
                        && storageError != GraphicsFrameError::NoReadyCredit) _imageRejection(*rejection);
                    if (storageError != GraphicsFrameError::None && storageError != GraphicsFrameError::NoReadyCredit
                        && storageError != GraphicsFrameError::Disconnected)
                    {
                        CameraSystem::syslog(storageError == GraphicsFrameError::ClassRefused
                            ? "Camera frame storage class was refused."
                            : "Camera GraphicsFrame 2D storage failed.", true);
                    }
                    return;
                }
                if (_imageRejection && rejection) _imageRejection(*rejection);
                if (!_cancelled.load(std::memory_order_acquire)) _callback(std::move(*frame), 0U);
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
            if (!lease || _cancelled.load(std::memory_order_acquire)
                || _refused.load(std::memory_order_acquire)) return;
            try
            {
                std::optional<PylonImageRejection> imageRejection;
                std::optional<PylonRangeRejection> rangeRejection;
                PylonFrameAnomalies anomalies;
                std::shared_ptr<GraphicsFrameContextSource> contextSource;
                {
                    std::lock_guard<std::mutex> lock(_storageMutex);
                    contextSource = _contextSource;
                }
                GraphicsFrameError storageError = GraphicsFrameError::None;
                auto frame = _adapter.convertGraphicsFrame(
                    container, cameraGraphicsFrameRequest(), _camera->scene3DProfile(),
                    sequence, _imageRejection ? &imageRejection : nullptr,
                    _rangeRejection ? &rangeRejection : nullptr, contextSource.get(),
                    &storageError, &_cancelled, &anomalies);
                if (storageError == GraphicsFrameError::ClassRefused) {
                    _refused.store(true, std::memory_order_release);
                    CameraSystem::syslog("Camera frame storage class was refused.", true);
                }
                if (_storageOutcome && storageError != GraphicsFrameError::None)
                    _storageOutcome(storageError);
                // Ordered, best-effort evidence: 2D first, then the range rule.
                // A fallback that produced an image is not an assembly rejection.
                if (_imageRejection && imageRejection && (!frame || !frame->hasImage()))
                    _imageRejection(*imageRejection);
                if (_rangeRejection) {
                    if (anomalies.count) {
                        for (std::size_t i = 0; i < anomalies.count; ++i) _rangeRejection(anomalies.entries[i]);
                        for (std::size_t reason = 0; reason < anomalies.overflowByReason.size(); ++reason) {
                            PylonRangeRejection overflowEvidence;
                            overflowEvidence.reason = static_cast<PylonRangeRejection::Reason>(reason);
                            overflowEvidence.frameSequence = sequence;
                            for (std::size_t i = 0; i < anomalies.overflowByReason[reason]; ++i)
                                _rangeRejection(overflowEvidence);
                        }
                    } else if (rangeRejection) _rangeRejection(*rangeRejection);
                }
                if (frame.has_value() && !_cancelled.load(std::memory_order_acquire))
                    _callback(std::move(*frame), 0U);
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
    cancel();
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
    CameraRangeRejectionCallback rangeRejection, CameraStorageOutcomeCallback storageOutcome)
    : _impl(std::make_unique<Impl>(camera, std::move(callback),
                                   std::move(imageRejection), std::move(rangeRejection), std::move(storageOutcome)))
{
}

void PylonGraphicsFrameStream::cancel() noexcept
{
    if (_impl) _impl->cancel();
}

void PylonGraphicsFrameStream::setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
{
    if (_impl) _impl->setFrameContextSource(std::move(source));
}

PylonGraphicsFrameStream::~PylonGraphicsFrameStream() = default;
