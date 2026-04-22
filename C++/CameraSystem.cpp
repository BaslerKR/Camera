#include "CameraSystem.h"

#include <algorithm>
#include <stdexcept>

CameraSystem::CameraSystem(){
    PylonInitialize();
    _tlFactory = &CTlFactory::GetInstance();
}

CameraSystem::~CameraSystem(){
    while(true){
        Camera* camera = nullptr;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if(_cameraList.empty()) break;
            camera = _cameraList.back();
        }
        delete camera;
    }
    PylonTerminate();
}

void CameraSystem::updateCameraListLocked()
{
    _devices.clear();
    auto cnt = _tlFactory->EnumerateDevices(_devices);
    syslog("Updated the camera list: " + to_string(cnt) + " Camera(s) found.");
    for(auto i=0; i<_devices.size(); ++i){
        syslog("-- " + std::string(_devices.at(i).GetFriendlyName()));
    }
}

void CameraSystem::updateCameraList(){
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        updateCameraListLocked();
    }catch(const GenericException &e){
        syslog(e.what(), true);
    }
}

std::vector<string> CameraSystem::getCameraList() {
    std::vector<std::string> list;
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        updateCameraListLocked();
        for(auto &cur : _devices)
            list.emplace_back(cur.GetFriendlyName().c_str());
    }catch(const GenericException &e) {
        syslog(e.what(), true);
    }
    return list;
}

bool CameraSystem::isAccessible(const string &camera){
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        updateCameraListLocked();
        for(const auto& device : _devices){
            if(camera == device.GetFriendlyName().c_str()){
                return _tlFactory->IsDeviceAccessible(device);
            }
        }
    }catch(const GenericException &e){
        syslog(e.what(), true);
    }
    return false;
}

CDeviceInfo CameraSystem::getCameraInfo(const string &cameraName) {
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        updateCameraListLocked();
        syslog("Searching for the device information of " + cameraName + "...");
        for(auto &cur:_devices){
            if(cameraName == cur.GetFriendlyName().c_str()){
                syslog("Matched information found.");
                return cur;
            }
        }
    }catch(const GenericException &e){
        syslog(e.what(), true);
    }
    syslog("No matched information found. ", true);
    return {};
}

Camera *CameraSystem::addCamera()
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto camera = new Camera(this, static_cast<int>(_nextCameraNumber++));
    _cameraList.push_back(camera);

    syslog("New camera instance created.");
    return camera;
}

void CameraSystem::removeCamera(Camera *camera)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _cameraList.erase(std::remove(_cameraList.begin(), _cameraList.end(), camera), _cameraList.end());
    syslog("This camera instance was removed.");
}

Camera *CameraSystem::getCamera(const int allottedNumber) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto iter = std::find_if(_cameraList.begin(), _cameraList.end(), [allottedNumber](const Camera* camera){
        return camera && camera->_allottedNumber == allottedNumber;
    });
    return iter == _cameraList.end() ? nullptr : *iter;
}

IPylonDevice* CameraSystem::createDevice(const string &cameraName)
{
    std::lock_guard<std::mutex> lock(_mutex);
    updateCameraListLocked();

    if(cameraName.empty()){
        for(const auto& device : _devices){
            if(_tlFactory->IsDeviceAccessible(device)){
                return _tlFactory->CreateDevice(device);
            }
        }
        throw std::runtime_error("No accessible camera device found.");
    }

    for(const auto& device : _devices){
        if(cameraName == device.GetFriendlyName().c_str()){
            if(!_tlFactory->IsDeviceAccessible(device)){
                throw std::runtime_error("Camera device is not accessible: " + cameraName);
            }
            return _tlFactory->CreateDevice(device);
        }
    }

    throw std::runtime_error("Camera device not found: " + cameraName);
}

void CameraSystem::syslog(const string &message, const bool warning)
{
    if(!warning) cout << "[Camera System] " << message << endl;
    else cerr << "[Camera System] " << message << endl;
}
