#include "Camera.h"
#include "CameraSystem.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

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

string componentTypeToString(const Pylon::EComponentType componentType)
{
    switch(componentType){
    case Pylon::ComponentType_Undefined:
        return "Undefined";
    case Pylon::ComponentType_Intensity:
        return "Intensity";
    case Pylon::ComponentType_Confidence:
        return "Confidence";
    case Pylon::ComponentType_Disparity:
        return "Disparity";
    case Pylon::ComponentType_Range:
        return "Range";
    case Pylon::ComponentType_Reflectance:
        return "Reflectance";
    case Pylon::ComponentType_Scatter:
        return "Scatter";
    case Pylon::ComponentType_IntensityCombined_STA:
        return "IntensityCombined_STA";
    case Pylon::ComponentType_Error_STA:
        return "Error_STA";
    case Pylon::ComponentType_RawCombined_STA:
        return "RawCombined_STA";
    case Pylon::ComponentType_Calibration_STA:
        return "Calibration_STA";
    }
    return "Unknown";
}

void log3DComponentLayout(const Pylon::CPylonDataContainer& container, int allottedNumber, const string& cameraName)
{
    std::ostringstream oss;
    oss << "[Info " << allottedNumber << "] " << cameraName << " 3D components:";

    for(size_t i = 0; i < container.GetDataComponentCount(); ++i){
        const auto component = container.GetDataComponent(i);
        if(!component.IsValid()) continue;

        oss << " ["
            << i
            << ": type=" << componentTypeToString(component.GetComponentType())
            << ", pixel=" << Pylon::CPixelTypeMapper::GetNameByPixelType(component.GetPixelType())
            << ", " << component.GetWidth() << "x" << component.GetHeight()
            << "]";
    }

    CameraSystem::syslog(oss.str());
}

}

Camera::Camera(CameraSystem *parent, const int allottedNumber) : _system(parent), _allottedNumber(allottedNumber)
{
    _currentCamera.RegisterConfiguration(this, RegistrationMode_ReplaceAll, Pylon::Cleanup_None);
}

Camera::~Camera()
{
    stop();
    _system->removeCamera(this);
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
        CameraSystem::syslog("Try to open " + (cameraName.empty() ? "any one of the cameras on this system" : cameraName) + ".");
        _currentCamera.Attach(_system->createDevice(cameraName), Cleanup_Delete);
        _currentCamera.Open();
        _logged3DComponentLayout = false;

        auto& nodeMap = _currentCamera.GetNodeMap();
        const auto modelName = toLowerCopy(_currentCamera.GetDeviceInfo().GetModelName().c_str());

        if(modelName.find("blaze") != string::npos){
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
        }else if(modelName.find("stereo ace") != string::npos || modelName.find("sta") != string::npos){
            enableComponent(nodeMap, "Intensity");
            enableComponent(nodeMap, "Disparity");
        }

        // Registering event handlers
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
                    String_t nodeName = child->GetName();
                    _currentCamera.RegisterCameraEventHandler(this, nodeName, _allottedNumber, ERegistrationMode::RegistrationMode_Append, ECleanup::Cleanup_None);
                }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
            }
        }
        return true;
    }catch(const GenericException &e){
CameraSystem::syslog(e.GetDescription(),true);
    }
    return false;
}

