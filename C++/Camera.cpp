#include "Camera.h"
#include "CameraSystem.h"
#include "CameraGrabDiagnostics.h"

#include <pylon/ConfigurationHelper.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>

namespace {

template<typename Callback>
using CallbackRegistry = std::unordered_map<size_t, Callback>;

template<typename Callback>
size_t registerCallback(std::mutex& mutex, CallbackRegistry<Callback>& registry, std::atomic<size_t>& nextId, Callback cb)
{
    if(!cb) return 0;

    std::lock_guard<std::mutex> lock(mutex);
    const size_t id = nextId++;
    registry.emplace(id, std::move(cb));
    return id;
}

template<typename Callback>
bool deregisterCallback(std::mutex& mutex, CallbackRegistry<Callback>& registry, size_t id)
{
    std::lock_guard<std::mutex> lock(mutex);
    return registry.erase(id) > 0;
}

template<typename Callback>
void clearCallbacks(std::mutex& mutex, CallbackRegistry<Callback>& registry)
{
    std::lock_guard<std::mutex> lock(mutex);
    registry.clear();
}

template<typename Callback, typename... Args>
void dispatchCallbacks(std::mutex& mutex, CallbackRegistry<Callback>& registry, Args&&... args)
{
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks.reserve(registry.size());
        for(auto& kv : registry){
            callbacks.push_back(kv.second);
        }
    }

    for(auto& cb : callbacks){
        if(cb) cb(std::forward<Args>(args)...);
    }
}

