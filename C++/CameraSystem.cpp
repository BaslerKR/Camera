#include "CameraSystem.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

CameraSystem::CameraSystem() : CameraSystem(DeviceEnumerator{}) {}

CameraSystem::CameraSystem(DeviceEnumerator deviceEnumerator)
    : _deviceEnumerator(std::move(deviceEnumerator)){
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
            _cameraList.pop_back();
        }
        delete camera;
    }
    // Device-info strings are SDK-owned objects; release them while pylon is alive.
    _devices.clear();
    _tlFactory = nullptr;
    try{
        PylonTerminate();
    }catch(const GenericException &e){
        syslog(e.GetDescription(), true);
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception while terminating pylon", true);
    }
}

void CameraSystem::updateCameraListLocked(bool report)
{
    _devices.clear();
    std::size_t count = 0;
    if (_deviceEnumerator) {
        _deviceEnumerator(_devices);
        count = _devices.size();
    } else {
        count = _tlFactory->EnumerateDevices(_devices);
    }
    if (report) {
        syslog("Updated the camera list: " + to_string(count) + " Camera(s) found.");
        for(std::size_t i = 0; i < _devices.size(); ++i){
            syslog("-- " + std::string(_devices.at(i).GetFriendlyName()));
        }
    }
}

void CameraSystem::updateCameraList(){
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        updateCameraListLocked();
    }catch(const GenericException &e){
        syslog(e.what(), true);
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception in updateCameraList", true);
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
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception in getCameraList", true);
    }
    return list;
}

std::vector<string> CameraSystem::getCachedCameraList() const {
    std::vector<std::string> list;
    try{
        std::lock_guard<std::mutex> lock(_mutex);
        for(const auto &cur : _devices)
            list.emplace_back(cur.GetFriendlyName().c_str());
    }catch(const GenericException &e) {
        syslog(e.what(), true);
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception in getCachedCameraList", true);
    }
    return list;
}

std::vector<CameraDeviceDescriptor> CameraSystem::deviceCatalog(bool refresh) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (refresh) updateCameraListLocked();
    return deviceCatalogLocked();
}

std::vector<CameraDeviceDescriptor> CameraSystem::discoverInitialCatalog(
    std::chrono::milliseconds firstDeviceWait, const std::function<bool()>& interrupted) {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto deadline = std::chrono::steady_clock::now() + firstDeviceWait;
    const auto retryInterval = std::chrono::milliseconds{250};
    for (;;) {
        if (interrupted && interrupted()) break;
        updateCameraListLocked(false);
        if (!_devices.empty() || (interrupted && interrupted())) break;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) break;
        std::this_thread::sleep_for(std::min(remaining, retryInterval));
    }
    syslog("Completed initial camera discovery with " + to_string(_devices.size()) + " Camera(s).");
    for (const auto& device : _devices) syslog("-- " + std::string(device.GetFriendlyName()));
    return deviceCatalogLocked();
}

std::vector<CameraDeviceDescriptor> CameraSystem::deviceCatalogLocked() const {
    std::vector<CameraDeviceDescriptor> result;
    for (const auto& device : _devices) {
        result.push_back({device.GetFullName().c_str(), device.GetFriendlyName().c_str(),
            device.GetSerialNumber().c_str()});
    }
    return result;
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
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception in isAccessible", true);
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
    }catch(const std::exception &e){
        syslog(e.what(), true);
    }catch(...){
        syslog("Unknown exception in getCameraInfo", true);
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
    if (!camera) return;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::remove(_cameraList.begin(), _cameraList.end(), camera);
        if (it != _cameraList.end()) {
            _cameraList.erase(it, _cameraList.end());
            removed = true;
        }
    }
    if (!removed) return;

    delete camera;
    syslog("This camera instance was removed.");
}

Camera *CameraSystem::getCamera(const int allottedNumber) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto iter = std::find_if(_cameraList.begin(), _cameraList.end(), [allottedNumber](const Camera* camera){
        return camera && camera->_allottedNumber == allottedNumber;
    });
    return iter == _cameraList.end() ? nullptr : *iter;
}

IPylonDevice* CameraSystem::createDevice(const string &cameraName, bool exactIdentity)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if(cameraName.empty()){
        for(const auto& device : _devices){
            if(_tlFactory->IsDeviceAccessible(device)){
                return _tlFactory->CreateDevice(device);
            }
        }
        throw std::runtime_error("No cached accessible camera device found. Refresh the camera list first.");
    }

    for(const auto& device : _devices){
        if(cameraName == device.GetFullName().c_str()){
            if(!_tlFactory->IsDeviceAccessible(device)) throw std::runtime_error("Camera device is not accessible: " + cameraName);
            return _tlFactory->CreateDevice(device);
        }
    }
    if (exactIdentity) throw std::runtime_error("Exact camera identity is no longer in the cached catalog: " + cameraName);
    for(const auto& device : _devices){
        if(cameraName == device.GetFriendlyName().c_str()){
            if(!_tlFactory->IsDeviceAccessible(device)){
                throw std::runtime_error("Camera device is not accessible: " + cameraName);
            }
            return _tlFactory->CreateDevice(device);
        }
    }

    throw std::runtime_error("Camera device not found in cached list: " + cameraName);
}

void CameraSystem::syslog(const string &message, const bool warning)
{
    if(!warning) cout << "[Camera System] " << message << endl;
    else cerr << "[Camera System] " << message << endl;
}
