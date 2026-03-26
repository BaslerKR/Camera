#include "CameraSystem.h"

CameraSystem::CameraSystem(){
    PylonInitialize();
    _tlFactory = &CTlFactory::GetInstance();
}

CameraSystem::~CameraSystem(){
    PylonTerminate();
}

void CameraSystem::updateCameraList(){
    try{
        _cameraList.clear();
        auto cnt = _tlFactory->EnumerateDevices(_devices);
        syslog("Updated the camera list: " + to_string(cnt) + " Camera(s) found.");
        for(auto i=0; i<_devices.size(); ++i){
            syslog("-- " + std::string(_devices.at(i).GetFriendlyName()));
        }
        // Signal needed to interface a UI class, sending to camera classes
    }catch(const GenericException &e){
        syslog(e.what(), true);
    }
}

std::vector<string> CameraSystem::getCameraList() {
    std::vector<std::string> list;
    try{
        for(auto &cur : _devices)
            list.emplace_back(cur.GetFriendlyName().c_str());
    }catch(const GenericException &e) {
        syslog(e.what(), true);
    }
    return list;
}

bool CameraSystem::isAccessible(const string &camera){
    return _tlFactory->IsDeviceAccessible(getCameraInfo(camera));
}

CDeviceInfo CameraSystem::getCameraInfo(const string &cameraName) {
    try{
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
    auto camera = new Camera(this, _cameraList.size());
    _cameraList.push_back(camera);

    syslog("New camera instance created.");
    return camera;
}

void CameraSystem::removeCamera(Camera *camera)
{
    _cameraList.erase(std::remove(_cameraList.begin(), _cameraList.end(), camera), _cameraList.end());
    syslog("This camera instance was removed.");

    updateCameraList();
}

Camera *CameraSystem::getCamera(const int allottedNumber) const {
    if(_cameraList.size() <= allottedNumber) return nullptr;
    else return _cameraList.at(allottedNumber);
}

IPylonDevice* CameraSystem::createDevice(const string &cameraName)
{
    const auto instance = &_tlFactory->GetInstance();
    if(cameraName.empty()) return instance->CreateFirstDevice();

    auto cameraInfo = getCameraInfo(cameraName);
    auto device = instance->CreateDevice(cameraInfo);

    return device;
}

void CameraSystem::syslog(const string &message, const bool warning)
{
    if(!warning) cout << "[Camera System] " << message << endl;
    else cerr << "[Camera System] " << message << endl;
}
