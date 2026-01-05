#include "Camera.h"
#include "CameraSystem.h"

Camera::Camera(CameraSystem *parent, int allottedNumber) : _system(parent), _allottedNumber(allottedNumber)
{
    _currentCamera.RegisterConfiguration(this, RegistrationMode_ReplaceAll, Pylon::Cleanup_None);
}

Camera::~Camera()
{
    stop();
    _system->removeCamera(this);
}

void Camera::onCameraStatus(StatusCallback cb)
{
    _scb = std::move(cb);
}

bool Camera::open(string cameraName){
    try{
        CameraSystem::syslog("Try to open " + (cameraName=="" ? "any one of the cameras on this system" : cameraName) + ".");
        _currentCamera.Attach(_system->createDevice(cameraName), Cleanup_Delete);
        _currentCamera.Open();

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
                }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
            }
        }
        return true;
    }catch(const GenericException &e){
CameraSystem::syslog(e.what(),true);
    }
    return false;
}

bool Camera::isOpened()
{
    try{
        return _currentCamera.IsOpen();
    }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
    return false;
}

void Camera::close(){
    try{
        _currentCamera.Close();
        _currentCamera.DetachDevice();
        _currentCamera.DestroyDevice();
    }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
}

size_t Camera::addObserver(GrabCallback cb)
{
    if(!cb) return 0;
    std::lock_guard<std::mutex> lock(_observerMutex);
    size_t id = _nextObserverId++;
    _observers.emplace(id, std::move(cb));
    return id;
}

bool Camera::removeObserver(size_t id)
{
    std::lock_guard<std::mutex> lock(_observerMutex);
    return _observers.erase(id) > 0;
}

void Camera::clearObservers()
{
    std::lock_guard<std::mutex> lock(_observerMutex);
    _observers.clear();
}

// For single callback function
void Camera::onGrabbed(GrabCallback cb)
{
    std::lock_guard<std::mutex> lock(_observerMutex);
    _observers.clear();

    if(cb) _observers.emplace(_nextObserverId++, std::move(cb));
}

void Camera::ready()
{
    _permits.fetch_add(1, std::memory_order_acq_rel);
    _permitCondition.notify_one();
}

void Camera::dispatchToObservers(const CPylonImage &image, size_t frame)
{
    std::vector<GrabCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(_observerMutex);
        cbs.reserve(_observers.size());
        for(auto & kv : _observers){
            cbs.push_back(kv.second);
        }
    }
    for(auto& cb: cbs){
        if(cb) cb(image, frame);
    }
}

void Camera::grab(size_t frame){
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
        _frameTarget.store(frame, std::memory_order_release);
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

                            CPylonImage image;
                            image.AttachGrabResultBuffer(grabResult);

                            auto seq = _frameSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
                            dispatchToObservers(image, seq);

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
            }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
            _isRunning.store(false, std::memory_order_release);
            _permitCondition.notify_all();
        });
    }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
}

void Camera::stop(){
    try{
        _isRunning.store(false, std::memory_order_release);
        _permitCondition.notify_all();

        if(_thread.joinable()) _thread.join();
    }catch(const GenericException &e){ CameraSystem::syslog(e.what(),true); }
}

std::vector<string> Camera::getUpdatedCameraList()
{
    _system->updateCameraList();
    return _system->getCameraList();
}

GenApi::INodeMap &Camera::getNodeMap(){
    return _currentCamera.GetNodeMap();
}

void Camera::onNodeUpdated(NodeCallback cb)
{
    _ncb = std::move(cb);
}

GenApi::INode *Camera::getNode(string name){
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
    if(_scb) _scb(ConnectionStatus, false);
}

void Camera::OnOpened(CInstantCamera &camera){
    _connectedCameraName = camera.GetDeviceInfo().GetFriendlyName();
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " opened.");
    if(_scb) _scb(ConnectionStatus, true);
}

void Camera::OnClosed(CInstantCamera &camera){
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " closed.");
    if(_scb) _scb(ConnectionStatus, false);
}

void Camera::OnCameraDeviceRemoved(CInstantCamera &camera){
    _connectedCameraName = "";
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " removed physically.");
    if(_scb) _scb(ConnectionStatus, false);
}

void Camera::OnGrabStarted(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " started grabbing.");
    if(_scb) _scb(GrabbingStatus, true);
}

void Camera::OnGrabStopped(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + camera.GetDeviceInfo().GetFriendlyName().c_str();
    CameraSystem::syslog(from + " stopped grabbing.");
    if(_scb) _scb(GrabbingStatus, false);
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

    if(_ncb) _ncb(pNode);
}