bool Camera::isOpened() const {
    try{
        return _currentCamera.IsOpen();
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
    return false;
}

void Camera::close(){
    try{
        _logged3DComponentLayout = false;
        _currentCamera.Close();
        _currentCamera.DetachDevice();
        _currentCamera.DestroyDevice();
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

void Camera::dispatchToObservers(const CPylonImage &image, size_t frame)
{
    dispatchCallbacks(_grabCallbackMutex, _grabCallbacks, image, frame);
}

void Camera::dispatchTo3DObservers(const Pylon::CPylonDataContainer &container, size_t frame)
{
    dispatchCallbacks(_grab3DCallbackMutex, _grab3DCallbacks, container, frame);
}

void Camera::dispatchStatus(Status status, bool on)
{
    dispatchCallbacks(_statusMutex, _statusObservers, status, on);
}

bool Camera::has3DComponents(const Pylon::CGrabResultPtr &grabResult) const
{
    if(!grabResult.IsValid()) return false;

    try{
        const auto container = grabResult->GetDataContainer();
        const auto count = container.GetDataComponentCount();
        for(size_t i = 0; i < count; ++i){
            const auto component = container.GetDataComponent(i);
            if(!component.IsValid()) continue;

            switch(component.GetComponentType()){
            case Pylon::ComponentType_Intensity:
            case Pylon::ComponentType_Confidence:
            case Pylon::ComponentType_Disparity:
            case Pylon::ComponentType_Range:
            case Pylon::ComponentType_Reflectance:
            case Pylon::ComponentType_Scatter:
            case Pylon::ComponentType_IntensityCombined_STA:
            case Pylon::ComponentType_Error_STA:
            case Pylon::ComponentType_RawCombined_STA:
            case Pylon::ComponentType_Calibration_STA:
                return true;
            case Pylon::ComponentType_Undefined:
                break;
            }
        }
    }catch(const GenericException &){
        return false;
    }

    return false;
}

void Camera::grab(const size_t frames){
    try{
        if(!_currentCamera.IsOpen()) return;
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

                while(_isRunning.load(std::memory_order_acquire) && _currentCamera.IsGrabbing()){
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
                            if(has3DComponents(grabResult)){
                                auto container = grabResult->GetDataContainer();
                                if(!_logged3DComponentLayout){
                                    log3DComponentLayout(container, _allottedNumber, _currentCamera.GetDeviceInfo().GetFriendlyName().c_str());
                                    _logged3DComponentLayout = true;
                                }
                                dispatchTo3DObservers(container, seq);
                            }else{
                                CPylonImage image;
                                image.AttachGrabResultBuffer(grabResult);
                                dispatchToObservers(image, seq);
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
        _isRunning.store(false, std::memory_order_release);
        _permitCondition.notify_all();

        if(_thread.joinable()) _thread.join();
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
}

std::vector<string> Camera::getUpdatedCameraList() const {
    _system->updateCameraList();
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

GenApi::INode *Camera::getNode(const string &name){
    return _currentCamera.GetNodeMap().GetNode(name.c_str());
}

void Camera::OnAttached(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " attached.");
}

void Camera::OnDetached(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Detached.");
}

void Camera::OnDestroyed(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Device destroyed.");
    dispatchStatus(ConnectionStatus, false);
}

void Camera::OnOpened(CInstantCamera &camera){
    _connectedCameraName = camera.GetDeviceInfo().GetFriendlyName();
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " opened.");
    dispatchStatus(ConnectionStatus, true);
}

void Camera::OnClosed(CInstantCamera &camera){
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " closed.");
    dispatchStatus(ConnectionStatus, false);
}

void Camera::OnCameraDeviceRemoved(CInstantCamera &camera){
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " removed physically.");
    dispatchStatus(ConnectionStatus, false);
}

void Camera::OnGrabStarted(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " started grabbing.");
    dispatchStatus(GrabbingStatus, true);
}

void Camera::OnGrabStopped(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " stopped grabbing.");
    dispatchStatus(GrabbingStatus, false);
}

void Camera::OnCameraEvent(CInstantCamera &camera, intptr_t userProvidedId, GenApi::INode *pNode){
    using namespace GenApi;
    std::string output = std::string("[Event ") + std::to_string(_allottedNumber) + "] ";

    switch(pNode->GetPrincipalInterfaceType()){
    case GenApi::intfIInteger:
        output += std::string(pNode->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + to_string(CIntegerPtr(pNode)->GetValue());
        break;
    case GenApi::intfIBoolean:
        output += std::string(pNode->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + to_string(CBooleanPtr(pNode)->GetValue());
        break;
    case GenApi::intfIFloat:
        output += std::string(pNode->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + to_string(CFloatPtr(pNode)->GetValue());
        break;
    case GenApi::intfIString:
        output += std::string(pNode->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + CStringPtr(pNode)->GetValue().c_str();
        break;
    case GenApi::intfIEnumeration:
        output += std::string(pNode->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + CEnumerationPtr(pNode)->GetCurrentEntry()->GetNode()->GetDisplayName().c_str();
        break;
    case GenApi::intfICommand:
        output += std::string(CCommandPtr(pNode)->GetNode()->GetDisplayName().c_str()) + " ( " + pNode->GetName().c_str() +" ) : " + CCommandPtr(pNode)->ToString().c_str();
        break;
    case GenApi::intfIRegister:
        output += CRegisterPtr(pNode)->GetNode()->GetDisplayName().c_str() + CRegisterPtr(pNode)->GetAddress();
        break;
    case GenApi::intfICategory:
        // We are going to ignore the category events due to the meaningless data for users.
        return;
    case GenApi::intfIEnumEntry:
    case GenApi::intfIPort:
    case GenApi::intfIValue:
    case GenApi::intfIBase:
        break;
    }
    CameraSystem::syslog(output);

    dispatchCallbacks(_nodeCallbackMutex, _nodeCallbacks, pNode);
}