string toLowerCopy(string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch){
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string enableComponent(GenApi::INodeMap& nodeMap,
                            const char* componentName,
                            const std::initializer_list<const char*> pixelFormats = {},
                            const char* sourceName = nullptr)
{
    auto* componentSelectorNode = nodeMap.GetNode("ComponentSelector");
    auto* componentEnableNode = nodeMap.GetNode("ComponentEnable");
    if(!componentSelectorNode || !componentEnableNode) return {};
    if(!GenApi::IsWritable(componentSelectorNode) || !GenApi::IsWritable(componentEnableNode)) return {};

    if(sourceName){
        auto* sourceSelectorNode = nodeMap.GetNode("SourceSelector");
        if(sourceSelectorNode && GenApi::IsWritable(sourceSelectorNode)){
            Pylon::CEnumParameter(nodeMap, "SourceSelector").TrySetValue(sourceName);
        }
    }

    auto componentSelector = Pylon::CEnumParameter(nodeMap, "ComponentSelector");
    auto componentEnable = Pylon::CBooleanParameter(nodeMap, "ComponentEnable");
    if(!componentSelector.TrySetValue(componentName)) return {};
    componentEnable.SetValue(true);

    auto* pixelFormatNode = nodeMap.GetNode("PixelFormat");
    if(pixelFormatNode && GenApi::IsReadable(pixelFormatNode)){
        auto pixelFormatParam = Pylon::CEnumParameter(nodeMap, "PixelFormat");
        if(GenApi::IsWritable(pixelFormatNode)){
            for(const char* pixelFormat : pixelFormats){
                if(pixelFormatParam.TrySetValue(pixelFormat)) break;
            }
        }
        return pixelFormatParam.GetValue().c_str();
    }

    return {};
}

std::optional<double> readFloatParameter(GenApi::INodeMap& nodeMap, const char* name)
{
    auto* node = nodeMap.GetNode(name);
    if(!node || !GenApi::IsReadable(node)) return std::nullopt;
    return Pylon::CFloatParameter(nodeMap, name).GetValue();
}

bool isColorPixelFormat(const std::string& value)
{
    const auto lower = toLowerCopy(value);
    return lower.find("rgb") != std::string::npos || lower.find("bgr") != std::string::npos;
}

bool trySelectEnumValue(GenApi::INodeMap& nodeMap, const char* parameterName, const char* value)
{
    auto* node = nodeMap.GetNode(parameterName);
    return node && GenApi::IsWritable(node)
        && Pylon::CEnumParameter(nodeMap, parameterName).TrySetValue(value);
}

class ScopedEnumValueRestore
{
public:
    ScopedEnumValueRestore(GenApi::INodeMap& nodeMap, const char* parameterName)
        : _nodeMap(nodeMap)
        , _parameterName(parameterName)
    {
        auto* node = _nodeMap.GetNode(_parameterName);
        if(node && GenApi::IsReadable(node) && GenApi::IsWritable(node)){
            _originalValue = Pylon::CEnumParameter(_nodeMap, _parameterName).GetValue().c_str();
        }
    }

    ~ScopedEnumValueRestore()
    {
        if(!_originalValue) return;

        auto* node = _nodeMap.GetNode(_parameterName);
        if(node && GenApi::IsWritable(node)){
            Pylon::CEnumParameter(_nodeMap, _parameterName).TrySetValue(_originalValue->c_str());
        }
    }

    ScopedEnumValueRestore(const ScopedEnumValueRestore&) = delete;
    ScopedEnumValueRestore& operator=(const ScopedEnumValueRestore&) = delete;

private:
    GenApi::INodeMap& _nodeMap;
    const char* _parameterName = nullptr;
    std::optional<std::string> _originalValue;
};

bool hasStereoMiniShape(GenApi::INodeMap& nodeMap)
{
    ScopedEnumValueRestore sourceRestore(nodeMap, "SourceSelector");
    ScopedEnumValueRestore componentRestore(nodeMap, "ComponentSelector");
    return trySelectEnumValue(nodeMap, "SourceSelector", "Source3")
        && trySelectEnumValue(nodeMap, "ComponentSelector", "Intensity")
        && trySelectEnumValue(nodeMap, "SourceSelector", "Source1")
        && trySelectEnumValue(nodeMap, "ComponentSelector", "Range");
}

bool hasStereoAceShape(GenApi::INodeMap& nodeMap)
{
    ScopedEnumValueRestore componentRestore(nodeMap, "ComponentSelector");
    return trySelectEnumValue(nodeMap, "ComponentSelector", "Disparity");
}

bool hasBlazeShape(GenApi::INodeMap& nodeMap)
{
    ScopedEnumValueRestore componentRestore(nodeMap, "ComponentSelector");
    return trySelectEnumValue(nodeMap, "ComponentSelector", "Confidence")
        && nodeMap.GetNode("Scan3dInvalidDataValue")
        && nodeMap.GetNode("Scan3dCoordinateSelector");
}

PylonScene3DProfile::DeviceFamily detect3DDeviceFamily(
    GenApi::INodeMap& nodeMap,
    const std::string& deviceClass)
{
    const auto lowerDeviceClass = toLowerCopy(deviceClass);
    if(lowerDeviceClass.find("stereo_mini") != std::string::npos){
        return PylonScene3DProfile::DeviceFamily::StereoMini;
    }
    if(lowerDeviceClass.find("basler_xw") != std::string::npos){
        return PylonScene3DProfile::DeviceFamily::StereoAce;
    }
    if(lowerDeviceClass.find("blaze") != std::string::npos){
        return PylonScene3DProfile::DeviceFamily::Blaze;
    }

    if(hasStereoMiniShape(nodeMap)){
        return PylonScene3DProfile::DeviceFamily::StereoMini;
    }
    if(hasStereoAceShape(nodeMap)){
        return PylonScene3DProfile::DeviceFamily::StereoAce;
    }
    if(hasBlazeShape(nodeMap)){
        return PylonScene3DProfile::DeviceFamily::Blaze;
    }

    return PylonScene3DProfile::DeviceFamily::Image2D;
}

const char* deviceFamilyName(PylonScene3DProfile::DeviceFamily family)
{
    switch(family){
    case PylonScene3DProfile::DeviceFamily::Blaze:
        return "blaze";
    case PylonScene3DProfile::DeviceFamily::StereoAce:
        return "stereo-ace";
    case PylonScene3DProfile::DeviceFamily::StereoMini:
        return "stereo-mini";
    case PylonScene3DProfile::DeviceFamily::Image2D:
        break;
    }
    return "2d";
}

std::string safeCameraName(Pylon::CInstantCamera& camera, const std::string& fallback)
{
    if(!fallback.empty()) return fallback;

    try{
        return camera.GetDeviceInfo().GetFriendlyName().c_str();
    }catch(const Pylon::GenericException&){
        return {};
    }
}

void applyGenDcContainerDefaults(GenApi::INodeMap& nodeMap, GenApi::INodeMap& instantCameraNodeMap)
{
    auto* genDCStreamingModeNode = nodeMap.GetNode("GenDCStreamingMode");
    if(genDCStreamingModeNode && GenApi::IsWritable(genDCStreamingModeNode)){
        Pylon::CEnumParameter(nodeMap, "GenDCStreamingMode").TrySetValue("On");
    }

    auto* useExtendedIdNode = instantCameraNodeMap.GetNode("UseExtendedIdIfAvailable");
    if(useExtendedIdNode && GenApi::IsWritable(useExtendedIdNode)){
        Pylon::CBooleanParameter(instantCameraNodeMap, "UseExtendedIdIfAvailable").TrySetValue(true);
    }
}

}

Camera::Camera(CameraSystem *parent, const int allottedNumber) : _system(parent), _allottedNumber(allottedNumber)
{
    _currentCamera.RegisterConfiguration(this, RegistrationMode_ReplaceAll, Pylon::Cleanup_None);
}

Camera::~Camera()
{
    try{
        close();
        _currentCamera.DeregisterConfiguration(this);
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(), true);
    }catch(const std::exception &e){
        CameraSystem::syslog(e.what(), true);
    }catch(...){
        CameraSystem::syslog("Unknown exception while destroying camera", true);
    }
}

