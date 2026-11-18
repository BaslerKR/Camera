#include "CameraSourceController.h"
#include "Tools/ToolProvider.h"
#include "CameraSystem.h"
#include "DeviceRuntime/Contract/HardwareResourceKey.h"
#include <QSet>
#include <QtTest>
#include <QElapsedTimer>
#include <QEventLoop>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

struct ScanReleaseGuard {
    std::function<void()> release;
    ~ScanReleaseGuard() { if (release) release(); }
};

/** @brief One host-side claim handle whose release updates a test registry. */
class TestClaimHandle final : public IDevicePluginResourceClaim {
public:
    TestClaimHandle(QSet<QString>* held, QString key)
        : _held(held), _key(std::move(key)) {}
    ~TestClaimHandle() override { release(); }
    QString key() const override { return _key; }
    void release() override
    {
        if (_released) return;
        _released = true;
        _held->remove(_key);
    }
private:
    QSet<QString>* _held = nullptr;
    QString _key;
    bool _released = false;
};

/** @brief Minimal claims service used to observe the controller claim lifecycle. */
class TestClaims final : public IDevicePluginResourceClaims {
public:
    std::shared_ptr<IDevicePluginResourceClaim> claimResource(const QString& key, QString* error) override
    {
        if (_held.contains(key)) {
            if (error) *error = QStringLiteral("The test claim is already held.");
            return {};
        }
        _held.insert(key);
        return std::make_shared<TestClaimHandle>(&_held, key);
    }
    QSet<QString> held() const { return _held; }
private:
    QSet<QString> _held;
};

/** @brief Collects change callbacks and can hold one delta to force queue overflow. */
class ChangeCollector {
public:
    IDeviceParameterControl::ChangeListener listener()
    {
        return [this](DeviceParameterChange change) { accept(std::move(change)); };
    }
    bool waitFor(const std::size_t count, const int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this, count] { return _changes.size() >= count; });
    }
    bool waitForResync(const int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            for (const auto& change : _changes) if (change.resync) return true;
            return false;
        });
    }
    std::optional<DeviceParameterChange> at(const std::size_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (index >= _changes.size()) return std::nullopt;
        return _changes[index];
    }
    std::vector<DeviceParameterChange> snapshot()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _changes;
    }
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _changes.size();
    }
    void holdDeltas(const bool hold)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _hold = hold;
    }
    bool waitUntilHeld(const int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _condition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return _holding; });
    }
    void releaseHeld()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _hold = false;
        _holding = false;
        _release.notify_all();
        _condition.notify_all();
    }

private:
    void accept(DeviceParameterChange change)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        const bool hold = _hold && !_holding && change.kind == DeviceParameterChangeKind::Delta
            && !change.resync && change.error == DeviceParameterError::None;
        if (hold) {
            _holding = true;
            _condition.notify_all();
            _release.wait(lock, [this] { return !_holding; });
        }
        _changes.push_back(std::move(change));
        _condition.notify_all();
    }
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::condition_variable _release;
    std::vector<DeviceParameterChange> _changes;
    bool _hold = false;
    bool _holding = false;
};

/** @brief Mirrors the module's GenApi tree mapping without calling its private helpers. */
struct ExpectedTree {
    std::vector<std::string> categoryPath;
    DeviceParameterVisibility visibility = DeviceParameterVisibility::Primary;
    DeviceParameterRepresentation representation = DeviceParameterRepresentation::Automatic;
    std::vector<DeviceParameterSelectorRelation> relations;
    bool producerShared = false;
};

ExpectedTree expectedTree(GenApi::INode* node, const DeviceParameterKind kind)
{
    ExpectedTree expected;
    GenApi::INode* current = node;
    std::set<GenApi::INode*> visited;
    for (int depth = 0; depth < 32; ++depth) {
        GenApi::NodeList_t parents;
        current->GetParents(parents);
        GenApi::INode* category = nullptr;
        for (auto* parent : parents) {
            if (parent->GetPrincipalInterfaceType() == GenApi::intfICategory) {
                category = parent;
                break;
            }
        }
        if (!category || !visited.insert(category).second) break;
        const std::string name = category->GetName().c_str();
        if (name == "Root") break;
        if (name == "TransportLayerControl") expected.producerShared = true;
        auto segment = std::string(category->GetDisplayName().c_str());
        if (segment.empty()) segment = name;
        if (segment.size() > deviceParameterCategorySegmentLimit)
            segment.resize(deviceParameterCategorySegmentLimit);
        if (!segment.empty()) expected.categoryPath.push_back(std::move(segment));
        current = category;
    }
    std::reverse(expected.categoryPath.begin(), expected.categoryPath.end());
    if (expected.categoryPath.size() > deviceParameterCategoryDepthLimit)
        expected.categoryPath.resize(deviceParameterCategoryDepthLimit);
    switch (node->GetVisibility()) {
    case GenApi::Expert: expected.visibility = DeviceParameterVisibility::Advanced; break;
    case GenApi::Guru: expected.visibility = DeviceParameterVisibility::Expert; break;
    case GenApi::Invisible: expected.visibility = DeviceParameterVisibility::Hidden; break;
    default: expected.visibility = DeviceParameterVisibility::Primary; break;
    }
    if (kind == DeviceParameterKind::Enumeration) {
        expected.representation = DeviceParameterRepresentation::Choice;
    } else if (kind == DeviceParameterKind::Boolean) {
        expected.representation = DeviceParameterRepresentation::Toggle;
    } else if (kind == DeviceParameterKind::String) {
        expected.representation = DeviceParameterRepresentation::Text;
    } else if (kind == DeviceParameterKind::Integer || kind == DeviceParameterKind::Float) {
        const auto representation = kind == DeviceParameterKind::Integer
            ? GenApi::CIntegerPtr(node)->GetRepresentation()
            : GenApi::CFloatPtr(node)->GetRepresentation();
        switch (representation) {
        case GenApi::Logarithmic: expected.representation = DeviceParameterRepresentation::Slider; break;
        case GenApi::Boolean: expected.representation = DeviceParameterRepresentation::Toggle; break;
        case GenApi::HexNumber:
        case GenApi::IPV4Address:
        case GenApi::MACAddress: expected.representation = DeviceParameterRepresentation::Text; break;
        case GenApi::Linear:
        case GenApi::PureNumber: expected.representation = DeviceParameterRepresentation::Number; break;
        default: expected.representation = DeviceParameterRepresentation::Automatic; break;
        }
    }
    std::map<std::string, DeviceParameterSelectorRole> relations;
    GenApi::CSelectorPtr selector(node);
    if (selector) {
        GenApi::FeatureList_t selected;
        selector->GetSelectedFeatures(selected);
        for (const auto feature : selected) {
            GenApi::INode* related = feature->GetNode();
            if (!related) continue;
            auto name = std::string(related->GetName().c_str());
            if (name.size() > deviceParameterSelectorRelationNameLimit) continue;
            relations.emplace(std::move(name), DeviceParameterSelectorRole::Selects);
        }
        GenApi::FeatureList_t selecting;
        selector->GetSelectingFeatures(selecting);
        for (const auto feature : selecting) {
            GenApi::INode* related = feature->GetNode();
            if (!related) continue;
            auto name = std::string(related->GetName().c_str());
            if (name.size() > deviceParameterSelectorRelationNameLimit) continue;
            relations.emplace(std::move(name), DeviceParameterSelectorRole::SelectedBy);
        }
    }
    for (auto& entry : relations) {
        if (expected.relations.size() >= deviceParameterSelectorRelationLimit) break;
        expected.relations.push_back({entry.first, entry.second});
    }
    return expected;
}

/** @brief Restricts test writes to stable image-format and exposure features. */
bool safeChangeName(const std::string& name)
{
    static const std::set<std::string> allowed{
        "Width", "Height", "OffsetX", "OffsetY", "AcquisitionFrameRate",
        "AcquisitionFrameRateEnable", "BinningHorizontal", "BinningVertical",
        "BlackLevel", "ChunkModeActive", "DigitalShift", "ExposureTime",
        "ExposureTimeAbs", "ExposureTimeBaseAbs", "ExposureTimeRaw", "Gain",
        "GainRaw", "Gamma", "LineDebouncerTime", "TriggerDelay", "LineIndex",
        "LineInverterBool0", "ForceFailedBufferCount", "ForcedFailedBuffersRemaining",
        "AutoExposureTimeLowerLimit", "AutoExposureTimeUpperLimit", "AutoGainLowerLimit",
        "AutoGainUpperLimit", "ReverseX", "ReverseY"};
    return allowed.count(name) > 0;
}

