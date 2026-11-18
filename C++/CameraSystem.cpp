#include "CameraSystem.h"
#include "CameraDiscoveryDiagnostics.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

namespace {

/** @brief Reads the pylon version, loaded transport layers and local IPv4 interfaces for the empty-list diagnostic. */
class PylonDiscoveryEnvironment final : public CameraDiscoveryDiagnostics::Environment {
public:
    explicit PylonDiscoveryEnvironment(CTlFactory& factory) : _factory(factory) {}

    std::string pylonVersion() const override
    {
        const char* version = Pylon::GetPylonVersionString();
        return version ? version : "unknown";
    }

    CameraDiscoveryDiagnostics::TransportLayerSnapshot transportLayers() const override
    {
        CameraDiscoveryDiagnostics::TransportLayerSnapshot snapshot;
        try {
            TlInfoList_t infos;
            _factory.EnumerateTls(infos);
            for (const auto& info : infos) {
                snapshot.layers.push_back({std::string(info.GetFriendlyName().c_str()),
                    std::string(info.GetVersion().c_str())});
            }
        } catch (const GenericException&) {
            snapshot.available = false;
        }
        return snapshot;
    }

    CameraDiscoveryDiagnostics::InterfaceSnapshot localInterfaces() const override
    {
        CameraDiscoveryDiagnostics::InterfaceSnapshot snapshot;
#if defined(__APPLE__) || defined(__linux__)
        ifaddrs* addresses = nullptr;
        if (getifaddrs(&addresses) != 0) {
            snapshot.available = false;
            return snapshot;
        }
        struct IfaddrsGuard {
            ifaddrs* list;
            ~IfaddrsGuard() { freeifaddrs(list); }
        } guard{addresses};
        for (ifaddrs* current = guard.list; current != nullptr; current = current->ifa_next) {
            if (!current->ifa_addr || current->ifa_addr->sa_family != AF_INET) continue;
            char address[INET_ADDRSTRLEN];
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
            if (!inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address))) continue;
            snapshot.interfaces.push_back({current->ifa_name, address});
        }
#endif
        return snapshot;
    }

private:
    CTlFactory& _factory;
};

void reportEmptyDiscovery(CTlFactory& factory)
{
    PylonDiscoveryEnvironment environment(factory);
    CameraSystem::syslog(CameraDiscoveryDiagnostics::emptyDiscoveryMessage(environment), true);
}

} // namespace

CameraSystem::CameraSystem() : CameraSystem(DeviceEnumerator{}) {}

CameraSystem::CameraSystem(DeviceEnumerator deviceEnumerator)
    : _deviceEnumerator(std::move(deviceEnumerator)){
    PylonInitialize();
    _tlFactory = &CTlFactory::GetInstance();
}

