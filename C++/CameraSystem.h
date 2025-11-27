#ifndef CAMERASYSTEM_H
#define CAMERASYSTEM_H
#include <pylon/PylonIncludes.h>
#include "Camera.h"

using namespace Pylon;
using namespace std;

class CameraSystem{
public:
    CameraSystem();
    ~CameraSystem();

    void updateCameraList();
    std::vector<std::string> getCameraList();
    bool isAccesible(std::string camera);
    const CDeviceInfo getCameraInfo(std::string cameraName);

    Camera* addCamera();
    void removeCamera(Camera* camera);
    Camera* getCamera(int allottedNumber);
    IPylonDevice *createDevice(std::string cameraName="");

private:
    CTlFactory *_tlFactory = nullptr;
    DeviceInfoList_t _devices;
    std::vector<Camera*> _cameraList;
};


#endif // CAMERASYSTEM_H
