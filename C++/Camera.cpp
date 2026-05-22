#include "Camera.h"
#include "CameraSystem.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <string>

namespace {

template<typename Callback>
using CallbackRegistry = std::unordered_map<size_t, Callback>;

template<typename Callback>
size_t registerCallback(std::mutex& mutex, CallbackRegistry<Callback>& registry, std::atomic<size_t>& nextId, Callback cb)
{
    if(!cb) return 0;

    std::lock_guard<std::mutex> lock(mutex);
    const size_t id = nextId++;
    registry.emplace(id, std::move(cb));
    return id;
}

template<typename Callback>
bool deregisterCallback(std::mutex& mutex, CallbackRegistry<Callback>& registry, size_t id)
{
    std::lock_guard<std::mutex> lock(mutex);
    return registry.erase(id) > 0;
}

template<typename Callback>
void clearCallbacks(std::mutex& mutex, CallbackRegistry<Callback>& registry)
{
    std::lock_guard<std::mutex> lock(mutex);
    registry.clear();
}

template<typename Callback, typename... Args>
void dispatchCallbacks(std::mutex& mutex, CallbackRegistry<Callback>& registry, Args&&... args)
{
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks.reserve(registry.size());
        for(auto& kv : registry){
            callbacks.push_back(kv.second);
        }
    }

    for(auto& cb : callbacks){
        if(cb) cb(std::forward<Args>(args)...);
    }
}

string toLowerCopy(string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch){
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void enableComponent(GenApi::INodeMap& nodeMap, const char* componentName, const char* pixelFormat = nullptr)
{
    auto* componentSelectorNode = nodeMap.GetNode("ComponentSelector");
    auto* componentEnableNode = nodeMap.GetNode("ComponentEnable");
    if(!componentSelectorNode || !componentEnableNode) return;
    if(!GenApi::IsWritable(componentSelectorNode) || !GenApi::IsWritable(componentEnableNode)) return;

    auto componentSelector = Pylon::CEnumParameter(nodeMap, "ComponentSelector");
    auto componentEnable = Pylon::CBooleanParameter(nodeMap, "ComponentEnable");
    componentSelector.SetValue(componentName);
    componentEnable.SetValue(true);

    if(pixelFormat){
        auto* pixelFormatNode = nodeMap.GetNode("PixelFormat");
        if(pixelFormatNode && GenApi::IsWritable(pixelFormatNode)){
            auto pixelFormatParam = Pylon::CEnumParameter(nodeMap, "PixelFormat");
            pixelFormatParam.TrySetValue(pixelFormat);
        }
    }
}

std::string safeCameraName(Pylon::CInstantCamera& camera, const std::string& fallback)
{
    if(!fallback.empty()) return fallback;

    try{
        return camera.GetDeviceInfo().GetFriendlyName().c_str();
    }catch(const Pylon::GenericException&){
        return {};
    }
}

}

Camera::Camera(CameraSystem *parent, const int allottedNumber) : _system(parent), _allottedNumber(allottedNumber)
{
    _currentCamera.RegisterConfiguration(this, RegistrationMode_ReplaceAll, Pylon::Cleanup_None);
}

Camera::~Camera()
{
    close();
}

Camera::CallbackId Camera::registerStatusCallback(StatusCallback cb)
{
    return registerCallback(_statusMutex, _statusObservers, _nextStatusObserverId, std::move(cb));
}

bool Camera::deregisterStatusCallback(CallbackId id)
{
    return deregisterCallback(_statusMutex, _statusObservers, id);
}

void Camera::clearStatusCallbacks()
{
    clearCallbacks(_statusMutex, _statusObservers);
}

bool Camera::open(const string& cameraName){
    try{
        _deviceAvailable.store(false, std::memory_order_release);
        clearNodeEventHandlers();
        CameraSystem::syslog("Try to open " + (cameraName.empty() ? "any one of the cameras on this system" : cameraName) + ".");
        _currentCamera.Attach(_system->createDevice(cameraName), Cleanup_Delete);
        _currentCamera.Open();
        if(_currentCamera.IsOpen()){
            markOpened(_currentCamera);
        }
        configureStreamForConnectedCamera();
        registerNodeEventHandlers();
        return true;
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(), true);
    }catch(const std::exception &e){
        CameraSystem::syslog(e.what(), true);
    }
    return false;
}

