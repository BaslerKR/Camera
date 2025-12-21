#ifndef CAMERA_H
#define CAMERA_H
#include <pylon/PylonIncludes.h>
#include <pylon/BaslerUniversalInstantCamera.h>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>

using namespace Pylon;
using namespace std;

class CameraSystem;
class Camera : public Pylon::CConfigurationEventHandler,
               public Pylon::CCameraEventHandler
{
public:
    Camera(CameraSystem* parent, int allottedNumber=0);
    ~Camera();

    enum Status{
        GrabbingStatus,
        ConnectionStatus
    };
    using StatusCallback = std::function<void(Status status, bool on)>;
    void onCameraStatus(StatusCallback cb);

    bool open(std::string cameraName="");
    bool isOpened();
    void close();
    std::string getConnectedCameraName(){ return _connectedCameraName; }

    using GrabCallback = std::function<void(const CPylonImage&, size_t frame)>;
    void onGrabbed(GrabCallback cb);

    size_t addObserver(GrabCallback cb);
    bool removeObserver(size_t id);
    void clearObservers();
    void ready();

    void dispatchToObservers(const CPylonImage& image, size_t frame);

    void grab(size_t frames=0);
    void stop();

    std::vector<std::string> getUpdatedCameraList();
    GenApi::INode* getNode(std::string name);
    GenApi::INodeMap& getNodeMap();

    using NodeCallback = std::function<void(GenApi::INode*)>;
    void onNodeUpdated(NodeCallback cb);

    CameraSystem* getSystem(){ return _system; }

private:
    CameraSystem *_system;
    CBaslerUniversalInstantCamera _currentCamera;
    std::string _connectedCameraName = "";
    int _allottedNumber = 0;

    std::thread _thread;
    std::atomic<bool> _isRunning=false;

    StatusCallback _scb;
    NodeCallback _ncb;

    std::mutex _observerMutex;
    std::unordered_map<size_t, GrabCallback> _observers;
    std::atomic<size_t> _nextObserverId{1};

    std::mutex _permitMutex;
    std::condition_variable _permitCondition;
    std::atomic<int> _permits{0};

    std::atomic<size_t> _frameSeq{0};
    std::atomic<size_t> _frameTarget{0};


protected:
    // Pylon::CConfigurationEventHandler functions
    virtual void OnAttached(Pylon::CInstantCamera& camera);
    virtual void OnDetached(Pylon::CInstantCamera& camera);
    virtual void OnDestroyed(Pylon::CInstantCamera& camera);
    virtual void OnOpened(Pylon::CInstantCamera& camera);
    virtual void OnClosed(Pylon::CInstantCamera& camera);
    virtual void OnGrabStarted(Pylon::CInstantCamera& camera);
    virtual void OnGrabStopped(Pylon::CInstantCamera& camera);
    virtual void OnGrabError(Pylon::CInstantCamera& camera, const char* errorMessage){}
    virtual void OnCameraDeviceRemoved(Pylon::CInstantCamera& camera);

    // Pylon::CCameraEventHandler function
    virtual void OnCameraEvent(Pylon::CInstantCamera& camera, intptr_t userProvidedId, GenApi::INode* pNode);

};

#endif // CAMERA_H