Camera::CallbackId Camera::registerStatusCallback(StatusCallback cb)
{
    return registerCallback(_statusMutex, _statusObservers, _nextStatusObserverId, std::move(cb));
}

bool Camera::deregisterStatusCallback(CallbackId id)
{
    return deregisterCallback(_statusMutex, _statusObservers, id);
}

void Camera::clearStatusCallbacks()
{
    clearCallbacks(_statusMutex, _statusObservers);
}

bool Camera::open(const string& cameraName){
    std::lock_guard<std::mutex> featureLifecycle(_featureLifecycleMutex);
    return openLocked(cameraName);
}

bool Camera::openLocked(const string& cameraName, bool exactIdentity){
    const auto cleanupFailedOpen = [this]{
        try{
            requestStop();
            _deviceAvailable.store(false, std::memory_order_release);
            _streamKind.store(StreamKind::Image2D, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
                _scene3DProfile = {};
            }
            clearNodeEventHandlers();
            if(_currentCamera.IsOpen()){
                _currentCamera.Close();
            }
            if(_currentCamera.IsPylonDeviceAttached()){
                _currentCamera.DestroyDevice();
            }
        }catch(const GenericException &e){
            CameraSystem::syslog(e.GetDescription(), true);
        }catch(const std::exception &e){
            CameraSystem::syslog(e.what(), true);
        }
    };

    try{
        _deviceAvailable.store(false, std::memory_order_release);
        clearNodeEventHandlers();
        CameraSystem::syslog("Try to open " + (cameraName.empty() ? "any one of the cameras on this system" : cameraName) + ".");
        _currentCamera.Attach(_system->createDevice(cameraName, exactIdentity), Cleanup_Delete);
        _currentCamera.Open();
        if(_currentCamera.IsOpen()){
            markOpened(_currentCamera);
        }
        configureStreamForConnectedCamera();
        registerNodeEventHandlers();
        return true;
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(), true);
    }catch(const std::exception &e){
        CameraSystem::syslog(e.what(), true);
    }
    cleanupFailedOpen();
    return false;
}