bool Camera::isOpened() const {
    try{
        return _deviceAvailable.load(std::memory_order_acquire) && _currentCamera.IsOpen();
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
    return false;
}

void Camera::close(){
    try{
        stop();
        _deviceAvailable.store(false, std::memory_order_release);
        _streamKind.store(StreamKind::Image2D, std::memory_order_release);
        if(_currentCamera.IsOpen()){
            _currentCamera.Close();
        }
        _currentCamera.DeregisterConfiguration(this);
        clearNodeEventHandlers();
        if(_currentCamera.IsPylonDeviceAttached()){
            _currentCamera.DestroyDevice();
        }
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
}

Camera::CallbackId Camera::registerGrabCallback(GrabCallback cb)
{
    return registerCallback(_grabCallbackMutex, _grabCallbacks, _nextGrabCallbackId, std::move(cb));
}

bool Camera::deregisterGrabCallback(const CallbackId id)
{
    return deregisterCallback(_grabCallbackMutex, _grabCallbacks, id);
}

void Camera::clearGrabCallbacks()
{
    clearCallbacks(_grabCallbackMutex, _grabCallbacks);
}

Camera::CallbackId Camera::registerGrab3DCallback(Grab3DCallback cb)
{
    return registerCallback(_grab3DCallbackMutex, _grab3DCallbacks, _nextGrab3DCallbackId, std::move(cb));
}

bool Camera::deregisterGrab3DCallback(const CallbackId id)
{
    return deregisterCallback(_grab3DCallbackMutex, _grab3DCallbacks, id);
}

void Camera::clearGrab3DCallbacks()
{
    clearCallbacks(_grab3DCallbackMutex, _grab3DCallbacks);
}

void Camera::ready()
{
    _permits.fetch_add(1, std::memory_order_acq_rel);
    _permitCondition.notify_one();
}

void Camera::configureStreamForConnectedCamera()
{
    _streamKind.store(StreamKind::Image2D, std::memory_order_release);

    auto& nodeMap = _currentCamera.GetNodeMap();
    const auto modelName = toLowerCopy(_currentCamera.GetDeviceInfo().GetModelName().c_str());

    if(modelName.find("blaze") != string::npos){
        configureBlazeStream(nodeMap);
    }else if(modelName.find("stereo ace") != string::npos || modelName.find("sta") != string::npos){
        configureStereoAceStream(nodeMap);
    }

    const auto streamKind = _streamKind.load(std::memory_order_acquire);
    const auto* routeText = streamKind == StreamKind::MultiPart3D ? "3D-only" : "2D";
    CameraSystem::syslog("[Info " + to_string(_allottedNumber) + "] model="
                         + modelName
                         + " route=" + routeText);
}

void Camera::configureBlazeStream(GenApi::INodeMap& nodeMap)
{
    _streamKind.store(StreamKind::MultiPart3D, std::memory_order_release);
    enableComponent(nodeMap, "Range", "Coord3D_ABC32f");
    enableComponent(nodeMap, "Intensity", "Mono16");
    enableComponent(nodeMap, "Confidence", "Confidence16");

    auto* coordinateSelectorNode = _currentCamera.Scan3dCoordinateSelector.GetNode();
    auto* invalidDataValueNode = _currentCamera.Scan3dInvalidDataValue.GetNode();
    if(coordinateSelectorNode && invalidDataValueNode &&
       GenApi::IsWritable(coordinateSelectorNode) &&
       GenApi::IsWritable(invalidDataValueNode)){
        for(const auto* axis : {"CoordinateA", "CoordinateB", "CoordinateC"}){
            _currentCamera.Scan3dCoordinateSelector.SetValue(axis);
            _currentCamera.Scan3dInvalidDataValue.SetValue(std::numeric_limits<float>::quiet_NaN());
        }
    }
}

void Camera::configureStereoAceStream(GenApi::INodeMap& nodeMap)
{
    _streamKind.store(StreamKind::MultiPart3D, std::memory_order_release);
    enableComponent(nodeMap, "Intensity");
    enableComponent(nodeMap, "Disparity");
}

void Camera::grab(const size_t frames){
    try{
        if(!isOpened()) return;
        if(_isRunning.load(std::memory_order_acquire)) return;
        if(_thread.joinable()) _thread.join();

        bool triggerMode = _currentCamera.TriggerMode.GetValue() == Basler_UniversalCameraParams::TriggerModeEnums::TriggerMode_On;
        if(triggerMode){
            _currentCamera.MaxNumBuffer = 30;
            _currentCamera.StartGrabbing(GrabStrategy_OneByOne, GrabLoop_ProvidedByUser);
        }else{
            _currentCamera.MaxNumBuffer = 5;
            _currentCamera.StartGrabbing(GrabStrategy_LatestImageOnly, GrabLoop_ProvidedByUser);
        }

        _isRunning.store(true, std::memory_order_release);
        _frameTarget.store(frames, std::memory_order_release);
        _frameSeq.store(0, std::memory_order_release);
        _permits.store(1, std::memory_order_release);

        _thread = std::thread([=]{
            try{
                CGrabResultPtr grabResult;
                size_t delivered = 0;

                while(_isRunning.load(std::memory_order_acquire) && _deviceAvailable.load(std::memory_order_acquire) && _currentCamera.IsGrabbing()){
                    if(_currentCamera.RetrieveResult(1000, grabResult, Pylon::TimeoutHandling_Return)){
                        if(grabResult->GrabSucceeded()){
                            if(!triggerMode){
                                std::unique_lock<std::mutex> lock(_permitMutex);
                                _permitCondition.wait(lock, [this]{
                                    return !_isRunning.load(std::memory_order_acquire) || _permits.load(std::memory_order_acquire) > 0;
                                });

                                if (!_isRunning.load(std::memory_order_acquire)) break;
                                _permits.fetch_sub(1, std::memory_order_acq_rel);
                            }

                            auto seq = _frameSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
                            if(_streamKind.load(std::memory_order_acquire) == StreamKind::MultiPart3D){
                                auto container = grabResult->GetDataContainer();
                                dispatchCallbacks(_grab3DCallbackMutex, _grab3DCallbacks, container, seq);
                            }else{
                                CPylonImage image;
                                image.AttachGrabResultBuffer(grabResult);
                                dispatchCallbacks(_grabCallbackMutex, _grabCallbacks, image, seq);
                            }

                            auto target = _frameTarget.load(std::memory_order_acquire);
                            if(target !=0 && ++delivered >= target){
                                _isRunning.store(false, std::memory_order_release);
                                _permitCondition.notify_all();
                                break;
                            }
                        }
                    }
                }
                if(_currentCamera.IsGrabbing()) _currentCamera.StopGrabbing();
            }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
            _isRunning.store(false, std::memory_order_release);
            _permitCondition.notify_all();
        });
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
}

void Camera::stop(){
    try{
        requestStop();

        if(_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) _thread.join();
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
}

void Camera::requestStop()
{
    _isRunning.store(false, std::memory_order_release);
    _permitCondition.notify_all();
}

std::vector<string> Camera::getUpdatedCameraList() const {
    return _system->getCameraList();
}

GenApi::INodeMap &Camera::getNodeMap(){
    return _currentCamera.GetNodeMap();
}

Camera::CallbackId Camera::registerNodeUpdatedCallback(NodeCallback cb)
{
    return registerCallback(_nodeCallbackMutex, _nodeCallbacks, _nextNodeCallbackId, std::move(cb));
}

bool Camera::deregisterNodeUpdatedCallback(const CallbackId id)
{
    return deregisterCallback(_nodeCallbackMutex, _nodeCallbacks, id);
}

void Camera::clearNodeUpdatedCallbacks()
{
    clearCallbacks(_nodeCallbackMutex, _nodeCallbacks);
}

void Camera::OnAttached(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, _connectedCameraName);
    CameraSystem::syslog(from + " attached.");
}

void Camera::OnDetached(CInstantCamera &camera){
    _deviceAvailable.store(false, std::memory_order_release);
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Detached.");
}

void Camera::OnDestroyed(CInstantCamera &camera){
    requestStop();
    _deviceAvailable.store(false, std::memory_order_release);
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Device destroyed.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnOpened(CInstantCamera &camera){
    markOpened(camera);
}

void Camera::OnClosed(CInstantCamera &camera){
    _deviceAvailable.store(false, std::memory_order_release);
    const auto cameraName = safeCameraName(camera, _connectedCameraName);
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + cameraName;
    CameraSystem::syslog(from + " closed.");
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnCameraDeviceRemoved(CInstantCamera &camera){
    requestStop();
    _deviceAvailable.store(false, std::memory_order_release);
    const auto cameraName = safeCameraName(camera, _connectedCameraName);
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + cameraName;
    CameraSystem::syslog(from + " removed physically.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnGrabStarted(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, _connectedCameraName);
    CameraSystem::syslog(from + " started grabbing.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, true);
}

void Camera::OnGrabStopped(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, _connectedCameraName);
    CameraSystem::syslog(from + " stopped grabbing.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
}

void Camera::OnCameraEvent(CInstantCamera &camera, intptr_t userProvidedId, GenApi::INode *pNode){
    if(!pNode || !_deviceAvailable.load(std::memory_order_acquire)) return;

    std::string nodeName;

    try{
        nodeName = pNode->GetName().c_str();
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(), true);
        return;
    }catch(const std::exception &e){
        CameraSystem::syslog(e.what(), true);
        return;
    }

    if(!nodeName.empty()){
        dispatchCallbacks(_nodeCallbackMutex, _nodeCallbacks, nodeName);
    }
}

void Camera::markOpened(CInstantCamera& camera)
{
    _connectedCameraName = safeCameraName(camera, {});
    const bool wasAvailable = _deviceAvailable.exchange(true, std::memory_order_acq_rel);
    if(wasAvailable) return;

    auto from = "[Info " + to_string(_allottedNumber)  +"] " + _connectedCameraName;
    CameraSystem::syslog(from + " opened.");
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, true);
}

void Camera::registerNodeEventHandlers()
{
    clearNodeEventHandlers();

    GenApi::NodeList_t nodes;
    _currentCamera.GetNodeMap().GetNodes(nodes);
    for(const auto cur : nodes){
        if(cur->GetName() == "Root") continue;
        if(cur->GetPrincipalInterfaceType() != GenApi::intfICategory) continue;
        if(!GenApi::IsAvailable(cur)) continue;

        GenApi::NodeList_t children;
        cur->GetChildren(children);

        for(const auto child : children){
            if(!GenApi::IsReadable(child)) continue;

            try{
                std::string nodeName = child->GetName().c_str();
                if(std::find(_registeredNodeEventNames.begin(), _registeredNodeEventNames.end(), nodeName) != _registeredNodeEventNames.end()) continue;

                _currentCamera.RegisterCameraEventHandler(this,
                                                         nodeName.c_str(),
                                                         _allottedNumber,
                                                         ERegistrationMode::RegistrationMode_Append,
                                                         ECleanup::Cleanup_None,
                                                         CameraEventAvailability_Optional);
                _registeredNodeEventNames.push_back(std::move(nodeName));
            }catch(const GenericException &e){
                CameraSystem::syslog(e.GetDescription(), true);
            }
        }
    }
}

void Camera::clearNodeEventHandlers()
{
    for(const auto& nodeName : _registeredNodeEventNames){
        _currentCamera.DeregisterCameraEventHandler(this, nodeName.c_str());
    }
    _registeredNodeEventNames.clear();
}

