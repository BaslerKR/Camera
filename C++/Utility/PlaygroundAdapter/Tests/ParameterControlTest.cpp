#include "CameraSourceController.h"
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
    void declaredToolContractRejectsMalformedCallsAndCompletesExactlyOnce() {
        auto* camera = _system->addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera);
        std::atomic<int> completions{0};
        const auto unused = [&](ApiResult<QJsonValue>) { ++completions; };
        QCOMPARE(controller->requestTool("device.connection", 2, {}, unused).error()->code, ApiErrorCode::Unsupported);
        QCOMPARE(controller->requestTool("device.unknown", 1, {}, unused).error()->code, ApiErrorCode::NotFound);
        QCOMPARE(controller->requestTool("device.connection", 1, {}, {}).error()->code, ApiErrorCode::InvalidArgument);
        QCOMPARE(controller->requestTool("device.connection", 1, {{"refresh", true}}, unused).error()->code, ApiErrorCode::InvalidArgument);
        for (const auto generation : {QJsonValue(0), QJsonValue("-1"), QJsonValue("00"), QJsonValue("18446744073709551616"), QJsonValue(" 1")})
            QCOMPARE(controller->requestTool("device.disconnect", 1, {{"expectedGeneration", generation}}, unused).error()->code, ApiErrorCode::InvalidArgument);
        QCOMPARE(controller->requestTool("device.connect", 1, {{"expectedGeneration", "0"}, {"deviceId", ""}}, unused).error()->code, ApiErrorCode::InvalidArgument);
        QCOMPARE(completions.load(), 0); QVERIFY(!camera->connectionState().connected);
        std::promise<void> entered; auto completed = entered.get_future();
        QVERIFY(controller->requestTool("device.connection", 1, {}, [&](ApiResult<QJsonValue>) {
            const auto count = ++completions;
            if (count == 1) { entered.set_value(); throw std::runtime_error("consumer completion failed"); }
        }).succeeded());
        QVERIFY(completed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        controller.reset(); // Drain the accepted worker before inspecting the final callback count.
        QCOMPARE(completions.load(), 1);
        _system->removeCamera(camera);
    }
    void acceptedToolWorkDrainsOnControllerDestruction() {
        auto* camera = _system->addCamera(); QVERIFY(camera);
        auto controller = std::make_unique<CameraSourceController>(camera);
        std::promise<void> allEntered, release; auto entered = allEntered.get_future(); auto released = release.get_future().share();
        std::atomic<int> started{0}, completed{0}, refusedCallback{0};
        for (int i = 0; i < 8; ++i) {
            QVERIFY(controller->requestTool("device.connection", 1, {}, [&](ApiResult<QJsonValue> result) {
                if (++started == 8) allEntered.set_value();
                released.wait();
                if (result.succeeded()) ++completed;
            }).succeeded());
        }
        const bool ready = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!ready) release.set_value();
        QVERIFY(ready);
        const auto refused = controller->requestTool("device.connection", 1, {}, [&](ApiResult<QJsonValue>) { ++refusedCallback; });
        std::thread releaser([&] { release.set_value(); });
        controller.reset(); releaser.join();
        QVERIFY(!refused.succeeded()); QCOMPARE(refused.error()->code, ApiErrorCode::Busy);
        QCOMPARE(completed.load(), 8); QCOMPARE(refusedCallback.load(), 0);
        _system->removeCamera(camera);
        QCoreApplication::processEvents(); // No late callback may access the deleted controller.
    }
    void declaredLifecycleToolsUseExactEmulatedTargets() {
        qputenv("PYLON_CAMEMU", "1");
        auto& system = *_system; auto* camera = system.addCamera(); QVERIFY(camera);
        CameraSourceController controller(camera);
        auto* provider = qobject_cast<IToolProvider*>(&controller); QVERIFY(provider);
        QCOMPARE(provider->toolDescriptors().size(), 5);
        const auto invoke = [&](const QString& name, QJsonObject arguments = {}) {
            auto promise = std::make_shared<std::promise<ApiResult<QJsonValue>>>(); auto future = promise->get_future();
            const auto admitted = provider->requestTool(name, 1, arguments, [promise](ApiResult<QJsonValue> result) { promise->set_value(std::move(result)); });
            if (!admitted.succeeded()) return ApiResult<QJsonValue>::failure(admitted.error()->code, admitted.error()->message);
            if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return ApiResult<QJsonValue>::failure(ApiErrorCode::Timeout, "Emulator completion timed out.");
            auto result = future.get(); QCoreApplication::processEvents(); return result;
        };
        const auto targets = invoke("device.discover"); QVERIFY2(targets.succeeded(), qPrintable(targets.error() ? targets.error()->message : QString()));
        QString emulatorId;
        for (const auto value : targets.value().toArray()) {
            const auto target = value.toObject();
            if (std::string(system.getCameraInfo(target.value("displayName").toString().toStdString()).GetDeviceClass().c_str()) == "BaslerCamEmu") {
                emulatorId = target.value("id").toString(); break;
            }
        }
        if (emulatorId.isEmpty()) QSKIP("The pylon camera emulator is not installed.");
        const auto initial = invoke("device.connection"); QVERIFY(initial.succeeded());
        QCOMPARE(initial.value().toObject().value("connected").toBool(), false);
        const auto initialGeneration = initial.value().toObject().value("connectionGeneration");
        QCOMPARE(invoke("device.connect", {{"deviceId", "missing"}, {"expectedGeneration", initialGeneration}}).error()->code, ApiErrorCode::NotFound);
        QCOMPARE(invoke("device.connect", {{"deviceId", emulatorId}, {"expectedGeneration", "00"}}).error()->code, ApiErrorCode::InvalidArgument);
        auto connected = invoke("device.connect", {{"deviceId", emulatorId}, {"expectedGeneration", initialGeneration}}); QVERIFY(connected.succeeded());
        QVERIFY(connected.value().toObject().value("connected").toBool());
        QCOMPARE(connected.value().toObject().value("deviceId").toString(), emulatorId);
        const auto generation = connected.value().toObject().value("connectionGeneration");
        QCOMPARE(invoke("device.connect", {{"deviceId", emulatorId}, {"expectedGeneration", generation}}).error()->code, ApiErrorCode::Conflict);
        QCOMPARE(invoke("device.disconnect", {{"expectedGeneration", initialGeneration}}).error()->code, ApiErrorCode::Conflict);
        std::vector<DeviceParameterDescriptor> parameters; QCOMPARE(controller.parameters(&parameters).error, DeviceParameterError::None);
        QVERIFY(!parameters.empty()); const auto staleParameter = parameters.front().target;
        const auto disconnected = invoke("device.disconnect", {{"expectedGeneration", generation}}); QVERIFY(disconnected.succeeded());
        QVERIFY(!disconnected.value().toObject().value("connected").toBool());
        connected = invoke("device.connect", {{"deviceId", emulatorId}, {"expectedGeneration", generation}}); QVERIFY(connected.succeeded());
        QVERIFY(connected.value().toObject().value("connectionGeneration") != generation);
        QCOMPARE(controller.readParameter(staleParameter).error, DeviceParameterError::Conflict);
        const auto finalGeneration = connected.value().toObject().value("connectionGeneration");
        std::promise<void> entered, release; auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
        auto holding = std::async(std::launch::async, [&] { return camera->accessFeatures([&](GenApi::INodeMap&, std::uint64_t) { entered.set_value(); released.wait(); }); });
        QVERIFY(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto busy = invoke("device.disconnect", {{"expectedGeneration", finalGeneration}});
        release.set_value(); QVERIFY(holding.get());
        QVERIFY(!busy.succeeded()); QCOMPARE(busy.error()->code, ApiErrorCode::Busy); QVERIFY(camera->connectionState().connected);
        QVERIFY(invoke("device.disconnect", {{"expectedGeneration", finalGeneration}}).succeeded());
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
