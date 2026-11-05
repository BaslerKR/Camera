#include "CameraSourceController.h"
#include "ToolProvider.h"
#include "CameraSystem.h"
#include <QtTest>
#include <algorithm>
#include <future>
#include <chrono>
#include <memory>
#include <atomic>
#include <stdexcept>

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
    void connectionCapabilityRejectsMalformedCallsAndCompletesExactlyOnce() {
        auto* camera = _system->addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera);
        auto* connection = qobject_cast<IDeviceConnectionControl*>(controller.get()); QVERIFY(connection);
        QVERIFY(!qobject_cast<IToolProvider*>(controller.get())); // Lifecycle tools are host-declared.
        const auto capabilities = connection->connectionCapabilities();
        QVERIFY(capabilities.targets.available && capabilities.discover.available && capabilities.connect.available);
        QVERIFY(!capabilities.single.available && !capabilities.single.reason.empty());
        QVERIFY(!capabilities.live.available && !capabilities.live.reason.empty());
        std::atomic<int> completions{0}, disconnectedStates{0};
        const auto unused = [&](DeviceConnectionResult) { ++completions; };
        QCOMPARE(connection->requestConnectionState({}).error, DeviceConnectionError::InvalidArgument);
        QCOMPARE(connection->requestSingle("x", unused).error, DeviceConnectionError::Unsupported);
        QCOMPARE(connection->requestLiveStart("x", unused).error, DeviceConnectionError::Unsupported);
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
};

QTEST_GUILESS_MAIN(CameraParameterControlTests)
#include "ParameterControlTest.moc"
