#ifndef CAMERA_H
#define CAMERA_H

/**
 * @file Camera.h
 * @brief Wrapper for one Basler pylon camera connection, grab loop, and node callbacks.
 *
 * Keeps SDK types inside the Camera submodule and exposes status/grab callback
 * hooks to the host application.
 */

#include <pylon/PylonIncludes.h>
#include <pylon/BaslerUniversalInstantCamera.h>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

using namespace Pylon;
using namespace std;

class CameraSystem;
class Camera : public Pylon::CConfigurationEventHandler,
               public Pylon::CCameraEventHandler
{
public:
    using CallbackId = size_t;

    explicit Camera(CameraSystem* parent, int allottedNumber=0);
    ~Camera() override;

    enum Status{
        GrabbingStatus,
        ConnectionStatus
    };
    using StatusCallback = std::function<void(Status status, bool on)>;
    /**
     * @brief Registers a callback for camera status transitions.
     * @param cb Callback invoked when grabbing or connection state changes.
     * @return Callback identifier that can be passed to `deregisterStatusCallback()`.
     */
    CallbackId registerStatusCallback(StatusCallback cb);
    /**
     * @brief Removes a previously registered camera status callback.
     * @param id Callback identifier returned by `registerStatusCallback()`.
     * @return `true` if the callback existed and was removed.
     */
    bool deregisterStatusCallback(CallbackId id);
    /**
     * @brief Removes every registered camera status callback.
     */
    void clearStatusCallbacks();

    bool open(const std::string& cameraName="");
    bool isOpened() const;
    void close();
    std::string getConnectedCameraName(){ return _connectedCameraName; }

    using GrabCallback = std::function<void(const CPylonImage&, size_t frame)>;
    /**
     * @brief Registers a callback for 2D grab results.
     * @param cb Callback invoked for every successfully grabbed 2D frame.
     * @return Callback identifier that can be passed to `deregisterGrabCallback()`.
     */
    CallbackId registerGrabCallback(GrabCallback cb);
    /**
     * @brief Removes a previously registered 2D grab callback.
     * @param id Callback identifier returned by `registerGrabCallback()`.
     * @return `true` if the callback existed and was removed.
     */
    bool deregisterGrabCallback(CallbackId id);
    /**
     * @brief Removes every registered 2D grab callback.
     */
    void clearGrabCallbacks();

    using Grab3DCallback = std::function<void(const Pylon::CPylonDataContainer&, size_t frame)>;
    /**
     * @brief Registers a callback for 3D grab results.
     * @param cb Callback invoked for every successfully grabbed 3D container.
     * @return Callback identifier that can be passed to `deregisterGrab3DCallback()`.
     */
    CallbackId registerGrab3DCallback(Grab3DCallback cb);
    /**
     * @brief Removes a previously registered 3D grab callback.
     * @param id Callback identifier returned by `registerGrab3DCallback()`.
     * @return `true` if the callback existed and was removed.
     */
    bool deregisterGrab3DCallback(CallbackId id);
    /**
     * @brief Removes every registered 3D grab callback.
     */
    void clearGrab3DCallbacks();
    void ready();

    void grab(size_t frames=0);
    void stop();

    std::vector<std::string> getUpdatedCameraList() const;
    GenApi::INodeMap& getNodeMap();

    using NodeCallback = std::function<void(GenApi::INode*)>;
    /**
     * @brief Registers a callback for node updates emitted by camera events.
     * @param cb Callback invoked when a camera node changes.
     * @return Callback identifier that can be passed to `deregisterNodeUpdatedCallback()`.
     */
    CallbackId registerNodeUpdatedCallback(NodeCallback cb);
    /**
     * @brief Removes a previously registered node update callback.
     * @param id Callback identifier returned by `registerNodeUpdatedCallback()`.
     * @return `true` if the callback existed and was removed.
     */
    bool deregisterNodeUpdatedCallback(CallbackId id);
    /**
     * @brief Removes every registered node update callback.
     */
    void clearNodeUpdatedCallbacks();

private:
    enum class StreamKind
    {
        Image2D,
        MultiPart3D
    };

    CameraSystem *_system;
    CBaslerUniversalInstantCamera _currentCamera;
    std::string _connectedCameraName;
    int _allottedNumber = 0;

    std::thread _thread;
    std::atomic<bool> _isRunning=false;

    std::mutex _statusMutex;
    std::unordered_map<size_t, StatusCallback> _statusObservers;
    std::atomic<size_t> _nextStatusObserverId{1};

    std::mutex _grabCallbackMutex;
    std::unordered_map<size_t, GrabCallback> _grabCallbacks;
    std::atomic<size_t> _nextGrabCallbackId{1};

    std::mutex _grab3DCallbackMutex;
    std::unordered_map<size_t, Grab3DCallback> _grab3DCallbacks;
    std::atomic<size_t> _nextGrab3DCallbackId{1};

    std::mutex _nodeCallbackMutex;
    std::unordered_map<size_t, NodeCallback> _nodeCallbacks;
    std::atomic<size_t> _nextNodeCallbackId{1};

    std::mutex _permitMutex;
    std::condition_variable _permitCondition;
    std::atomic<int> _permits{0};

    std::atomic<size_t> _frameSeq{0};
    std::atomic<size_t> _frameTarget{0};
    StreamKind _streamKind = StreamKind::Image2D;

    void configureStreamForConnectedCamera();
    void configureBlazeStream(GenApi::INodeMap& nodeMap);
    void configureStereoAceStream(GenApi::INodeMap& nodeMap);


protected:
    // Pylon::CConfigurationEventHandler functions
    void OnAttached(Pylon::CInstantCamera& camera) override;
    void OnDetached(Pylon::CInstantCamera& camera) override;
    void OnDestroyed(Pylon::CInstantCamera& camera) override;
    void OnOpened(Pylon::CInstantCamera& camera) override;
    void OnClosed(Pylon::CInstantCamera& camera) override;
    void OnGrabStarted(Pylon::CInstantCamera& camera) override;
    void OnGrabStopped(Pylon::CInstantCamera& camera) override;
    void OnGrabError(Pylon::CInstantCamera& camera, const char* errorMessage) override {}
    void OnCameraDeviceRemoved(Pylon::CInstantCamera& camera) override;

    // Pylon::CCameraEventHandler function
    void OnCameraEvent(Pylon::CInstantCamera& camera, intptr_t userProvidedId, GenApi::INode* pNode) override;

};

#endif // CAMERA_H
