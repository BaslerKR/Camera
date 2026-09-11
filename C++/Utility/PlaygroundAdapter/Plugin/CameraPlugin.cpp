#include "DevicePluginTemplate.h"

#include "Camera.h"
#include "CameraSystem.h"
#include "CameraSourceController.h"
#include "Utility/Qt/QCameraWidget.h"

#include <QPointer>

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
        QStringList discoveredCameraNames)
        : _system(std::move(system)), _camera(camera),
          _discoveredCameraNames(std::move(discoveredCameraNames)),
          _controller(std::make_unique<CameraSourceController>(_camera))
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
        if (_widget) {
            _widget->prepareForShutdown();
        }
        _controller.reset();
        if (_system && _camera) {
            _system->removeCamera(_camera);
        }
    }

    QString title() const override { return _titleState->title(); }

    std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
    {
        if (!_widget) {
            _widget = new QCameraWidget(parent, _camera);
            _widget->setDiscoveredCameraNames(_discoveredCameraNames);
        }
        return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
                 Qt::LeftDockWidgetArea, _widget, true}};
    }

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
    QStringList _discoveredCameraNames;
    std::unique_ptr<CameraSourceController> _controller;
    QPointer<QCameraWidget> _widget;
    Camera::CallbackId _statusCallback = 0;
    std::shared_ptr<CameraPluginTitleState> _titleState = std::make_shared<CameraPluginTitleState>();
};

class CameraPlugin final : public DevicePluginTemplate {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
    Q_INTERFACES(IDevicePlugin)

public:
    DevicePluginDescriptor descriptor() const override
    {
        return {QStringLiteral("camera"), QStringLiteral("Basler Camera"), playgroundDevicePluginVersion(), true};
    }

    bool discoverDevices(QVariantMap* discoveryData, QString* errorMessage) override
    {
        try {
            const auto system = cameraSystem();
            if (!system) {
                if (errorMessage) *errorMessage = QStringLiteral("Camera plugin is shutting down.");
                return false;
            }
            const auto cameraNames = system->getCameraList();
            // An empty list is a valid startup state. The camera control owns
            // the subsequent hardware rescan through its Refresh action.
            QStringList names;
            names.reserve(static_cast<qsizetype>(cameraNames.size()));
            for (const std::string& cameraName : cameraNames) {
                names.append(QString::fromStdString(cameraName));
            }
            if (discoveryData) discoveryData->insert(QStringLiteral("cameraNames"), names);
            return true;
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return false;
        }
    }

    std::unique_ptr<IDevicePluginSession> createSession(
        const QVariantMap& discoveryData, QString* errorMessage) override
    {
        try {
            const QStringList cameraNames = discoveryData.value(QStringLiteral("cameraNames")).toStringList();
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
            return std::make_unique<CameraPluginSession>(system, camera, cameraNames);
        } catch (const std::exception& error) {
            if (errorMessage) *errorMessage = QString::fromLocal8Bit(error.what());
            return {};
        }
    }

    void shutdown() override
    {
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
    bool _shuttingDown = false;
};

#include "CameraPlugin.moc"
