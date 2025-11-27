#include "CameraSystem.h"

CameraSystem::CameraSystem(){
    PylonInitialize();
    _tlFactory = &CTlFactory::GetInstance();
}

CameraSystem::~CameraSystem(){
    for(const auto cur : _cameraList){
        cur->stop();
        cur->close();
    }
    Pylon::PylonTerminate();
}

void CameraSystem::updateCameraList(){
    try{
        _cameraList.clear();
        auto cnt = _tlFactory->EnumerateDevices(_devices);
        cout << cnt << " Camera(s) found." << endl;
        for(auto i=0; i<_devices.size(); ++i){
            cout << "-- " <<_devices.at(i).GetFriendlyName() << endl;
        }
        // Signal needed to interface a UI class, sending to camera classes
    }catch(const GenericException &e){
        cerr << e.what() << endl;
    }
}

std::vector<string> CameraSystem::getCameraList(){
    std::vector<std::string> list;
    try{
        for(auto &cur : _devices)
            list.push_back(cur.GetFriendlyName().c_str());
    }catch(const GenericException &e){
        cerr << e.what() << endl;
    }
    return list;
}

bool CameraSystem::isAccesible(string camera){
    return _tlFactory->IsDeviceAccessible(getCameraInfo(camera));
}

const CDeviceInfo CameraSystem::getCameraInfo(string cameraName){
    try{
        cout << "Searching the device information of " + cameraName + "..." << endl;
        for(auto &cur:_devices){
            if(cameraName == cur.GetFriendlyName().c_str()){
                cout << "Matched information found." << endl;
                return cur;
            }
        }
    }catch(const GenericException &e){
        cerr << e.what() << endl;
    }
    return CDeviceInfo();
}

Camera *CameraSystem::addCamera()
{
    auto camera = new Camera(this, _cameraList.size());
    _cameraList.push_back(camera);

    cout << "New camera instance created." << endl;
    return camera;
}

Camera *CameraSystem::getCamera(int allottedNumber)
{
    if(_cameraList.size() <= allottedNumber) return nullptr;
    else return _cameraList.at(allottedNumber);
}

IPylonDevice* CameraSystem::createDevice(string cameraName)
{
    if(cameraName=="") return _tlFactory->GetInstance().CreateFirstDevice();
    return _tlFactory->GetInstance().CreateDevice(getCameraInfo(cameraName));
}