bool Camera::isOpened() const {
    try{
        return _deviceAvailable.load(std::memory_order_acquire) && _currentCamera.IsOpen();
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
    return false;
}

void Camera::close(){
    std::lock_guard<std::mutex> featureLifecycle(_featureLifecycleMutex);
    closeLocked();
}

bool Camera::closeLocked(){
    try{
        stop();
        _deviceAvailable.store(false, std::memory_order_release);
        _streamKind.store(StreamKind::Image2D, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
            _scene3DProfile = {};
        }
        if(_currentCamera.IsOpen()){
            _currentCamera.Close();
        }
        clearNodeEventHandlers();
        if(_currentCamera.IsPylonDeviceAttached()){
            _currentCamera.DestroyDevice();
        }
        return true;
    }catch(const GenericException &e){ CameraSystem::syslog(e.GetDescription(),true); }
    catch(const std::exception &e){ CameraSystem::syslog(e.what(),true); }
    catch(...){ CameraSystem::syslog("Unknown exception while closing camera", true); }
    return false;
}

std::vector<CameraDeviceDescriptor> Camera::deviceCatalog(bool refresh) const { return _system->deviceCatalog(refresh); }

CameraConnectionState Camera::connectionState() const {
    std::lock_guard<std::mutex> lock(_connectionStateMutex);
    const bool connected = _deviceAvailable.load(std::memory_order_acquire);
    return {connected, _connectionGeneration.load(std::memory_order_acquire), connected ? _connectedDeviceId : std::string(), connected ? _connectedCameraName : std::string()};
}

CameraConnectionOutcome Camera::controlConnection(bool connect, const std::string& deviceId, std::uint64_t expectedGeneration,
        CameraConnectionState* result) {
    std::unique_lock<std::mutex> lifecycle(_featureLifecycleMutex, std::try_to_lock);
    if (!lifecycle.owns_lock()) return CameraConnectionOutcome::Busy;
    if (_connectionGeneration.load(std::memory_order_acquire) != expectedGeneration) return CameraConnectionOutcome::Conflict;
    if (connect && _deviceAvailable.load(std::memory_order_acquire)) return CameraConnectionOutcome::AlreadyConnected;
    if (connect) {
        const auto targets = deviceCatalog(false);
        if (deviceId.empty() || std::none_of(targets.begin(), targets.end(), [&](const auto& target) { return target.id == deviceId; })) return CameraConnectionOutcome::NotFound;
    }
    const bool ok = connect ? openLocked(deviceId, true) : closeLocked();
    if (result) *result = connectionState();
    return ok ? CameraConnectionOutcome::Success : CameraConnectionOutcome::Failed;
}

std::string Camera::getConnectedCameraName() const
{
    std::lock_guard<std::mutex> lock(_connectionStateMutex);
    return _connectedCameraName;
}

Camera::CallbackId Camera::registerGrabCallback(GrabCallback cb)
{
    return registerCallback(_grabCallbackMutex, _grabCallbacks, _nextGrabCallbackId, std::move(cb));
}

bool Camera::deregisterGrabCallback(const CallbackId id)
{
    return deregisterCallback(_grabCallbackMutex, _grabCallbacks, id);
}

void Camera::clearGrabCallbacks()
{
    clearCallbacks(_grabCallbackMutex, _grabCallbacks);
}

Camera::CallbackId Camera::registerGrab3DCallback(Grab3DCallback cb)
{
    return registerCallback(_grab3DCallbackMutex, _grab3DCallbacks, _nextGrab3DCallbackId, std::move(cb));
}

bool Camera::deregisterGrab3DCallback(const CallbackId id)
{
    return deregisterCallback(_grab3DCallbackMutex, _grab3DCallbacks, id);
}

void Camera::clearGrab3DCallbacks()
{
    clearCallbacks(_grab3DCallbackMutex, _grab3DCallbacks);
}

void Camera::ready()
{
    if(!_permitBackpressureEnabled.load(std::memory_order_acquire)) return;

    int current = _permits.load(std::memory_order_acquire);
    while(current < 1 && !_permits.compare_exchange_weak(
              current,
              1,
              std::memory_order_acq_rel,
              std::memory_order_acquire)){
    }
    _permitCondition.notify_one();
}

void Camera::configureStreamForConnectedCamera()
{
    _streamKind.store(StreamKind::Image2D, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
        _scene3DProfile = {};
    }

    auto& nodeMap = _currentCamera.GetNodeMap();
    auto& instantCameraNodeMap = _currentCamera.GetInstantCameraNodeMap();

    const auto deviceClass = _currentCamera.GetDeviceInfo().GetDeviceClass().c_str();
    const auto family = detect3DDeviceFamily(nodeMap, deviceClass);
    if(family != PylonScene3DProfile::DeviceFamily::StereoMini){
        applyGenDcContainerDefaults(nodeMap, instantCameraNodeMap);
    }
    if(family == PylonScene3DProfile::DeviceFamily::StereoAce){
        configureStereoAceStream(nodeMap);
    }else if(family == PylonScene3DProfile::DeviceFamily::StereoMini){
        configureStereoMiniStream(nodeMap);
    }else if(family == PylonScene3DProfile::DeviceFamily::Blaze){
        configureBlazeStream(nodeMap);
    }

    const auto streamKind = _streamKind.load(std::memory_order_acquire);
    const auto* routeText = streamKind == StreamKind::MultiPart3D ? "3D-only" : "2D";
    const auto modelName = toLowerCopy(_currentCamera.GetDeviceInfo().GetModelName().c_str());
    CameraSystem::syslog("[Info " + to_string(_allottedNumber) + "] model="
                         + modelName
                         + " family=" + deviceFamilyName(family)
                         + " route=" + routeText);
}

void Camera::configureBlazeStream(GenApi::INodeMap& nodeMap)
{
    _streamKind.store(StreamKind::MultiPart3D, std::memory_order_release);

    auto* triggerModeNode = nodeMap.GetNode("TriggerMode");
    bool isHardwareTrigger = false;
    if(triggerModeNode && GenApi::IsReadable(triggerModeNode)){
        isHardwareTrigger = (Pylon::CEnumParameter(nodeMap, "TriggerMode").GetValue() == "On");
    }

    if(!isHardwareTrigger){
        Pylon::CConfigurationHelper::DisableAllTriggers(nodeMap);
    }

    enableComponent(nodeMap, "Range", {"Coord3D_ABC32f"});
    enableComponent(nodeMap, "Intensity", {"Mono16"});
    enableComponent(nodeMap, "Confidence", {"Confidence16"});

    auto* coordinateSelectorNode = _currentCamera.Scan3dCoordinateSelector.GetNode();
    auto* invalidDataValueNode = _currentCamera.Scan3dInvalidDataValue.GetNode();
    if(coordinateSelectorNode && invalidDataValueNode &&
       GenApi::IsWritable(coordinateSelectorNode) &&
       GenApi::IsWritable(invalidDataValueNode)){
        for(const auto* axis : {"CoordinateA", "CoordinateB", "CoordinateC"}){
            _currentCamera.Scan3dCoordinateSelector.SetValue(axis);
            _currentCamera.Scan3dInvalidDataValue.SetValue(std::numeric_limits<float>::quiet_NaN());
        }
    }

    std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
    _scene3DProfile.family = PylonScene3DProfile::DeviceFamily::Blaze;
    _scene3DProfile.geometry = PylonScene3DProfile::GeometryKind::DirectXyzRange;
}

void Camera::configureStereoAceStream(GenApi::INodeMap& nodeMap)
{
    _streamKind.store(StreamKind::MultiPart3D, std::memory_order_release);
    const auto intensityFormat = enableComponent(nodeMap, "Intensity", {"RGB8", "Mono8"});
    enableComponent(nodeMap, "Disparity", {"Coord3D_C16"});

    PylonScene3DProfile profile;
    profile.family = PylonScene3DProfile::DeviceFamily::StereoAce;
    profile.geometry = PylonScene3DProfile::GeometryKind::DisparityReconstruction;
    profile.colorRegisteredToRange = isColorPixelFormat(intensityFormat);
    profile.coordinateScale = readFloatParameter(nodeMap, "Scan3dCoordinateScale").value_or(0.0);
    profile.coordinateOffset = readFloatParameter(nodeMap, "Scan3dCoordinateOffset").value_or(0.0);
    profile.baseline = readFloatParameter(nodeMap, "Scan3dBaseline").value_or(0.0);
    profile.focalLength = readFloatParameter(nodeMap, "Scan3dFocalLength").value_or(0.0);
    profile.principalPointU = readFloatParameter(nodeMap, "Scan3dPrincipalPointU").value_or(0.0);
    profile.principalPointV = readFloatParameter(nodeMap, "Scan3dPrincipalPointV").value_or(0.0);

    std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
    _scene3DProfile = std::move(profile);
}

void Camera::configureStereoMiniStream(GenApi::INodeMap& nodeMap)
{
    _streamKind.store(StreamKind::MultiPart3D, std::memory_order_release);
    enableComponent(nodeMap, "Intensity");
    enableComponent(nodeMap, "Range", {"Coord3D_ABC32f", "Coord3D_C16"});

    PylonScene3DProfile profile;
    profile.family = PylonScene3DProfile::DeviceFamily::StereoMini;
    profile.geometry = PylonScene3DProfile::GeometryKind::DirectXyzRange;
    profile.colorRegisteredToRange = false;
    profile.coordinateScale = readFloatParameter(nodeMap, "Scan3dCoordinateScale").value_or(1.0);
    profile.coordinateOffset = readFloatParameter(nodeMap, "Scan3dCoordinateOffset").value_or(0.0);
    profile.baseline = readFloatParameter(nodeMap, "Scan3dBaseline").value_or(0.0);
    profile.focalLength = readFloatParameter(nodeMap, "Scan3dFocalLength").value_or(0.0);
    profile.principalPointU = readFloatParameter(nodeMap, "Scan3dPrincipalPointU").value_or(0.0);
    profile.principalPointV = readFloatParameter(nodeMap, "Scan3dPrincipalPointV").value_or(0.0);

    std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
    _scene3DProfile = std::move(profile);
}

void Camera::grab(const size_t frames){
    try{
        if(!isOpened()) return;
        if(_isRunning.load(std::memory_order_acquire)) return;
        if(_thread.joinable()){
            const auto joinStarted = std::chrono::steady_clock::now();
            _thread.join();
            const auto joinMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - joinStarted).count();
            if(joinMs >= 100){
                CameraSystem::syslog("[WARN] Previous camera grab worker join took "
                    + std::to_string(joinMs) + " ms before re-arm.", true);
            }
        }

        const bool triggerMode = _currentCamera.TriggerMode.GetValue()
            == Basler_UniversalCameraParams::TriggerModeEnums::TriggerMode_On;
        _grabErrorCount.store(0, std::memory_order_release);
        if(triggerMode){
            _currentCamera.MaxNumBuffer = 30;
            _currentCamera.StartGrabbing(GrabStrategy_OneByOne, GrabLoop_ProvidedByUser);
        }else{
            _currentCamera.MaxNumBuffer = 5;
            _currentCamera.StartGrabbing(GrabStrategy_LatestImageOnly, GrabLoop_ProvidedByUser);
        }

        _isRunning.store(true, std::memory_order_release);
        _frameTarget.store(frames, std::memory_order_release);
        _frameSeq.store(0, std::memory_order_release);
        _permits.store(1, std::memory_order_release);
        _permitBackpressureEnabled.store(!triggerMode, std::memory_order_release);

        CameraSystem::syslog("[DEBUG] Grab armed: targetFrames=" + std::to_string(frames)
            + ", triggerMode=" + (triggerMode ? "On" : "Off")
            + ", strategy=" + (triggerMode ? "OneByOne" : "LatestImageOnly")
            + ", receiveLoop=worker-thread.");

        _thread = std::thread([this, triggerMode]{
            const auto workerStarted = std::chrono::steady_clock::now();
            auto nextProgressLog = workerStarted + std::chrono::seconds(10);
            std::chrono::milliseconds maximumCallbackTime{0};
            std::size_t slowCallbackCount = 0;
            std::size_t delivered = 0;
            std::size_t failedResults = 0;
            try{
                CGrabResultPtr grabResult;

                while(_isRunning.load(std::memory_order_acquire) && _deviceAvailable.load(std::memory_order_acquire) && _currentCamera.IsGrabbing()){
                    if(_currentCamera.RetrieveResult(1000, grabResult, Pylon::TimeoutHandling_Return)){
                        if(grabResult->GrabSucceeded()){
                            if(!triggerMode){
                                std::unique_lock<std::mutex> lock(_permitMutex);
                                _permitCondition.wait(lock, [this]{
                                    return !_isRunning.load(std::memory_order_acquire) || _permits.load(std::memory_order_acquire) > 0;
                                });

                                if (!_isRunning.load(std::memory_order_acquire)) break;
                                _permits.fetch_sub(1, std::memory_order_acq_rel);
                            }

                            auto seq = _frameSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
                            const auto callbackStarted = std::chrono::steady_clock::now();
                            if(_streamKind.load(std::memory_order_acquire) == StreamKind::MultiPart3D){
                                try{
                                    auto container = grabResult->GetDataContainer();
                                    dispatchCallbacks(_grab3DCallbackMutex, _grab3DCallbacks, container, seq);
                                }catch(const GenericException &e){
                                    CameraSystem::syslog(std::string("[WARN] [Camera System] GetDataContainer exception: ") + e.GetDescription(), true);
                                }catch(const std::exception &e){
                                    CameraSystem::syslog(std::string("[WARN] [Camera System] GetDataContainer std::exception: ") + e.what(), true);
                                }catch(...){
                                    CameraSystem::syslog("[WARN] [Camera System] GetDataContainer unknown exception", true);
                                }
                            }else{
                                CPylonImage image;
                                image.AttachGrabResultBuffer(grabResult);
                                dispatchCallbacks(_grabCallbackMutex, _grabCallbacks, image, seq);
                            }

                            const auto callbackTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - callbackStarted);
                            maximumCallbackTime = std::max(maximumCallbackTime, callbackTime);
                            if(callbackTime >= std::chrono::milliseconds(500)){
                                ++slowCallbackCount;
                                if(slowCallbackCount <= 3 || slowCallbackCount % 100 == 0){
                                    CameraSystem::syslog("[WARN] Grab callback backpressure: frame="
                                        + std::to_string(seq) + ", callbackMs="
                                        + std::to_string(callbackTime.count()) + ", slowCount="
                                        + std::to_string(slowCallbackCount) + ".", true);
                                }
                            }

                            ++delivered;
                            const auto now = std::chrono::steady_clock::now();
                            if(now >= nextProgressLog){
                                CameraSystem::syslog("[DEBUG] Grab worker progress: frames="
                                    + std::to_string(delivered) + ", lastCallbackMs="
                                    + std::to_string(callbackTime.count()) + ", maxCallbackMs="
                                    + std::to_string(maximumCallbackTime.count()) + ".");
                                nextProgressLog = now + std::chrono::seconds(10);
                            }

                            auto target = _frameTarget.load(std::memory_order_acquire);
                            if(target !=0 && delivered >= target){
                                _isRunning.store(false, std::memory_order_release);
                                _permitCondition.notify_all();
                                break;
                            }
                        } else {
                            ++failedResults;
                            if (CameraGrabDiagnostics::shouldReportFailure(failedResults)) {
                                const auto cameraName = getConnectedCameraName();
                                const auto errorDescription = grabResult->GetErrorDescription();
                                CameraSystem::syslog(
                                    "[WARN] Incomplete grab result camera="
                                        + (cameraName.empty() ? "<unknown>" : cameraName)
                                        + " errorCode=" + std::to_string(grabResult->GetErrorCode())
                                        + " description=" + std::string(errorDescription.c_str())
                                        + " failureCount=" + std::to_string(failedResults) + ".",
                                    true);
                            }
                        }
                    } else {
                        ++failedResults;
                        if (CameraGrabDiagnostics::shouldReportFailure(failedResults)) {
                            const auto cameraName = getConnectedCameraName();
                            CameraSystem::syslog(
                                "[WARN] Grab result timeout/no result camera="
                                    + (cameraName.empty() ? "<unknown>" : cameraName)
                                    + " failureCount=" + std::to_string(failedResults) + ".",
                                true);
                        }
                    }
                }
            }catch(const GenericException &e){
                CameraSystem::syslog(e.GetDescription(),true);
            }catch(const std::exception &e){
                CameraSystem::syslog(e.what(),true);
            }catch(...){
                CameraSystem::syslog("Unknown exception in Camera::grab thread.", true);
            }
            _isRunning.store(false, std::memory_order_release);
            _permitCondition.notify_all();
            try{
                if(_currentCamera.IsGrabbing()) _currentCamera.StopGrabbing();
            }catch(const GenericException &e){
                CameraSystem::syslog(std::string("[WARN] StopGrabbing cleanup failed: ")
                    + e.GetDescription(), true);
            }catch(const std::exception &e){
                CameraSystem::syslog(std::string("[WARN] StopGrabbing cleanup failed: ")
                    + e.what(), true);
            }
            _permitBackpressureEnabled.store(false, std::memory_order_release);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - workerStarted).count();
            CameraSystem::syslog("[DEBUG] Grab worker finished: frames="
                + std::to_string(delivered) + ", elapsedMs=" + std::to_string(elapsedMs)
                + ", maxCallbackMs=" + std::to_string(maximumCallbackTime.count())
                + ", slowCallbacks=" + std::to_string(slowCallbackCount) + ".");
        });
    }catch(const GenericException &e){
        _isRunning.store(false, std::memory_order_release);
        _permitBackpressureEnabled.store(false, std::memory_order_release);
        try{
            if(_currentCamera.IsGrabbing()) _currentCamera.StopGrabbing();
        }catch(...){
        }
        CameraSystem::syslog(e.GetDescription(),true);
    }catch(const std::exception &e){
        _isRunning.store(false, std::memory_order_release);
        _permitBackpressureEnabled.store(false, std::memory_order_release);
        try{
            if(_currentCamera.IsGrabbing()) _currentCamera.StopGrabbing();
        }catch(...){
        }
        CameraSystem::syslog(std::string("Camera grab start failed: ") + e.what(), true);
    }
}

