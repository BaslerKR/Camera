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
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

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
    /** @brief Starts the one bounded worker startup scan without waiting for network discovery. */
    void startInitialDiscovery(std::chrono::milliseconds discoveryWindow);
    /** @brief Waits for a running initial scan, then copies its published target catalog. */
    std::vector<CameraDeviceDescriptor> deviceCatalogAfterInitialDiscovery(
        std::chrono::milliseconds waitLimit, const std::function<bool()>& cancellationRequested = {});
    /** @brief Wakes first-target observers when their owning session is closing. */
    void wakeInitialDiscoveryWaiters();
    bool isAccessible(const std::string &camera);

    CDeviceInfo getCameraInfo(const std::string &cameraName);

    Camera* addCamera();
    void removeCamera(Camera* camera);
    Camera* getCamera(int allottedNumber) const;
    IPylonDevice *createDevice(const std::string &cameraName="", bool exactIdentity = false);

    static void syslog(const std::string &message, bool warning=false);

private:
    void updateCameraListImpl(bool report = true);
    void runInitialDiscovery(std::chrono::milliseconds discoveryWindow);
    void stopInitialDiscovery();
    std::vector<CameraDeviceDescriptor> deviceCatalogLocked() const;

    DeviceEnumerator _deviceEnumerator;
    CTlFactory *_tlFactory = nullptr;
    DeviceInfoList_t _devices;
    std::vector<Camera*> _cameraList;
    std::size_t _nextCameraNumber = 0;
    mutable std::mutex _mutex;
    std::mutex _enumerationMutex;
    std::condition_variable _initialDiscoveryCondition;
    bool _initialDiscoveryRunning = false;
    bool _initialDiscoveryEnumerationInProgress = false;
    std::string _initialDiscoveryError;
    std::mutex _discoveryLifecycleMutex;
    std::thread _initialDiscoveryThread;
    bool _initialDiscoveryStarted = false;
    std::atomic<bool> _stopInitialDiscoveryRequested{false};
    std::mutex _discoveryStopMutex;
    std::condition_variable _discoveryStopCondition;
};


#endif // CAMERASYSTEM_H
