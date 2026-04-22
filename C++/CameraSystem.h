#ifndef CAMERASYSTEM_H
#define CAMERASYSTEM_H

/**
 * @file CameraSystem.h
 * @brief Manages Basler pylon runtime access, device listing, and Camera creation.
 *
 * Owns Camera wrapper instances and keeps transport-layer device creation inside
 * the Camera submodule.
 */

#include <pylon/PylonIncludes.h>
#include "Camera.h"

#include <cstddef>
#include <mutex>

using namespace Pylon;
using namespace std;

class CameraSystem{
public:
    CameraSystem();
    ~CameraSystem();

    void updateCameraList();
    std::vector<std::string> getCameraList();
    bool isAccessible(const std::string &camera);

    CDeviceInfo getCameraInfo(const std::string &cameraName);

    Camera* addCamera();
    void removeCamera(Camera* camera);
    Camera* getCamera(int allottedNumber) const;
    IPylonDevice *createDevice(const std::string &cameraName="");

    static void syslog(const std::string &message, bool warning=false);

private:
    void updateCameraListLocked();

    CTlFactory *_tlFactory = nullptr;
    DeviceInfoList_t _devices;
    std::vector<Camera*> _cameraList;
    std::size_t _nextCameraNumber = 0;
    mutable std::mutex _mutex;
};


#endif // CAMERASYSTEM_H