void Camera::stop(){
    try{
        requestStop();

        if(_thread.joinable() && _thread.get_id() != std::this_thread::get_id()){
            const auto joinStarted = std::chrono::steady_clock::now();
            _thread.join();
            const auto joinMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - joinStarted).count();
            CameraSystem::syslog("[DEBUG] Camera stop joined grab worker in "
                + std::to_string(joinMs) + " ms.");
        }
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(),true);
    }catch(const std::exception &e){
        CameraSystem::syslog(std::string("Camera stop failed: ") + e.what(), true);
    }
}

void Camera::requestStop()
{
    _isRunning.store(false, std::memory_order_release);
    _permitCondition.notify_all();
}

std::vector<string> Camera::getUpdatedCameraList() const {
    return _system->getCameraList();
}

std::vector<string> Camera::getCachedCameraList() const {
    return _system->getCachedCameraList();
}

GenApi::INodeMap &Camera::getNodeMap(){
    return _currentCamera.GetNodeMap();
}

bool Camera::accessFeatures(const std::function<void(GenApi::INodeMap&, std::uint64_t)>& operation, bool* busy)
{
    if (busy) *busy = false;
    std::unique_lock<std::mutex> lifecycle(_featureLifecycleMutex, std::defer_lock);
    if (busy) {
        if (!lifecycle.try_lock()) { *busy = true; return false; }
    } else lifecycle.lock();
    Pylon::AutoLock cameraLock(_currentCamera.GetLock());
    if (!_deviceAvailable.load(std::memory_order_acquire) || !_currentCamera.IsOpen()) return false;
    auto& map = _currentCamera.GetNodeMap();
    GenApi::AutoLock nodeLock(map.GetLock());
    operation(map, _connectionGeneration.load(std::memory_order_acquire));
    return true;
}

