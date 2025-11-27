#include "Camera.h"
#include "CameraSystem.h"

Camera::Camera(CameraSystem *parent, int allottedNumber) : _system(parent), _allottedNumber(allottedNumber)
{
    _currentCamera.RegisterConfiguration(this, RegistrationMode_ReplaceAll, Pylon::Cleanup_None);
}

Camera::~Camera()
{
}

bool Camera::open(string cameraName){
    try{

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
                }catch(const GenericException &e){ cerr << e.what() << endl; }
            }
        }

        return true;
    }catch(const GenericException &e){ cerr << e.what() << endl; }
    return false;
}

bool Camera::isOpened()
{
    try{
        return _currentCamera.IsOpen();
    }catch(const GenericException &e){ cerr << e.what() << endl; }
    return false;
}

void Camera::close(){
    try{
        _currentCamera.Close();
    }catch(const GenericException &e){ cerr << e.what() << endl; }
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

        _currentCamera.MaxNumBuffer = 5;
        _currentCamera.StartGrabbing(GrabStrategy_LatestImageOnly, GrabLoop_ProvidedByUser);

        _isRunning.store(true, std::memory_order_release);
        _frameTarget.store(frame, std::memory_order_release);
        _frameSeq.store(0, std::memory_order_release);
        _permits.store(1, std::memory_order_release);

        _thread = std::thread([this]{
            try{
                CGrabResultPtr grabResult;
                size_t delivered = 0;

                while(_isRunning.load(std::memory_order_acquire) && _currentCamera.IsGrabbing()){
                    // Waiting the call of ready()
                    {
                        std::unique_lock<std::mutex> lock(_permitMutex);
                        _permitCondition.wait(lock, [this]{
                            return !_isRunning.load(std::memory_order_acquire) || _permits.load(std::memory_order_acquire) > 0;
                        });

                        if (!_isRunning.load(std::memory_order_acquire)) break;
                        _permits.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    if(_currentCamera.RetrieveResult(1000, grabResult, TimeoutHandling_ThrowException)){
                        if(grabResult->GrabSucceeded()){
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
            }catch(const GenericException &e){ cerr << e.what() << endl; }

            _isRunning.store(false, std::memory_order_release);
            _permitCondition.notify_all();
        });
    }catch(const GenericException &e){ cerr << e.what() << endl; }
}

void Camera::stop(){
    try{
        _isRunning.exchange(false, std::memory_order_acq_rel);
        if(_currentCamera.IsGrabbing()) _currentCamera.StopGrabbing();
        if(_thread.joinable()) _thread.join();
    }catch(const GenericException &e){ cerr << e.what() << endl; }
}

std::vector<string> Camera::getUpdatedCameraList()
{
    _system->updateCameraList();
    return _system->getCameraList();
}

GenApi::INodeMap &Camera::getNodeMap(){
    return _currentCamera.GetNodeMap();
}

void Camera::onConfiguration(NodeCallback cb)
{
    _ncb = std::move(cb);
}

GenApi::INode *Camera::getNode(string name){
    return _currentCamera.GetNodeMap().GetNode(name.c_str());
}

void Camera::OnAttached(CInstantCamera &camera){
    cout << "[Info " + to_string(_allottedNumber)  +"] " << camera.GetDeviceInfo().GetFriendlyName() + " is attached." << endl;
}

void Camera::OnOpened(CInstantCamera &camera){
    cout << "[Info " + to_string(_allottedNumber)  +"] " << camera.GetDeviceInfo().GetFriendlyName() + " is opened." << endl;
}

void Camera::OnGrabStarted(CInstantCamera &camera){
    cout << "[Info " + to_string(_allottedNumber)  +"] " << camera.GetDeviceInfo().GetFriendlyName() << " starts grabbing." << endl;
}

void Camera::OnGrabStopped(CInstantCamera &camera){
    cout << "[Info " + to_string(_allottedNumber)  +"] " << camera.GetDeviceInfo().GetFriendlyName() << " stopped grabbing." << endl;
}

void Camera::OnCameraEvent(CInstantCamera &camera, intptr_t userProvidedId, GenApi::INode *pNode){
    // 여기서 받은 데이터를 Camera Widget에 넘길 수 있어야 한다..
    using namespace GenApi;
    std::string output = std::string("[Event] ") + camera.GetDeviceInfo().GetFriendlyName().c_str() + " - ";
    switch(pNode->GetPrincipalInterfaceType()){
    case GenApi_3_1_Basler_pylon_v3::intfIInteger:
        output += std::string(pNode->GetDisplayName().c_str()) + ": " + to_string(CIntegerPtr(pNode)->GetValue());
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIBoolean:
        output += std::string(pNode->GetDisplayName().c_str()) + ": " + to_string(CBooleanPtr(pNode)->GetValue());
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIFloat:
        output += std::string(pNode->GetDisplayName().c_str()) + ": " + to_string(CFloatPtr(pNode)->GetValue());
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIString:
        output += std::string(pNode->GetDisplayName().c_str()) + ": " + CStringPtr(pNode)->GetValue().c_str();
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIEnumeration:
        output += std::string(pNode->GetDisplayName().c_str()) + ": " + CEnumerationPtr(pNode)->GetCurrentEntry()->GetNode()->GetDisplayName().c_str();
        break;
    case GenApi_3_1_Basler_pylon_v3::intfICommand:
        output += std::string(CCommandPtr(pNode)->GetNode()->GetDisplayName().c_str()) + ": " + CCommandPtr(pNode)->ToString().c_str();
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIRegister:
        output += CRegisterPtr(pNode)->GetNode()->GetDisplayName().c_str() + CRegisterPtr(pNode)->GetAddress();
        break;
    case GenApi_3_1_Basler_pylon_v3::intfIEnumEntry:
        cout << "enum entry" << endl; break;
    case GenApi_3_1_Basler_pylon_v3::intfICategory:
        cout << "category" << endl; break;
    case GenApi_3_1_Basler_pylon_v3::intfIPort:
        cout << "port" << endl; break;
    case GenApi_3_1_Basler_pylon_v3::intfIValue:
        cout << "ivalue" << endl; break;
    case GenApi_3_1_Basler_pylon_v3::intfIBase:
        cout << "Another case occurred." << endl;
        break;
    }
    cout << output << endl;
    if(_ncb) _ncb(pNode);
}

