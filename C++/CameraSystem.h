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

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>

using namespace Pylon;
using namespace std;

class CameraSystem{
public:
    using DeviceEnumerator = std::function<void(DeviceInfoList_t&)>;

    CameraSystem();
    explicit CameraSystem(DeviceEnumerator deviceEnumerator);
    ~CameraSystem();

    void updateCameraList();
    std::vector<std::string> getCameraList();
    std::vector<std::string> getCachedCameraList() const;
    /** @brief Copies cached identities, optionally refreshing them through the discovery authority. */
    std::vector<CameraDeviceDescriptor> deviceCatalog(bool refresh);
    /** @brief Keeps startup discovery pending until its first device appears or the wait expires. */
    std::vector<CameraDeviceDescriptor> discoverInitialCatalog(
        std::chrono::milliseconds firstDeviceWait, const std::function<bool()>& interrupted = {});
    bool isAccessible(const std::string &camera);

    CDeviceInfo getCameraInfo(const std::string &cameraName);

    Camera* addCamera();
    void removeCamera(Camera* camera);
    Camera* getCamera(int allottedNumber) const;
    IPylonDevice *createDevice(const std::string &cameraName="", bool exactIdentity = false);

    static void syslog(const std::string &message, bool warning=false);

private:
    void updateCameraListLocked(bool report = true);
    std::vector<CameraDeviceDescriptor> deviceCatalogLocked() const;

    DeviceEnumerator _deviceEnumerator;
    CTlFactory *_tlFactory = nullptr;
    DeviceInfoList_t _devices;
    std::vector<Camera*> _cameraList;
    std::size_t _nextCameraNumber = 0;
    mutable std::mutex _mutex;
};


#endif // CAMERASYSTEM_H