/** @brief Selects a writable scalar with a different valid value. */
struct WritableCandidate {
    DeviceParameterDescriptor descriptor;
    DeviceParameterValue value;
};
std::optional<WritableCandidate> writableChange(const DeviceParameterDescriptor& descriptor)
{
    if (!descriptor.available || !descriptor.writable || !descriptor.value) return std::nullopt;
    if (!descriptor.target.selectors.empty() || !safeChangeName(descriptor.target.name))
        return std::nullopt;
    for (const auto& relation : descriptor.selectorRelations)
        if (relation.role == DeviceParameterSelectorRole::Selects) return std::nullopt;
    switch (descriptor.kind) {
    case DeviceParameterKind::Integer: {
        const auto current = std::get<std::int64_t>(*descriptor.value);
        if (!descriptor.integerMinimum || !descriptor.integerMaximum) return std::nullopt;
        const auto step = descriptor.integerIncrement.value_or(1);
        if (step <= 0 || *descriptor.integerMinimum >= *descriptor.integerMaximum) return std::nullopt;
        auto candidate = *descriptor.integerMinimum;
        if (candidate == current) {
            if (candidate > *descriptor.integerMaximum - step) return std::nullopt;
            candidate += step;
        }
        return WritableCandidate{descriptor, candidate};
    }
    case DeviceParameterKind::Float: {
        if (!descriptor.floatMinimum || !descriptor.floatMaximum) return std::nullopt;
        if (!(*descriptor.floatMinimum < *descriptor.floatMaximum)) return std::nullopt;
        auto candidate = *descriptor.floatMinimum;
        if (candidate == std::get<double>(*descriptor.value)) candidate = *descriptor.floatMaximum;
        return WritableCandidate{descriptor, candidate};
    }
    case DeviceParameterKind::Boolean:
        return WritableCandidate{descriptor, !std::get<bool>(*descriptor.value)};
    default:
        return std::nullopt;
    }
}

/**
 * @brief Reports whether any observed name changes writability in the collected order.
 * @note The snapshot establishes the baseline; the first delta that flips a name matches.
 */
std::string findEmulator(CameraSystem& system)
{
    for (const auto& name : system.getCameraList()) {
        if (std::string(system.getCameraInfo(name).GetDeviceClass().c_str()) == "BaslerCamEmu")
            return name;
    }
    return {};
}

/**
 * @brief Reports whether a normal delta flips any observed name's writability.
 * @note Snapshots reset the baseline; a resync snapshot never counts as a transition,
 *       so the test proves the device observation arrived as a delta.
 */
bool sawWritabilityTransition(const std::vector<DeviceParameterChange>& changes,
    const bool fromWritable, const bool toWritable)
{
    std::map<std::string, bool> writable;
    for (const auto& change : changes) {
        if (change.error != DeviceParameterError::None) continue;
        if (change.kind == DeviceParameterChangeKind::Snapshot) {
            writable.clear();
            for (const auto& descriptor : change.descriptors)
                writable[descriptor.target.name] = descriptor.writable;
            continue;
        }
        if (change.resync) continue;
        for (const auto& descriptor : change.descriptors) {
            const auto previous = writable.find(descriptor.target.name);
            if (previous != writable.end() && previous->second == fromWritable
                && descriptor.writable == toWritable)
                return true;
            writable[descriptor.target.name] = descriptor.writable;
        }
    }
    return false;
}

} // namespace

class CameraParameterControlTests : public QObject {
    Q_OBJECT
    std::unique_ptr<CameraSystem> _system;
private slots:
    void initTestCase() {
        qputenv("PYLON_CAMEMU", "1");
        // The adapter shares one SDK runtime, as the loaded plugin does.
        _system = std::make_unique<CameraSystem>();
    }
    void cleanupTestCase() { _system.reset(); }
    void firstConnectionTargetsRequestUsesWorkerDiscoveryCatalog() {
        std::string emulatorDisplayName;
        {
            CameraSystem probe;
            emulatorDisplayName = findEmulator(probe);
        }
        if (emulatorDisplayName.empty()) QSKIP("The pylon camera emulator is not installed.");

        CameraSystem system;
        // This test directly refreshes its CameraSystem, then checks cached reads and explicit refresh.
        const auto startupCatalog = system.deviceCatalog(true);
        std::string expectedEmulatorId;
        for (const auto& item : startupCatalog) {
            if (item.displayName == emulatorDisplayName) {
                expectedEmulatorId = item.id;
                break;
            }
        }
        QVERIFY(!expectedEmulatorId.empty());

        auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);

