#include "DevicePluginSession.h"

#include "Camera.h"
#include "CameraSystem.h"
#include "CameraSourceController.h"

#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

class CameraPluginTitleState final {
public:
    QString title() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _title;
    }

    void setCallback(std::function<void(const QString&)> callback)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _callback = std::move(callback);
    }

    void publish(const QString& title)
    {
        std::function<void(const QString&)> callback;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_active) return;
            _title = title;
            callback = _callback;
        }
        if (callback) callback(title);
    }

    bool readConnectedCameraName(Camera* camera, QString* connectedName) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_active || !camera || !connectedName) return false;
        *connectedName = QString::fromStdString(camera->getConnectedCameraName());
        return true;
    }

    void deactivate()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _active = false;
        _callback = {};
    }

private:
    mutable std::mutex _mutex;
    bool _active = true;
    QString _title = QStringLiteral("Basler Camera Session");
    std::function<void(const QString&)> _callback;
};

class CameraPluginSession final : public IDevicePluginSession {
public:
    CameraPluginSession(
        std::shared_ptr<CameraSystem> system,
        Camera* camera,
        IDevicePluginResourceClaims* claims)
        : _system(std::move(system)), _camera(camera),
          _controller(std::make_unique<CameraSourceController>(_camera, claims, &_claim))
    {
        std::weak_ptr<CameraPluginTitleState> titleState = _titleState;
        Camera* const cameraForCallback = _camera;
        _statusCallback = _camera->registerStatusCallback([titleState, cameraForCallback](Camera::Status status, bool connected) {
            if (status != Camera::ConnectionStatus || !connected || !cameraForCallback) {
                return;
            }
            const auto state = titleState.lock();
            if (!state) return;

            QString connectedName;
            if (state->readConnectedCameraName(cameraForCallback, &connectedName) && !connectedName.isEmpty()) {
                state->publish(connectedName);
            }
        });
    }

    ~CameraPluginSession() override
    {
        _titleState->deactivate();
        if (_camera && _statusCallback != 0) {
            _camera->deregisterStatusCallback(_statusCallback);
        }
        _controller.reset();
        if (_system && _camera) {
            _system->removeCamera(_camera);
        }
        // Release the device key only after the native camera close completed.
        _claim.reset();
    }

    QString title() const override { return _titleState->title(); }

    AbstractSourceController* sourceController() const override { return _controller.get(); }
    unsigned int capabilities() const noexcept override
    {
        return DevicePluginSessionCapability::GraphicsEngine
            | DevicePluginSessionCapability::ScriptEditor;
    }
    void setTitleChangedCallback(std::function<void(const QString&)> callback) override
    {
        _titleState->setCallback(std::move(callback));
    }

private:
    std::shared_ptr<CameraSystem> _system;
    Camera* _camera = nullptr;
    std::shared_ptr<IDevicePluginResourceClaim> _claim;
    std::unique_ptr<CameraSourceController> _controller;
    Camera::CallbackId _statusCallback = 0;
    std::shared_ptr<CameraPluginTitleState> _titleState = std::make_shared<CameraPluginTitleState>();
};

class CameraPlugin final : public QObject, public IDeviceRuntime,
    public IDevicePluginResourceClaimsReceiver {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDeviceRuntime_iid)
    Q_INTERFACES(IDeviceRuntime IDevicePluginResourceClaimsReceiver)

public:
    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("camera"), playgroundDevicePluginDisplayName(), playgroundDevicePluginVersion(), true};
    }

    bool discoverDevices(QString* errorMessage) override
    {
        try {
            const auto system = cameraSystem();
            if (!system) {
                if (errorMessage) *errorMessage = QStringLiteral("Camera plugin is shutting down.");
                return false;
            }
            // Keep startup discovery active while a slower transport publishes its first device.
            (void)system->discoverInitialCatalog(std::chrono::seconds{10}, [] {
                return QThread::currentThread()->isInterruptionRequested();
            });
            if (QThread::currentThread()->isInterruptionRequested()) {
                if (errorMessage) *errorMessage = QStringLiteral("Camera discovery was cancelled.");
                return false;
            }
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        } catch (...) {
            if (errorMessage) *errorMessage = QStringLiteral("Camera discovery failed unexpectedly.");
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& sessionOptions, QString* errorMessage) override
    {
        Q_UNUSED(sessionOptions)
        try {
            const auto system = cameraSystem();
            if (!system) {
                if (errorMessage) *errorMessage = QStringLiteral("Camera plugin is shutting down.");
                return {};
            }
            Camera* camera = system->addCamera();
            if (!camera) {
                if (errorMessage) *errorMessage = QStringLiteral("No Basler camera was detected.");
                return {};
            }
            return std::make_unique<CameraPluginSession>(system, camera,
                _claims.load(std::memory_order_acquire));
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return {};
        }
    }

    void setResourceClaims(IDevicePluginResourceClaims* claims) override
    {
        _claims.store(claims, std::memory_order_release);
    }

    void shutdown() override
    {
        _claims.store(nullptr, std::memory_order_release);
        std::shared_ptr<CameraSystem> system;
        {
            std::lock_guard<std::mutex> lock(_systemMutex);
            _shuttingDown = true;
            system = std::move(_system);
        }
    }

private:
    std::shared_ptr<CameraSystem> cameraSystem()
    {
        std::lock_guard<std::mutex> lock(_systemMutex);
        if (_shuttingDown) return {};
        if (!_system) {
            _system = std::make_shared<CameraSystem>();
        }
        return _system;
    }

    std::mutex _systemMutex;
    std::shared_ptr<CameraSystem> _system;
    std::atomic<IDevicePluginResourceClaims*> _claims{nullptr};
    bool _shuttingDown = false;
};

#include "CameraPlugin.moc"