CameraSystem::~CameraSystem(){
    stopInitialDiscovery();
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

void CameraSystem::updateCameraListImpl(bool report)
{
    DeviceInfoList_t updated;
    std::size_t count = 0;
    std::vector<std::string> friendlyNames;
    std::unique_lock<std::mutex> enumerationLock(_enumerationMutex);
    try {
        if (_deviceEnumerator) {
            _deviceEnumerator(updated);
            count = updated.size();
        } else {
            count = _tlFactory->EnumerateDevices(updated);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _devices.clear();
        }
        _initialDiscoveryCondition.notify_all();
        throw;
    }
    if (report) {
        friendlyNames.reserve(updated.size());
        for (const auto& device : updated) {
            friendlyNames.emplace_back(device.GetFriendlyName().c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _devices.clear();
        for (const auto& device : updated) _devices.push_back(device);
    }
    _initialDiscoveryCondition.notify_all();
    if (report) {
        syslog("Updated the camera list: " + to_string(count) + " Camera(s) found.");
        if (count == 0 && !_deviceEnumerator && _tlFactory) reportEmptyDiscovery(*_tlFactory);
        for (const auto& name : friendlyNames) syslog("-- " + name);
    }
    enumerationLock.unlock();
}

void CameraSystem::updateCameraList(){
    try{
        updateCameraListImpl();
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
        updateCameraList();
        std::lock_guard<std::mutex> lock(_mutex);
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
    if (refresh) updateCameraListImpl();
    std::lock_guard<std::mutex> lock(_mutex);
    if (refresh && !_initialDiscoveryRunning) _initialDiscoveryError.clear();
    return deviceCatalogLocked();
}

void CameraSystem::startInitialDiscovery(std::chrono::milliseconds discoveryWindow)
{
    std::lock_guard<std::mutex> lifecycleLock(_discoveryLifecycleMutex);
    if (_initialDiscoveryStarted) return;
    _initialDiscoveryStarted = true;
    _stopInitialDiscoveryRequested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _initialDiscoveryError.clear();
        _initialDiscoveryRunning = true;
    }
    try {
        _initialDiscoveryThread = std::thread([this, discoveryWindow] {
            runInitialDiscovery(discoveryWindow);
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _initialDiscoveryRunning = false;
            _initialDiscoveryError = "The Camera startup discovery thread could not be started.";
        }
        _initialDiscoveryStarted = false;
        _initialDiscoveryCondition.notify_all();
        throw;
    }
}

std::vector<CameraDeviceDescriptor> CameraSystem::deviceCatalogAfterInitialDiscovery(
    std::chrono::milliseconds waitLimit, const std::function<bool()>& cancellationRequested)
{
    std::unique_lock<std::mutex> lock(_mutex);
    const auto deadline = std::chrono::steady_clock::now() + waitLimit;
    while (_initialDiscoveryRunning && _devices.empty()) {
        if (cancellationRequested && cancellationRequested()) {
            throw std::runtime_error("Camera startup target observation was cancelled.");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline && !_initialDiscoveryEnumerationInProgress) {
            throw std::runtime_error("Camera startup discovery is still running; target observation timed out.");
        }
        const auto nextWait = now < deadline
            ? std::min(std::chrono::milliseconds{50},
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
            : std::chrono::milliseconds{50};
        _initialDiscoveryCondition.wait_for(lock, nextWait);
    }
    if (cancellationRequested && cancellationRequested()) {
        throw std::runtime_error("Camera startup target observation was cancelled.");
    }
    if (!_initialDiscoveryError.empty()) {
        throw std::runtime_error(_initialDiscoveryError);
    }
    return deviceCatalogLocked();
}

void CameraSystem::wakeInitialDiscoveryWaiters()
{
    _initialDiscoveryCondition.notify_all();
}

void CameraSystem::runInitialDiscovery(std::chrono::milliseconds discoveryWindow)
{
    const auto deadline = std::chrono::steady_clock::now() + discoveryWindow;
    const auto retryInterval = std::chrono::milliseconds{1000};
    std::size_t attempts = 0;
    std::string error;
    bool cancelled = false;
    for (;;) {
        if (_stopInitialDiscoveryRequested.load(std::memory_order_acquire)) {
            cancelled = true;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _initialDiscoveryEnumerationInProgress = true;
        }
        try {
            updateCameraListImpl(false);
            ++attempts;
        } catch (const GenericException& exception) {
            error = exception.GetDescription();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown exception during Camera startup discovery.";
        }
        bool finished = false;
        bool stopRequested = false;
        std::chrono::milliseconds remaining{};
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _initialDiscoveryEnumerationInProgress = false;
            stopRequested = _stopInitialDiscoveryRequested.load(std::memory_order_acquire);
            const bool foundTargets = !_devices.empty();
            const auto timeRemaining = deadline - std::chrono::steady_clock::now();
            const bool windowElapsed = timeRemaining <= std::chrono::steady_clock::duration::zero();
            finished = !error.empty() || foundTargets || stopRequested || windowElapsed;
            if (finished) {
                _initialDiscoveryError = error;
                _initialDiscoveryRunning = false;
            } else {
                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(timeRemaining);
                if (remaining <= std::chrono::milliseconds::zero()) {
                    remaining = std::chrono::milliseconds{1};
                }
            }
        }
        _initialDiscoveryCondition.notify_all();
        cancelled = cancelled || stopRequested;
        if (!error.empty() || finished) break;

        std::unique_lock<std::mutex> stopLock(_discoveryStopMutex);
        _discoveryStopCondition.wait_for(stopLock, std::min(remaining, retryInterval), [this] {
            return _stopInitialDiscoveryRequested.load(std::memory_order_acquire);
        });
    }

    std::vector<CameraDeviceDescriptor> targets;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _initialDiscoveryError = error;
        _initialDiscoveryRunning = false;
        targets = deviceCatalogLocked();
    }
    _initialDiscoveryCondition.notify_all();
    if (!error.empty()) {
        syslog("Initial Camera discovery failed: " + error, true);
    } else {
        syslog("Completed initial Camera discovery after " + to_string(attempts)
            + " attempt(s) with " + to_string(targets.size()) + " Camera(s).");
        if (targets.empty() && !cancelled && !_deviceEnumerator && _tlFactory) {
            std::lock_guard<std::mutex> enumerationLock(_enumerationMutex);
            reportEmptyDiscovery(*_tlFactory);
        }
        for (const auto& target : targets) syslog("-- " + target.displayName);
    }
}

void CameraSystem::stopInitialDiscovery()
{
    std::thread thread;
    {
        std::lock_guard<std::mutex> lifecycleLock(_discoveryLifecycleMutex);
        _stopInitialDiscoveryRequested.store(true, std::memory_order_release);
        if (_initialDiscoveryThread.joinable()) thread = std::move(_initialDiscoveryThread);
    }
    _discoveryStopCondition.notify_all();
    if (thread.joinable()) thread.join();
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
        updateCameraListImpl();
        std::lock_guard<std::mutex> lock(_mutex);
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
        updateCameraListImpl();
        std::lock_guard<std::mutex> lock(_mutex);
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