        const auto readTargets = [&](bool refresh, DeviceConnectionTargets* result) {
            auto promise = std::make_shared<std::promise<DeviceConnectionTargets>>();
            auto future = promise->get_future();
            const auto completion = [promise](DeviceConnectionTargets value) {
                promise->set_value(std::move(value));
            };
            const auto admitted = refresh
                ? connection->requestDiscover(completion)
                : connection->requestConnectionTargets(completion);
            if (admitted.error != DeviceConnectionError::None
                || future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return false;
            QCoreApplication::processEvents();
            *result = future.get();
            return true;
        };
        const auto targetIds = [](const DeviceConnectionTargets& targets) {
            std::vector<std::string> ids;
            for (const auto& target : targets.targets) ids.push_back(target.targetId);
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        std::vector<std::string> startupIds;
        for (const auto& item : startupCatalog) startupIds.push_back(item.id);
        std::sort(startupIds.begin(), startupIds.end());

        DeviceConnectionTargets first;
        QVERIFY(readTargets(false, &first));
        QCOMPARE(first.result.error, DeviceConnectionError::None);
        QVERIFY(std::find_if(first.targets.begin(), first.targets.end(), [&](const auto& target) {
            return target.targetId == expectedEmulatorId;
        }) != first.targets.end());
        QVERIFY(targetIds(first) == startupIds);

        DeviceConnectionTargets cached;
        QVERIFY(readTargets(false, &cached));
        QCOMPARE(cached.result.error, DeviceConnectionError::None);
        QVERIFY(targetIds(cached) == startupIds);

        DeviceConnectionTargets refreshed;
        QVERIFY(readTargets(true, &refreshed));
        QCOMPARE(refreshed.result.error, DeviceConnectionError::None);
        QVERIFY(std::find_if(refreshed.targets.begin(), refreshed.targets.end(), [&](const auto& target) {
            return target.targetId == expectedEmulatorId;
        }) != refreshed.targets.end());
    }
    void firstTargetsRequestWaitsForDelayedStartupDiscovery() {
        std::promise<void> delayedEnumerationStarted;
        auto delayedStarted = delayedEnumerationStarted.get_future();
        std::promise<void> releaseEnumeration;
        auto release = releaseEnumeration.get_future().share();
        std::atomic<int> enumerationCalls{0};
        const std::string targetId = "BaslerGigE#12345678#192.0.2.50:3956";
        CameraSystem system([&](DeviceInfoList_t& devices) {
            const int call = enumerationCalls.fetch_add(1);
            if (call == 0) return;
            if (call == 1) {
                delayedEnumerationStarted.set_value();
                release.wait();
            }
            Pylon::CDeviceInfo device;
            device.SetDeviceClass("BaslerGigE");
            device.SetSerialNumber("12345678");
            device.SetFriendlyName("Basler GigE (first request delayed)");
            device.SetFullName(targetId.c_str());
            devices.push_back(device);
        });
        auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::WaitForInitialDiscovery);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);
        system.startInitialDiscovery(std::chrono::seconds{3});
        const bool discoveryPending = delayedStarted.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!discoveryPending) {
            releaseEnumeration.set_value();
            QFAIL("Startup discovery did not reach the delayed fake transport enumeration.");
        }

        auto targetsPromise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targetsFuture = targetsPromise->get_future();
        const auto admission = connection->requestConnectionTargets(
            [targetsPromise](DeviceConnectionTargets targets) {
                targetsPromise->set_value(std::move(targets));
            });
        if (admission.error != DeviceConnectionError::None) {
            releaseEnumeration.set_value();
            QCOMPARE(admission.error, DeviceConnectionError::None);
            return;
        }
        const bool returnedWhileDiscoveryPending = targetsFuture.wait_for(std::chrono::milliseconds(100))
            == std::future_status::ready;
        releaseEnumeration.set_value();
        const bool completed = targetsFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        QVERIFY(!returnedWhileDiscoveryPending);
        QVERIFY(completed);
        QCOMPARE(enumerationCalls.load(), 2);
        const auto targets = targetsFuture.get();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        QCOMPARE(targets.targets.size(), std::size_t(1));
        QCOMPARE(targets.targets.front().targetId, targetId);
        QCOMPARE(targets.targets.front().displayName, std::string("Basler GigE (first request delayed)"));
    }
    void closingControllerCancelsWaitingTargetRead() {
        std::promise<void> delayedEnumerationStarted;
        auto delayedStarted = delayedEnumerationStarted.get_future();
        std::promise<void> releaseEnumeration;
        auto release = releaseEnumeration.get_future().share();
        std::atomic<bool> enumerationReleased{false};
        const auto releaseScan = [&] {
            if (!enumerationReleased.exchange(true)) releaseEnumeration.set_value();
        };
        const std::string targetId = "BaslerGigE#12345678#192.0.2.50:3956";
        std::atomic<int> enumerationCalls{0};
        CameraSystem system([&](DeviceInfoList_t& devices) {
            const int call = enumerationCalls.fetch_add(1);
            if (call == 0) return;
            if (call == 1) {
                delayedEnumerationStarted.set_value();
                release.wait();
            }
            Pylon::CDeviceInfo device;
            device.SetDeviceClass("BaslerGigE");
            device.SetSerialNumber("12345678");
            device.SetFriendlyName("Basler GigE (close cancels target wait)");
            device.SetFullName(targetId.c_str());
            devices.push_back(device);
        });
        ScanReleaseGuard releaseGuard{releaseScan};
        auto* camera = system.addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::WaitForInitialDiscovery);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        system.startInitialDiscovery(std::chrono::seconds{3});
        if (delayedStarted.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            releaseScan();
            QFAIL("Startup discovery did not reach the delayed fake transport enumeration.");
        }

        auto targetsPromise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targetsFuture = targetsPromise->get_future();
        const auto admission = connection->requestConnectionTargets(
            [targetsPromise](DeviceConnectionTargets targets) {
                targetsPromise->set_value(std::move(targets));
            });
        if (admission.error != DeviceConnectionError::None) {
            releaseScan();
            QCOMPARE(admission.error, DeviceConnectionError::None);
            return;
        }
        const bool wasWaiting = targetsFuture.wait_for(std::chrono::milliseconds{100})
            == std::future_status::timeout;

        std::promise<void> controllerClosedPromise;
        auto controllerClosed = controllerClosedPromise.get_future().share();
        std::atomic<bool> scanReleasedToUnblockClose{false};
        std::thread closeWatchdog([&] {
            if (controllerClosed.wait_for(std::chrono::milliseconds{500}) != std::future_status::ready) {
                scanReleasedToUnblockClose.store(true);
                releaseScan();
            }
        });
        controller.reset();
        controllerClosedPromise.set_value();
        closeWatchdog.join();
        releaseScan();

        QVERIFY(wasWaiting);
        QVERIFY(!scanReleasedToUnblockClose.load());
        QVERIFY(targetsFuture.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
        const auto targets = targetsFuture.get();
        QCOMPARE(targets.result.error, DeviceConnectionError::Unavailable);
        QVERIFY(targets.targets.empty());
    }
    void targetObservationWaitsForEnumerationAlreadyInFlightPastBudget() {
        std::promise<void> delayedEnumerationStarted;
        auto delayedStarted = delayedEnumerationStarted.get_future();
        std::promise<void> releaseEnumeration;
        auto release = releaseEnumeration.get_future().share();
        std::atomic<bool> enumerationReleased{false};
        const auto releaseScan = [&] {
            if (!enumerationReleased.exchange(true)) releaseEnumeration.set_value();
        };
        const std::string targetId = "BaslerGigE#12345678#192.0.2.50:3956";
        std::atomic<int> enumerationCalls{0};
        CameraSystem system([&](DeviceInfoList_t& devices) {
            const int call = enumerationCalls.fetch_add(1);
            if (call == 0) return;
            if (call == 1) {
                delayedEnumerationStarted.set_value();
                release.wait();
            }
            Pylon::CDeviceInfo device;
            device.SetDeviceClass("BaslerGigE");
            device.SetSerialNumber("12345678");
            device.SetFriendlyName("Basler GigE (enumeration exceeds wait budget)");
            device.SetFullName(targetId.c_str());
            devices.push_back(device);
        });
        ScanReleaseGuard releaseGuard{releaseScan};
        auto* camera = system.addCamera(); QVERIFY(camera);
        system.startInitialDiscovery(std::chrono::seconds{3});
        if (delayedStarted.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            releaseScan();
            QFAIL("Startup discovery did not reach the delayed fake transport enumeration.");
        }

        auto targetsFuture = std::async(std::launch::async, [&] {
            return camera->deviceCatalogAfterInitialDiscovery(std::chrono::milliseconds{50});
        });
        const bool stayedPendingAfterBudget = targetsFuture.wait_for(std::chrono::milliseconds{150})
            == std::future_status::timeout;
        releaseScan();
        QVERIFY(targetsFuture.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        std::vector<CameraDeviceDescriptor> targets;
        QString error;
        try {
            targets = targetsFuture.get();
        } catch (const std::exception& exception) {
            error = QString::fromLocal8Bit(exception.what());
        }
        QVERIFY2(stayedPendingAfterBudget, "The target read timed out while EnumerateDevices was still in flight.");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(targets.size(), std::size_t(1));
        QCOMPARE(targets.front().id, targetId);
    }
    void emptyDelayedEnumerationCompletesWithoutObserverFailureAfterBudget() {
        std::promise<void> delayedEnumerationStarted;
        auto delayedStarted = delayedEnumerationStarted.get_future();
        std::promise<void> releaseEnumeration;
        auto release = releaseEnumeration.get_future().share();
        std::atomic<bool> enumerationReleased{false};
        const auto releaseScan = [&] {
            if (!enumerationReleased.exchange(true)) releaseEnumeration.set_value();
        };
        std::atomic<int> enumerationCalls{0};
        CameraSystem system([&](DeviceInfoList_t&) {
            const int call = enumerationCalls.fetch_add(1);
            if (call == 0) return;
            if (call == 1) {
                delayedEnumerationStarted.set_value();
                release.wait();
            }
        });
        ScanReleaseGuard releaseGuard{releaseScan};
        auto* camera = system.addCamera(); QVERIFY(camera);
        system.startInitialDiscovery(std::chrono::milliseconds{50});
        if (delayedStarted.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
            releaseScan();
            QFAIL("Startup discovery did not reach the delayed empty transport enumeration.");
        }

        auto targetsFuture = std::async(std::launch::async, [&] {
            return camera->deviceCatalogAfterInitialDiscovery(std::chrono::milliseconds{50});
        });
        const bool stayedPendingAfterBudget = targetsFuture.wait_for(std::chrono::milliseconds{150})
            == std::future_status::timeout;
        releaseScan();
        QVERIFY(targetsFuture.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

        std::vector<CameraDeviceDescriptor> targets;
        QString error;
        try {
            targets = targetsFuture.get();
        } catch (const std::exception& exception) {
            error = QString::fromLocal8Bit(exception.what());
        }
        QVERIFY2(stayedPendingAfterBudget, "The target read timed out while EnumerateDevices was still in flight.");
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(targets.empty());
    }
    void workerPreflightReturnsSnapshotThenUserTargetsWait() {
        std::promise<void> delayedEnumerationStarted;
        auto delayedStarted = delayedEnumerationStarted.get_future();
        std::promise<void> releaseEnumeration;
        auto release = releaseEnumeration.get_future().share();
        std::atomic<int> enumerationCalls{0};
        const std::string targetId = "BaslerGigE#12345678#192.0.2.50:3956";
        CameraSystem system([&](DeviceInfoList_t& devices) {
            const int call = enumerationCalls.fetch_add(1);
            if (call == 0) return;
            if (call == 1) {
                delayedEnumerationStarted.set_value();
                release.wait();
            }
            Pylon::CDeviceInfo device;
            device.SetDeviceClass("BaslerGigE");
            device.SetSerialNumber("12345678");
            device.SetFriendlyName("Basler GigE (delayed)");
            device.SetFullName(targetId.c_str());
            devices.push_back(device);
        });
        auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera, nullptr, nullptr, nullptr,
            CameraSourceController::InitialTargetsPolicy::WorkerPreflightThenWait);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);
        system.startInitialDiscovery(std::chrono::seconds{3});
        const bool discoveryPending = delayedStarted.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!discoveryPending) {
            releaseEnumeration.set_value();
            QFAIL("Startup discovery did not reach the delayed fake transport enumeration.");
        }

        auto preflightPromise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto preflightFuture = preflightPromise->get_future();
        const auto preflightAdmission = connection->requestConnectionTargets(
            [preflightPromise](DeviceConnectionTargets targets) {
                preflightPromise->set_value(std::move(targets));
            });
        if (preflightAdmission.error != DeviceConnectionError::None) {
            releaseEnumeration.set_value();
            QCOMPARE(preflightAdmission.error, DeviceConnectionError::None);
            return;
        }
        const bool preflightReturned = preflightFuture.wait_for(std::chrono::seconds(2))
            == std::future_status::ready;
        if (!preflightReturned) {
            releaseEnumeration.set_value();
            QFAIL("The discovery-only target snapshot waited for the running scan.");
        }
        const auto preflightTargets = preflightFuture.get();

        auto targetsPromise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targetsFuture = targetsPromise->get_future();
        const auto admission = connection->requestConnectionTargets(
            [targetsPromise](DeviceConnectionTargets targets) {
                targetsPromise->set_value(std::move(targets));
            });
        if (admission.error != DeviceConnectionError::None) {
            releaseEnumeration.set_value();
            QCOMPARE(admission.error, DeviceConnectionError::None);
            return;
        }
        auto concurrentTargetsPromise = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto concurrentTargetsFuture = concurrentTargetsPromise->get_future();
        const auto concurrentAdmission = connection->requestConnectionTargets(
            [concurrentTargetsPromise](DeviceConnectionTargets targets) {
                concurrentTargetsPromise->set_value(std::move(targets));
            });
        if (concurrentAdmission.error != DeviceConnectionError::None) {
            releaseEnumeration.set_value();
            QCOMPARE(concurrentAdmission.error, DeviceConnectionError::None);
            return;
        }
        const bool returnedWhileDiscoveryPending = targetsFuture.wait_for(std::chrono::milliseconds(100))
            == std::future_status::ready;
        const bool concurrentReturnedWhileDiscoveryPending = concurrentTargetsFuture.wait_for(
            std::chrono::milliseconds(0)) == std::future_status::ready;
        releaseEnumeration.set_value();
        const bool completed = targetsFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        const bool concurrentCompleted = concurrentTargetsFuture.wait_for(std::chrono::seconds(2))
            == std::future_status::ready;
        QCOMPARE(preflightTargets.result.error, DeviceConnectionError::None);
        QVERIFY(preflightTargets.targets.empty());
        QVERIFY(!returnedWhileDiscoveryPending);
        QVERIFY(!concurrentReturnedWhileDiscoveryPending);
        QVERIFY(completed);
        QVERIFY(concurrentCompleted);
        QCOMPARE(enumerationCalls.load(), 2);
        const auto targets = targetsFuture.get();
        QCOMPARE(targets.result.error, DeviceConnectionError::None);
        QCOMPARE(targets.targets.size(), std::size_t(1));
        QCOMPARE(targets.targets.front().targetId, targetId);
        QCOMPARE(targets.targets.front().displayName, std::string("Basler GigE (delayed)"));
        const auto concurrentTargets = concurrentTargetsFuture.get();
        QCOMPARE(concurrentTargets.result.error, DeviceConnectionError::None);
        QCOMPARE(concurrentTargets.targets.size(), std::size_t(1));
        QCOMPARE(concurrentTargets.targets.front().targetId, targetId);
    }
    void connectionCapabilityRejectsMalformedCallsAndCompletesExactlyOnce() {
        auto* camera = _system->addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        QVERIFY(!qobject_cast<IToolProvider*>(controller.get())); // Lifecycle tools are host-declared.
        const auto capabilities = connection->connectionCapabilities();
        QVERIFY(capabilities.targets.available && capabilities.discover.available && capabilities.connect.available);
        QVERIFY(!capabilities.single.available && !capabilities.single.reason.empty());
        QVERIFY(capabilities.live.available && capabilities.live.reason.empty());
        std::atomic<int> completions{0}, disconnectedStates{0};
        QCOMPARE(connection->requestConnectionState({}).error, DeviceConnectionError::InvalidArgument);
        const auto refusedWithoutConnection = [&](auto admit) -> DeviceConnectionResult {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto future = promise->get_future();
            const auto admission = admit([promise](DeviceConnectionResult result) { promise->set_value(std::move(result)); });
            if (admission.error != DeviceConnectionError::None) return admission;
            if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                return DeviceConnectionResult{DeviceConnectionError::Timeout, "Lifecycle admission did not finish."};
            return future.get();
        };
        QCOMPARE(refusedWithoutConnection([&](auto done) { return connection->requestSingle("x", done); }).error,
            DeviceConnectionError::Disconnected);
        QCOMPARE(refusedWithoutConnection([&](auto done) { return connection->requestLiveStart("x", done); }).error,
            DeviceConnectionError::Disconnected);
        QCOMPARE(completions.load(), 0); QVERIFY(!camera->connectionState().connected);
        std::promise<void> entered; auto completed = entered.get_future();
        QCOMPARE(connection->requestConnectionState([&](DeviceConnectionResult result) {
            const auto count = ++completions;
            if (result.state && !result.state->connected && result.state->connectionId.empty()) ++disconnectedStates;
            if (count == 1) { entered.set_value(); throw std::runtime_error("consumer completion failed"); }
        }).error, DeviceConnectionError::None);
        QVERIFY(completed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        controller.reset(); // Drain the accepted worker before inspecting the final callback count.
        QCOMPARE(completions.load(), 1); QCOMPARE(disconnectedStates.load(), 1);
        _system->removeCamera(camera);
    }
    void acceptedConnectionWorkDrainsOnControllerDestruction() {
        auto* camera = _system->addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        std::promise<void> allEntered, release; auto entered = allEntered.get_future(); auto released = release.get_future().share();
        std::atomic<int> started{0}, completed{0}, refusedCallback{0};
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(connection->requestConnectionState([&](DeviceConnectionResult result) {
                if (++started == 8) allEntered.set_value();
                released.wait();
                if (result.error == DeviceConnectionError::None) ++completed;
            }).error, DeviceConnectionError::None);
        }
        const bool ready = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!ready) release.set_value();
        QVERIFY(ready);
        const auto refused = connection->requestConnectionState([&](DeviceConnectionResult) { ++refusedCallback; });
        std::thread releaser([&] { release.set_value(); });
        controller.reset(); releaser.join();
        QCOMPARE(refused.error, DeviceConnectionError::Busy);
        QCOMPARE(completed.load(), 8); QCOMPARE(refusedCallback.load(), 0);
        _system->removeCamera(camera);
        QCoreApplication::processEvents(); // No late callback may access the deleted controller.
    }
    void connectionLifecycleUsesExactEmulatedTargets() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system; auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);
        const auto wait = [](auto& future) {
            const bool ready = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
            QCoreApplication::processEvents();
            return ready;
        };
        const auto run = [&](auto admit) {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>(); auto future = promise->get_future();
            const auto admitted = admit([promise](DeviceConnectionResult result) { promise->set_value(std::move(result)); });
            if (admitted.error != DeviceConnectionError::None) return admitted;
            if (!wait(future)) return DeviceConnectionResult{DeviceConnectionError::BackendFailure, "Emulator completion timed out."};
            return future.get();
        };
        auto discovered = std::make_shared<std::promise<DeviceConnectionTargets>>(); auto targets = discovered->get_future();
        QCOMPARE(connection->requestDiscover([discovered](DeviceConnectionTargets value) { discovered->set_value(std::move(value)); }).error,
            DeviceConnectionError::None);
        QVERIFY(wait(targets));
        const auto catalog = targets.get(); QCOMPARE(catalog.result.error, DeviceConnectionError::None);
        std::string emulatorId;
        for (const auto& target : catalog.targets)
            if (std::string(system.getCameraInfo(target.displayName).GetDeviceClass().c_str()) == "BaslerCamEmu") { emulatorId = target.targetId; break; }
        if (emulatorId.empty()) QSKIP("The pylon camera emulator is not installed.");
        const auto state = [&] { return run([&](auto done) { return connection->requestConnectionState(done); }); };
        const auto connect = [&](std::string id) { return run([&](auto done) { return connection->requestConnect(id, done); }); };
        const auto disconnect = [&](std::string id) { return run([&](auto done) { return connection->requestDisconnect(id, done); }); };
        const auto initial = state(); QVERIFY(initial.state && !initial.state->connected);
        QCOMPARE(connect("missing").error, DeviceConnectionError::NotFound);
        auto connected = connect(emulatorId); QCOMPARE(connected.error, DeviceConnectionError::None);
        QVERIFY(connected.state->connected); QCOMPARE(connected.state->targetId, emulatorId);
        const auto first = connected.state->connectionId; QVERIFY(!first.empty());
        QCOMPARE(connect(emulatorId).error, DeviceConnectionError::Conflict);
        QCOMPARE(disconnect(first + "x").error, DeviceConnectionError::Conflict);
        std::vector<DeviceParameterDescriptor> parameters; QCOMPARE(controller.parameters(&parameters).error, DeviceParameterError::None);
        // One opaque id addresses both the lifecycle and the parameter targets of a connection.
        QVERIFY(!parameters.empty()); QCOMPARE(parameters.front().target.connectionId, first);
        const auto staleParameter = parameters.front().target;
        const auto disconnected = disconnect(first); QCOMPARE(disconnected.error, DeviceConnectionError::None);
        QVERIFY(!disconnected.state->connected); QVERIFY(disconnected.state->connectionId.empty());
        QCOMPARE(disconnect(first).error, DeviceConnectionError::Disconnected);
        connected = connect(emulatorId); QCOMPARE(connected.error, DeviceConnectionError::None);
        const auto second = connected.state->connectionId; QVERIFY(second != first);
        QCOMPARE(controller.readParameter(staleParameter).error, DeviceParameterError::Conflict);
        QCOMPARE(disconnect(first).error, DeviceConnectionError::Conflict);
        std::promise<void> entered, release; auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
        auto holding = std::async(std::launch::async, [&] { return camera->accessFeatures([&](GenApi::INodeMap&, std::uint64_t) { entered.set_value(); released.wait(); }); });
        QVERIFY(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto busy = disconnect(second);
        release.set_value(); QVERIFY(holding.get());
        QCOMPARE(busy.error, DeviceConnectionError::Busy); QVERIFY(camera->connectionState().connected);
        QCOMPARE(disconnect(second).error, DeviceConnectionError::None);
    }
    void connectionStateReportsItsSourceAndDeliveredFrames() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);
        const auto wait = [](auto& future) {
            const bool ready = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
            QCoreApplication::processEvents();
            return ready;
        };
        const auto run = [&](auto admit) {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto future = promise->get_future();
            const auto admitted = admit([promise](DeviceConnectionResult result) {
                promise->set_value(std::move(result));
            });
            if (admitted.error != DeviceConnectionError::None) return admitted;
            if (!wait(future)) return DeviceConnectionResult{DeviceConnectionError::BackendFailure,
                "Emulator completion timed out."};
            return future.get();
        };
        const auto emulator = findEmulator(system);
        std::string emulatorId;
        for (const auto& target : camera->deviceCatalog(true))
            if (target.displayName == emulator) { emulatorId = target.id; break; }
        if (emulatorId.empty()) QSKIP("The pylon camera emulator is not installed.");

        const auto initial = run([&](auto done) { return connection->requestConnectionState(done); });
        QVERIFY(initial.state && !initial.state->connected);
        QVERIFY(controller.sourceDescriptors().empty());

        const auto connected = run([&](auto done) { return connection->requestConnect(emulatorId, done); });
        QCOMPARE(connected.error, DeviceConnectionError::None);
        QVERIFY(connected.state && connected.state->connected);
        QCOMPARE(connected.state->sources.size(), std::size_t(1));
        QCOMPARE(connected.state->sources.front().sourceId, emulatorId);
        QCOMPARE(connected.state->sources.front().sourceIndex, 0U);
        QVERIFY(connected.state->deliveredFrames.has_value());
        QCOMPARE(*connected.state->deliveredFrames, std::uint64_t(0));
        const auto descriptors = controller.sourceDescriptors();
        QCOMPARE(descriptors.size(), std::size_t(1));
        QCOMPARE(descriptors.front().id, emulatorId);
        QCOMPARE(descriptors.front().index, 0U);
        const auto connectionId = connected.state->connectionId;

        QCOMPARE(run([&](auto done) { return connection->requestLiveStart(connectionId, done); }).error,
            DeviceConnectionError::None);
        std::optional<std::uint64_t> firstDelivered;
        QElapsedTimer firstTimer;
        firstTimer.start();
        while (firstTimer.elapsed() < 15000) {
            const auto diagnostics = controller.sourceRuntimeDiagnostics(0);
            const auto observed = run([&](auto done) { return connection->requestConnectionState(done); });
            if (diagnostics && diagnostics->delivered > 0 && observed.state
                && observed.state->deliveredFrames && *observed.state->deliveredFrames > 0) {
                QVERIFY(observed.state->live);
                QCOMPARE(observed.state->sources.size(), std::size_t(1));
                QCOMPARE(observed.state->sources.front().sourceId, emulatorId);
                QCOMPARE(observed.state->sources.front().sourceIndex, 0U);
                firstDelivered = *observed.state->deliveredFrames;
                break;
            }
            QTest::qWait(20);
        }
        QVERIFY2(firstDelivered.has_value(),
            "The connection state did not report the source.diagnostics delivered count.");
        bool grew = false;
        QElapsedTimer growthTimer;
        growthTimer.start();
        while (growthTimer.elapsed() < 15000 && !grew) {
            const auto observed = run([&](auto done) { return connection->requestConnectionState(done); });
            grew = observed.state && observed.state->deliveredFrames
                && *observed.state->deliveredFrames > *firstDelivered;
            if (!grew) QTest::qWait(50);
        }
        QVERIFY2(grew, "The connection state's delivered frame count did not grow while Live.");
        QCOMPARE(run([&](auto done) { return connection->requestLiveStop(connectionId, done); }).error,
            DeviceConnectionError::None);

        const auto disconnected = run([&](auto done) { return connection->requestDisconnect(connectionId, done); });
        QCOMPARE(disconnected.error, DeviceConnectionError::None);
        QVERIFY(disconnected.state && !disconnected.state->connected);
        QVERIFY(disconnected.state->sources.empty());
        QVERIFY(controller.sourceDescriptors().empty());
        camera->close();
    }
    void removedCameraReleasesItsClaimBeforeAnotherTarget() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        auto* camera = system.addCamera(); QVERIFY(camera);
        TestClaims claims;
        std::shared_ptr<IDevicePluginResourceClaim> held;
        auto controller = std::make_unique<CameraSourceController>(camera, &claims, &held);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        const auto wait = [](auto& future) {
            const bool ready = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
            QCoreApplication::processEvents();
            return ready;
        };
        const auto run = [&](auto admit) {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto future = promise->get_future();
            const auto admitted = admit([promise](DeviceConnectionResult result) {
                promise->set_value(std::move(result));
            });
            if (admitted.error != DeviceConnectionError::None) return admitted;
            if (!wait(future))
                return DeviceConnectionResult{DeviceConnectionError::BackendFailure,
                    "Emulator completion timed out."};
            return future.get();
        };
        auto discovered = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targets = discovered->get_future();
        QCOMPARE(connection->requestDiscover([discovered](DeviceConnectionTargets value) {
            discovered->set_value(std::move(value));
        }).error, DeviceConnectionError::None);
        QVERIFY(wait(targets));
        const auto catalog = targets.get();
        QCOMPARE(catalog.result.error, DeviceConnectionError::None);
        std::vector<std::pair<std::string, QString>> emulators;
        for (const auto& target : catalog.targets) {
            const auto info = system.getCameraInfo(target.displayName);
            if (std::string(info.GetDeviceClass().c_str()) != "BaslerCamEmu") continue;
            emulators.emplace_back(target.targetId,
                playgroundCameraResourceKey(QString::fromStdString(info.GetSerialNumber().c_str())));
        }
        if (emulators.size() < 2) QSKIP("The pylon two-camera emulator is not configured.");
        const auto connect = [&](const std::string& id) {
            return run([&](auto done) { return connection->requestConnect(id, done); });
        };
        const auto disconnected = [&](const std::string& id) {
            return run([&](auto done) { return connection->requestDisconnect(id, done); });
        };

        auto connected = connect(emulators.at(0).first);
        QCOMPARE(connected.error, DeviceConnectionError::None);
        QVERIFY(held && held->key() == emulators.at(0).second);
        QCOMPARE(claims.held(), QSet<QString>({emulators.at(0).second}));

        // A camera removal invalidates the connection without a disconnect request. The next
        // target must release the old key before claiming its own, or a sibling session could
        // open the removed camera while this session still holds its key.
        camera->close();
        connected = connect(emulators.at(1).first);
        QCOMPARE(connected.error, DeviceConnectionError::None);
        QCOMPARE(claims.held(), QSet<QString>({emulators.at(1).second}));
        QVERIFY(held && held->key() == emulators.at(1).second);

        // Disconnecting an already invalidated connection also releases the stale key.
        camera->close();
        const auto stale = disconnected(connected.state->connectionId);
        QCOMPARE(stale.error, DeviceConnectionError::Disconnected);
        QVERIFY(claims.held().isEmpty());
        QVERIFY(!held);

        held.reset();
        controller.reset();
        system.removeCamera(camera);
    }

    void concurrentConnectRequestsKeepOneClaim() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        auto* camera = system.addCamera(); QVERIFY(camera);
        TestClaims claims;
        std::shared_ptr<IDevicePluginResourceClaim> held;
        auto controller = std::make_unique<CameraSourceController>(camera, &claims, &held);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        const auto wait = [](auto& future) {
            const bool ready = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
            QCoreApplication::processEvents();
            return ready;
        };
        auto discovered = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targets = discovered->get_future();
        QCOMPARE(connection->requestDiscover([discovered](DeviceConnectionTargets value) {
            discovered->set_value(std::move(value));
        }).error, DeviceConnectionError::None);
        QVERIFY(wait(targets));
        const auto catalog = targets.get();
        QCOMPARE(catalog.result.error, DeviceConnectionError::None);
        std::string emulatorId;
        QString emulatorKey;
        for (const auto& target : catalog.targets) {
            const auto info = system.getCameraInfo(target.displayName);
            if (std::string(info.GetDeviceClass().c_str()) != "BaslerCamEmu") continue;
            emulatorId = target.targetId;
            emulatorKey = playgroundCameraResourceKey(
                QString::fromStdString(info.GetSerialNumber().c_str()));
            break;
        }
        if (emulatorId.empty()) QSKIP("The pylon camera emulator is not installed.");

        // Let the finished discovery worker leave the bounded worker set before the burst.
        QTest::qWait(100);
        std::vector<std::future<DeviceConnectionResult>> futures;
        for (int index = 0; index < 7; ++index) {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
            futures.push_back(promise->get_future());
            const auto admitted = connection->requestConnect(emulatorId,
                [promise](DeviceConnectionResult result) {
                    promise->set_value(std::move(result));
                });
            QCOMPARE(admitted.error, DeviceConnectionError::None);
        }
        int successes = 0;
        int conflicts = 0;
        DeviceConnectionResult winner;
        for (auto& future : futures) {
            QVERIFY(wait(future));
            auto result = future.get();
            if (result.error == DeviceConnectionError::None) {
                ++successes;
                winner = std::move(result);
            } else if (result.error == DeviceConnectionError::Conflict) {
                ++conflicts;
            }
        }
        QCOMPARE(successes, 1);
        QCOMPARE(conflicts, 6);
        QCOMPARE(claims.held(), QSet<QString>({emulatorKey}));
        QVERIFY(held && held->key() == emulatorKey);
        QVERIFY(winner.state && winner.state->connected);

        // The winning request still owns the claim after the losing requests returned.
        auto disconnected = std::make_shared<std::promise<DeviceConnectionResult>>();
        auto disconnectFuture = disconnected->get_future();
        QCOMPARE(connection->requestDisconnect(winner.state->connectionId,
            [disconnected](DeviceConnectionResult result) {
                disconnected->set_value(std::move(result));
            }).error, DeviceConnectionError::None);
        QVERIFY(wait(disconnectFuture));
        QCOMPARE(disconnectFuture.get().error, DeviceConnectionError::None);
        QVERIFY(claims.held().isEmpty());
        QVERIFY(!held);

        controller.reset();
        system.removeCamera(camera);
    }

    void emulatedSdkParametersAndConnectionLifetime() {
        // Only an explicitly selected pylon emulator is opened, even if real devices exist.
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        std::string emulator;
        for (const auto& name : system.getCameraList()) {
            if (std::string(system.getCameraInfo(name).GetDeviceClass().c_str()) == "BaslerCamEmu") {
                emulator = name; break;
            }
        }
        if (emulator.empty()) QSKIP("The pylon camera emulator is not installed.");
        auto* camera = system.addCamera();
        QVERIFY(camera != nullptr);
        CameraSourceController controller(camera);
        auto* capability = qobject_cast<IDeviceParameterControl*>(&controller);
        QVERIFY(capability);
        auto asynchronousInventory = std::make_shared<std::promise<DeviceParameterInventory>>();
        auto inventoryFuture = asynchronousInventory->get_future();
        QVERIFY(capability->requestParameters([asynchronousInventory](DeviceParameterInventory value) {
            asynchronousInventory->set_value(std::move(value));
        }).error == DeviceParameterError::None);
        QVERIFY(inventoryFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QCOMPARE(inventoryFuture.get().result.error, DeviceParameterError::Disconnected);
        std::vector<DeviceParameterDescriptor> inventory;
        QCOMPARE(controller.parameters(&inventory).error, DeviceParameterError::Disconnected);
        QVERIFY(camera->open(emulator));
        const auto listed = controller.parameters(&inventory);
        QVERIFY2(listed.error == DeviceParameterError::None, listed.message.c_str());
        QVERIFY(!inventory.empty());
        const auto found = std::find_if(inventory.begin(), inventory.end(), [](const auto& feature) {
            return feature.target.name == "Width";
        });
        QVERIFY(found != inventory.end());
        const auto initial = *found;
        auto asynchronousRead = std::make_shared<std::promise<DeviceParameterResult>>();
        auto readFuture = asynchronousRead->get_future();
        QVERIFY(capability->requestReadParameter(initial.target, [asynchronousRead](DeviceParameterResult value) {
            asynchronousRead->set_value(std::move(value));
        }).error == DeviceParameterError::None);
        QVERIFY(readFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(readFuture.get().descriptor.has_value());
        QCOMPARE(initial.kind, DeviceParameterKind::Integer);
        QVERIFY(initial.writable && initial.readable && initial.integerMinimum && initial.integerIncrement);
        {
            std::promise<void> entered;
            auto enteredFuture = entered.get_future();
            std::promise<void> release;
            auto released = release.get_future().share();
            auto worker = std::async(std::launch::async, [&] {
                return camera->accessFeatures([&](GenApi::INodeMap&, std::uint64_t) {
                    entered.set_value();
                    released.wait();
                });
            });
            struct ReleaseGuard {
                std::promise<void>& promise;
                ~ReleaseGuard() { promise.set_value(); }
            } releaseGuard{release};
            QVERIFY(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            QCOMPARE(controller.parameters(&inventory).error, DeviceParameterError::Busy);
            const auto busy = controller.writeParameter(initial.target, *initial.integerMinimum, {});
            QCOMPARE(busy.error, DeviceParameterError::Busy);
            QCOMPARE(busy.effect, DeviceParameterEffect::None);
        }
        int notifications = 0;
        const auto callback = camera->registerNodeUpdatedCallback([&](const std::string& name) {
            if (name == "Root") ++notifications;
        });
        const auto wrongType = controller.writeParameter(initial.target, 1.0, initial.revision);
        QCOMPARE(wrongType.error, DeviceParameterError::InvalidArgument);
        QCOMPARE(wrongType.effect, DeviceParameterEffect::None);
        const auto requested = *initial.integerMinimum;
        const auto written = controller.writeParameter(initial.target, requested, initial.revision);
        QVERIFY2(written.error == DeviceParameterError::None, written.message.c_str());
        QCOMPARE(written.effect, DeviceParameterEffect::Applied);
        QVERIFY(written.descriptor && written.descriptor->value);
        QCOMPARE(std::get<std::int64_t>(*written.descriptor->value), requested);
        QVERIFY(notifications >= 1);
        const auto reread = controller.readParameter(initial.target);
        QVERIFY(reread.descriptor);
        QCOMPARE(reread.descriptor->revision, written.descriptor->revision);
        QVERIFY(camera->isOpened());
        QVERIFY(!controller.isGrabbing());
        const auto stale = initial.target;
        camera->close();
        QCOMPARE(controller.readParameter(stale).error, DeviceParameterError::Disconnected);
        QVERIFY(camera->open(emulator));
        QCOMPARE(controller.readParameter(stale).error, DeviceParameterError::Conflict);
        QCOMPARE(controller.writeParameter(stale, requested, {}).error, DeviceParameterError::Conflict);
        camera->deregisterNodeUpdatedCallback(callback);
        camera->close();
    }
    void parameterTreeCarriesGenApiStructure() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        const auto emulator = findEmulator(system);
        if (emulator.empty()) QSKIP("The pylon camera emulator is not installed.");
        auto* camera = system.addCamera(); QVERIFY(camera);
        {
            CameraSourceController controller(camera);
            QVERIFY(camera->open(emulator));
            std::vector<DeviceParameterDescriptor> inventory;
            const auto listed = controller.parameters(&inventory);
            QVERIFY2(listed.error == DeviceParameterError::None, listed.message.c_str());
            QVERIFY(!inventory.empty());
            std::map<std::string, ExpectedTree> expected;
            QVERIFY(camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t) {
                GenApi::NodeList_t nodes;
                map.GetNodes(nodes);
                for (auto* node : nodes) {
                    const auto type = node->GetPrincipalInterfaceType();
                    if (type != GenApi::intfIInteger && type != GenApi::intfIFloat
                        && type != GenApi::intfIBoolean && type != GenApi::intfIString
                        && type != GenApi::intfIEnumeration && type != GenApi::intfICommand)
                        continue;
                    DeviceParameterKind kind = DeviceParameterKind::String;
                    switch (type) {
                    case GenApi::intfIInteger: kind = DeviceParameterKind::Integer; break;
                    case GenApi::intfIFloat: kind = DeviceParameterKind::Float; break;
                    case GenApi::intfIBoolean: kind = DeviceParameterKind::Boolean; break;
                    case GenApi::intfIEnumeration: kind = DeviceParameterKind::Enumeration; break;
                    case GenApi::intfICommand: kind = DeviceParameterKind::Command; break;
                    default: break;
                    }
                    expected.emplace(node->GetName().c_str(), expectedTree(node, kind));
                }
            }));
            std::size_t withCategory = 0, withRelations = 0, producerShared = 0;
            for (const auto& descriptor : inventory) {
                const auto found = expected.find(descriptor.target.name);
                QVERIFY2(found != expected.end(), descriptor.target.name.c_str());
                QCOMPARE(descriptor.categoryPath, found->second.categoryPath);
                QVERIFY(descriptor.visibility == found->second.visibility);
                QVERIFY(descriptor.representation == found->second.representation);
                QCOMPARE(descriptor.selectorRelations, found->second.relations);
                QCOMPARE(descriptor.producerShared, found->second.producerShared);
                std::string reason;
                QVERIFY2(deviceParameterTreeValid(descriptor, &reason), reason.c_str());
                if (!descriptor.categoryPath.empty()) ++withCategory;
                if (!descriptor.selectorRelations.empty()) ++withRelations;
                if (descriptor.producerShared) ++producerShared;
            }
            QVERIFY(withCategory > 0);
            QVERIFY(producerShared > 0);
            // LineSelector selects the per-line features; the emulator exposes that tree.
            QVERIFY2(withRelations > 0, "No descriptor carried a GenApi selector relationship.");
            camera->close();
        }
        system.removeCamera(camera);
    }
    void parameterChangesDeliverSnapshotDeltaAndWritability() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        const auto emulator = findEmulator(system);
        if (emulator.empty()) QSKIP("The pylon camera emulator is not installed.");
        auto* camera = system.addCamera(); QVERIFY(camera);
        {
            CameraSourceController controller(camera);
            auto* capability = qobject_cast<IDeviceParameterControl*>(&controller);
            auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller);
            QVERIFY(capability && connection);
            QVERIFY(camera->open(emulator));
            std::vector<DeviceParameterDescriptor> inventory;
            QVERIFY(controller.parameters(&inventory).error == DeviceParameterError::None);
            QVERIFY(!inventory.empty());
            const auto connectionId = inventory.front().target.connectionId;
            ChangeCollector collector;
            QCOMPARE(capability->requestParameterChanges(connectionId, "watch-1",
                collector.listener()).error, DeviceParameterError::None);
            QVERIFY(collector.waitFor(1, 5000));
            const auto snapshot = collector.at(0);
            QVERIFY(snapshot && snapshot->kind == DeviceParameterChangeKind::Snapshot);
            QVERIFY(!snapshot->resync);
            QCOMPARE(snapshot->sequence, std::uint64_t(1));
            QCOMPARE(snapshot->descriptors.size(), inventory.size());
            QCOMPARE(snapshot->subscriptionId, std::string("watch-1"));

            std::optional<WritableCandidate> written;
            for (const auto& descriptor : inventory) {
                if (descriptor.target.name != "Width") continue;
                written = writableChange(descriptor);
            }
            if (!written) {
                for (const auto& descriptor : inventory) {
                    written = writableChange(descriptor);
                    if (written) break;
                }
            }
            QVERIFY(written.has_value());
            const auto result = controller.writeParameter(written->descriptor.target,
                written->value, written->descriptor.revision);
            QVERIFY2(result.error == DeviceParameterError::None, result.message.c_str());
            std::uint64_t baseRevision = 0;
            for (const auto& descriptor : snapshot->descriptors)
                if (descriptor.target.name == written->descriptor.target.name)
                    baseRevision = descriptor.revision;
            const DeviceParameterValue expected = result.descriptor && result.descriptor->value
                ? *result.descriptor->value : written->value;
            bool sawWrittenDelta = false;
            QElapsedTimer deltaTimer;
            deltaTimer.start();
            while (!sawWrittenDelta && deltaTimer.elapsed() < 10000) {
                for (const auto& change : collector.snapshot()) {
                    if (change.kind != DeviceParameterChangeKind::Delta || change.resync) continue;
                    for (const auto& descriptor : change.descriptors) {
                        if (descriptor.target.name != written->descriptor.target.name) continue;
                        if (!descriptor.value || *descriptor.value != expected) continue;
                        sawWrittenDelta = change.sequence > snapshot->sequence
                            && descriptor.revision > baseRevision;
                    }
                }
                if (!sawWrittenDelta) QTest::qWait(20);
            }
            QVERIFY2(sawWrittenDelta, "The write did not deliver a revision-stamped delta.");

            // An active subscription must not turn a following write into a Busy refusal while
            // the observer catches up; no caller-side retry is involved here.
            std::set<std::string> writtenNames{written->descriptor.target.name};
            for (int round = 0; round < 3; ++round) {
                std::optional<WritableCandidate> next;
                for (const auto& descriptor : inventory) {
                    if (writtenNames.count(descriptor.target.name)) continue;
                    next = writableChange(descriptor);
                    if (next) break;
                }
                QVERIFY(next.has_value());
                writtenNames.insert(next->descriptor.target.name);
                QCOMPARE(controller.writeParameter(next->descriptor.target, next->value, {}).error,
                    DeviceParameterError::None);
                QTest::qWait(20);
            }

            const auto waitTransition = [&](const bool from, const bool to) {
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 15000) {
                    if (sawWritabilityTransition(collector.snapshot(), from, to)) return true;
                    QTest::qWait(50);
                }
                return false;
            };
            auto live = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto liveFuture = live->get_future();
            QCOMPARE(connection->requestLiveStart(connectionId,
                [live](DeviceConnectionResult value) { live->set_value(std::move(value)); }).error,
                DeviceConnectionError::None);
            QVERIFY(liveFuture.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
            QCOMPARE(liveFuture.get().error, DeviceConnectionError::None);
            QVERIFY2(waitTransition(true, false),
                "Live start did not deliver a writability delta from the device lock.");
            auto stopped = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto stoppedFuture = stopped->get_future();
            QCOMPARE(connection->requestLiveStop(connectionId,
                [stopped](DeviceConnectionResult value) { stopped->set_value(std::move(value)); }).error,
                DeviceConnectionError::None);
            QVERIFY(stoppedFuture.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
            QCOMPARE(stoppedFuture.get().error, DeviceConnectionError::None);
            QVERIFY2(waitTransition(false, true),
                "Live stop did not re-observe the unlocked writability.");

            auto released = std::make_shared<std::promise<DeviceParameterResult>>();
            auto releasedFuture = released->get_future();
            QCOMPARE(capability->releaseParameterChanges(connectionId, "watch-1",
                [released](DeviceParameterResult value) { released->set_value(std::move(value)); }).error,
                DeviceParameterError::None);
            QVERIFY(releasedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            QCOMPARE(releasedFuture.get().error, DeviceParameterError::None);
            const auto delivered = collector.size();
            QTest::qWait(300);
            QCOMPARE(collector.size(), delivered);
            QCOMPARE(capability->releaseParameterChanges(connectionId, "watch-1",
                [](DeviceParameterResult) {}).error, DeviceParameterError::NotFound);
            camera->close();
        }
        system.removeCamera(camera);
    }
    void parameterChangeOverflowResyncs() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        const auto emulator = findEmulator(system);
        if (emulator.empty()) QSKIP("The pylon camera emulator is not installed.");
        auto* camera = system.addCamera(); QVERIFY(camera);
        {
            CameraSourceController controller(camera);
            auto* capability = qobject_cast<IDeviceParameterControl*>(&controller);
            QVERIFY(capability);
            QVERIFY(camera->open(emulator));
            std::vector<DeviceParameterDescriptor> inventory;
            QVERIFY(controller.parameters(&inventory).error == DeviceParameterError::None);
            QVERIFY(!inventory.empty());
            const auto connectionId = inventory.front().target.connectionId;
            ChangeCollector collector;
            QCOMPARE(capability->requestParameterChanges(connectionId, "watch-overflow",
                collector.listener()).error, DeviceParameterError::None);
            QVERIFY(collector.waitFor(1, 5000));
            collector.holdDeltas(true);
            std::optional<WritableCandidate> held;
            for (const auto& descriptor : inventory) {
                if (descriptor.target.name != "Width") continue;
                held = writableChange(descriptor);
            }
            QVERIFY(held.has_value());
            QCOMPARE(controller.writeParameter(held->descriptor.target, held->value,
                held->descriptor.revision).error, DeviceParameterError::None);
            QVERIFY(collector.waitUntilHeld(5000));

            std::size_t changed = 0;
            std::set<std::string> used{held->descriptor.target.name};
            for (const auto& descriptor : inventory) {
                if (changed >= deviceParameterChangeQueueLimit + 4) break;
                if (used.count(descriptor.target.name)) continue;
                const auto candidate = writableChange(descriptor);
                if (!candidate) continue;
                QCOMPARE(controller.writeParameter(candidate->descriptor.target,
                    candidate->value, {}).error, DeviceParameterError::None);
                used.insert(descriptor.target.name);
                ++changed;
                // Let the observer turn this write into its own delta while delivery is held.
                QTest::qWait(40);
            }
            collector.releaseHeld();
            if (changed < deviceParameterChangeQueueLimit + 1) {
                collector.holdDeltas(false);
                QSKIP(qPrintable(QStringLiteral(
                    "The emulator exposes only %1 writable scalar features; the queue cannot overflow.")
                    .arg(changed)));
            }
            QVERIFY(collector.waitForResync(15000));
            bool sawResync = false;
            for (const auto& change : collector.snapshot()) {
                if (!change.resync) continue;
                sawResync = true;
                QCOMPARE(change.kind, DeviceParameterChangeKind::Snapshot);
                QVERIFY(!change.overflowReason.empty());
                QVERIFY(change.sequence > 1);
                QVERIFY(change.descriptors.size() >= inventory.size());
            }
            QVERIFY2(sawResync, "The bounded change queue did not deliver a resync snapshot.");
            collector.holdDeltas(false);
            QCOMPARE(capability->releaseParameterChanges(connectionId, "watch-overflow",
                [](DeviceParameterResult) {}).error, DeviceParameterError::None);
            camera->close();
        }
        system.removeCamera(camera);
    }
    void emulatedSingleAndLiveFollowAcquisitionMode() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system;
        std::string emulator;
        for (const auto& name : system.getCameraList()) {
            if (std::string(system.getCameraInfo(name).GetDeviceClass().c_str()) == "BaslerCamEmu") {
                emulator = name; break;
            }
        }
        if (emulator.empty()) QSKIP("The pylon camera emulator is not installed.");
        auto* camera = system.addCamera(); QVERIFY(camera);
        {
        CameraSourceController controller(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(&controller); QVERIFY(connection);
        const auto run = [](auto& future) {
            const bool ready = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
            QCoreApplication::processEvents();
            return ready;
        };
        const auto invoke = [&](auto admit) {
            auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
            auto future = promise->get_future();
            const auto admitted = admit([promise](DeviceConnectionResult result) { promise->set_value(std::move(result)); });
            if (admitted.error != DeviceConnectionError::None) return admitted;
            if (!run(future)) return DeviceConnectionResult{DeviceConnectionError::BackendFailure,
                "Emulator completion timed out."};
            return future.get();
        };
        auto discovered = std::make_shared<std::promise<DeviceConnectionTargets>>();
        auto targets = discovered->get_future();
        QCOMPARE(connection->requestDiscover([discovered](DeviceConnectionTargets value) {
            discovered->set_value(std::move(value));
        }).error, DeviceConnectionError::None);
        QVERIFY(run(targets));
        const auto catalog = targets.get();
        QCOMPARE(catalog.result.error, DeviceConnectionError::None);
        std::string emulatorId;
        for (const auto& target : catalog.targets)
            if (std::string(system.getCameraInfo(target.displayName).GetDeviceClass().c_str()) == "BaslerCamEmu") {
                emulatorId = target.targetId; break;
            }
        QVERIFY(!emulatorId.empty());
        const auto connected = invoke([&](auto done) { return connection->requestConnect(emulatorId, done); });
        QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
        QVERIFY(connected.state && connected.state->connected);
        const auto connectionId = connected.state->connectionId;
        const auto capabilities = connection->connectionCapabilities();
        QVERIFY2(capabilities.single.available, capabilities.single.reason.c_str());

        std::atomic<int> frames{0};
        controller.setFrameConsumer([&frames](SessionFrame&&, unsigned) { frames.fetch_add(1); });
        const auto waitFrames = [&](const int minimum) {
            QElapsedTimer timer; timer.start();
            while (frames.load() < minimum && timer.elapsed() < 30000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            return frames.load() >= minimum;
        };
        const auto readMode = [&](std::string* value, std::string* error) {
            std::vector<DeviceParameterDescriptor> inventory;
            const auto listed = controller.parameters(&inventory);
            if (listed.error != DeviceParameterError::None) {
                *error = listed.message;
                return false;
            }
            const auto found = std::find_if(inventory.begin(), inventory.end(), [](const auto& feature) {
                return feature.target.name == "AcquisitionMode";
            });
            if (found == inventory.end() || !found->value) {
                *error = "AcquisitionMode was not observed.";
                return false;
            }
            *value = std::get<std::string>(*found->value);
            return true;
        };

        auto single = invoke([&](auto done) { return connection->requestSingle(connectionId, done); });
        QVERIFY2(single.error == DeviceConnectionError::None, single.message.c_str());
        QCOMPARE(frames.load(), 1);
        std::string mode, modeError;
        QVERIFY2(readMode(&mode, &modeError), modeError.c_str());
        QCOMPARE(mode, std::string("SingleFrame"));

        frames.store(0);
        auto live = invoke([&](auto done) { return connection->requestLiveStart(connectionId, done); });
        QVERIFY2(live.error == DeviceConnectionError::None, live.message.c_str());
        // A parameter write while Live can observe AcquisitionMode as locked; stop must
        // re-observe it before Single availability is offered again.
        camera->notifyFeaturesChanged();
        QVERIFY(waitFrames(2));
        auto stopped = invoke([&](auto done) { return connection->requestLiveStop(connectionId, done); });
        QVERIFY2(stopped.error == DeviceConnectionError::None, stopped.message.c_str());
        const auto afterStop = connection->connectionCapabilities();
        QVERIFY2(afterStop.single.available, afterStop.single.reason.c_str());
        QVERIFY2(readMode(&mode, &modeError), modeError.c_str());
        QCOMPARE(mode, std::string("Continuous"));

        frames.store(0);
        single = invoke([&](auto done) { return connection->requestSingle(connectionId, done); });
        QVERIFY2(single.error == DeviceConnectionError::None, single.message.c_str());
        QCOMPARE(frames.load(), 1);
        QVERIFY2(readMode(&mode, &modeError), modeError.c_str());
        QCOMPARE(mode, std::string("SingleFrame"));

        // While Live runs, Single refuses explicitly instead of emulating it by stopping the stream.
        QCOMPARE(invoke([&](auto done) { return connection->requestLiveStart(connectionId, done); }).error,
            DeviceConnectionError::None);
        const auto refused = invoke([&](auto done) { return connection->requestSingle(connectionId, done); });
        QCOMPARE(refused.error, DeviceConnectionError::Busy);
        QVERIFY2(refused.message == "Stop Live before a single grab.", refused.message.c_str());
        QCOMPARE(invoke([&](auto done) { return connection->requestLiveStop(connectionId, done); }).error,
            DeviceConnectionError::None);
        controller.setFrameConsumer({});
        QCOMPARE(invoke([&](auto done) { return connection->requestDisconnect(connectionId, done); }).error,
            DeviceConnectionError::None);
        }
        system.removeCamera(camera);
    }
};

QTEST_GUILESS_MAIN(CameraParameterControlTests)
#include "ParameterControlTest.moc"