PylonScene3DProfile Camera::scene3DProfile() const
{
    std::lock_guard<std::mutex> lock(_scene3DProfileMutex);
    return _scene3DProfile;
}

Camera::CallbackId Camera::registerNodeUpdatedCallback(NodeCallback cb)
{
    return registerCallback(_nodeCallbackMutex, _nodeCallbacks, _nextNodeCallbackId, std::move(cb));
}

bool Camera::deregisterNodeUpdatedCallback(const CallbackId id)
{
    return deregisterCallback(_nodeCallbackMutex, _nodeCallbacks, id);
}

void Camera::clearNodeUpdatedCallbacks()
{
    clearCallbacks(_nodeCallbackMutex, _nodeCallbacks);
}

void Camera::notifyFeaturesChanged()
{
    dispatchCallbacks(_nodeCallbackMutex, _nodeCallbacks, std::string("Root"));
}

void Camera::OnAttached(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, getConnectedCameraName());
    CameraSystem::syslog(from + " attached.");
}

void Camera::OnDetached(CInstantCamera &camera){
    _deviceAvailable.store(false, std::memory_order_release);
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Detached.");
}

void Camera::OnDestroyed(CInstantCamera &camera){
    requestStop();
    _deviceAvailable.store(false, std::memory_order_release);
    auto from = "[Info " + to_string(_allottedNumber)  +"] ";
    CameraSystem::syslog(from + "Device destroyed.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnOpened(CInstantCamera &camera){
    markOpened(camera);
}

void Camera::OnClosed(CInstantCamera &camera){
    _deviceAvailable.store(false, std::memory_order_release);
    const auto cameraName = safeCameraName(camera, getConnectedCameraName());
    {
        std::lock_guard<std::mutex> lock(_connectionStateMutex);
        _connectedCameraName.clear();
    }
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + cameraName;
    CameraSystem::syslog(from + " closed.");
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnCameraDeviceRemoved(CInstantCamera &camera){
    requestStop();
    _deviceAvailable.store(false, std::memory_order_release);
    const auto cameraName = safeCameraName(camera, getConnectedCameraName());
    {
        std::lock_guard<std::mutex> lock(_connectionStateMutex);
        _connectedCameraName.clear();
    }
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + cameraName;
    CameraSystem::syslog(from + " removed physically.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, false);
}

void Camera::OnGrabStarted(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, getConnectedCameraName());
    CameraSystem::syslog(from + " started grabbing.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, true);
}

void Camera::OnGrabStopped(CInstantCamera &camera){
    auto from = "[Info " + to_string(_allottedNumber)  +"] " + safeCameraName(camera, getConnectedCameraName());
    CameraSystem::syslog(from + " stopped grabbing.");
    dispatchCallbacks(_statusMutex, _statusObservers, GrabbingStatus, false);
}

void Camera::OnGrabError(CInstantCamera& camera, const char* errorMessage)
{
    const auto failureCount = _grabErrorCount.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (!CameraGrabDiagnostics::shouldReportFailure(failureCount)) return;

    const auto cameraName = safeCameraName(camera, getConnectedCameraName());
    CameraSystem::syslog(
        "[WARN] Camera grab error camera="
            + (cameraName.empty() ? "<unknown>" : cameraName)
            + " failureCount=" + std::to_string(failureCount)
            + " description=" + (errorMessage ? errorMessage : "<none>") + ".",
        true);
}

void Camera::OnCameraEvent(CInstantCamera &camera, intptr_t userProvidedId, GenApi::INode *pNode){
    if(!pNode || !_deviceAvailable.load(std::memory_order_acquire)) return;

    std::string nodeName;

    try{
        nodeName = pNode->GetName().c_str();
    }catch(const GenericException &e){
        CameraSystem::syslog(e.GetDescription(), true);
        return;
    }catch(const std::exception &e){
        CameraSystem::syslog(e.what(), true);
        return;
    }

    if(!nodeName.empty()){
        dispatchCallbacks(_nodeCallbackMutex, _nodeCallbacks, nodeName);
    }
}

void Camera::markOpened(CInstantCamera& camera)
{
    const auto cameraName = safeCameraName(camera, {});
    bool wasAvailable;
    {
        std::lock_guard<std::mutex> lock(_connectionStateMutex);
        _connectedCameraName = cameraName;
        _connectedDeviceId = camera.GetDeviceInfo().GetFullName().c_str();
        wasAvailable = _deviceAvailable.exchange(true, std::memory_order_acq_rel);
        if (!wasAvailable) _connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    if(wasAvailable) return;

    auto from = "[Info " + to_string(_allottedNumber)  +"] " + cameraName;
    CameraSystem::syslog(from + " opened.");
    dispatchCallbacks(_statusMutex, _statusObservers, ConnectionStatus, true);
}

void Camera::registerNodeEventHandlers()
{
    clearNodeEventHandlers();

    GenApi::NodeList_t nodes;
    _currentCamera.GetNodeMap().GetNodes(nodes);
    for(const auto cur : nodes){
        if(cur->GetName() == "Root") continue;
        if(cur->GetPrincipalInterfaceType() != GenApi::intfICategory) continue;
        if(!GenApi::IsAvailable(cur)) continue;

        GenApi::NodeList_t children;
        cur->GetChildren(children);

        for(const auto child : children){
            const auto accessMode = child->GetAccessMode();
            if(accessMode != GenApi::RO && accessMode != GenApi::RW) continue;
            if(!GenApi::IsReadable(child)) continue;

            try{
                std::string nodeName = child->GetName().c_str();
                if(std::find(_registeredNodeEventNames.begin(), _registeredNodeEventNames.end(), nodeName) != _registeredNodeEventNames.end()) continue;

                _currentCamera.RegisterCameraEventHandler(this,
                                                         nodeName.c_str(),
                                                         _allottedNumber,
                                                         ERegistrationMode::RegistrationMode_Append,
                                                         ECleanup::Cleanup_None,
                                                         CameraEventAvailability_Optional);
                _registeredNodeEventNames.push_back(std::move(nodeName));
            }catch(const GenericException &e){
                CameraSystem::syslog(e.GetDescription(), true);
            }
        }
    }
}

void Camera::clearNodeEventHandlers()
{
    for(const auto& nodeName : _registeredNodeEventNames){
        _currentCamera.DeregisterCameraEventHandler(this, nodeName.c_str());
    }
    _registeredNodeEventNames.clear();
}

