#pragma once
#include "AbstractSourceController.h"
#include "Camera.h"
#include "DeviceParameterControl.h"
#include "ToolProvider.h"
#include <map>
#include <QSet>
#include <QThread>
#include <atomic>
#include <memory>

class PylonGraphicsFrameStream;

class CameraSourceController : public AbstractSourceController, public IDeviceParameterControl, public IToolProvider {
    Q_OBJECT
    Q_INTERFACES(IDeviceParameterControl IToolProvider)
public:
    using Completion = IDeviceParameterControl::Completion;
    explicit CameraSourceController(Camera* camera, QObject* parent = nullptr);
    ~CameraSourceController() override;

    void start() override;
    void stop() override;
    bool isGrabbing() const override;
    void setFrameConsumer(FrameConsumer consumer) override;
    bool supports3D() const override;
    /** @brief Copies current remote-device parameters without widget state. */
    DeviceParameterResult parameters(std::vector<DeviceParameterDescriptor>* output);
    /** @brief Reads an exact connection-scoped parameter. */
    DeviceParameterResult readParameter(const DeviceParameterTarget& target);
    /** @brief Validates and writes a typed parameter without changing acquisition. */
    DeviceParameterResult writeParameter(const DeviceParameterTarget& target,
        const DeviceParameterValue& value, std::optional<std::uint64_t> expectedRevision);
    /** @brief Executes a supported command without a synthetic acquisition result. */
    DeviceParameterResult executeParameter(const DeviceParameterTarget& target,
        std::optional<std::uint64_t> expectedRevision);
    /** @brief Admits bounded worker inventory work and drains it before destruction. */
    DeviceParameterResult requestParameters(InventoryCompletion completion) override;
    /** @brief Admits bounded worker observation without blocking the GUI. */
    DeviceParameterResult requestReadParameter(DeviceParameterTarget target, Completion completion) override;
    /** @brief Admits bounded worker mutation with copied targets and values. */
    DeviceParameterResult requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
        std::optional<std::uint64_t> expectedRevision, Completion completion) override;
    /** @brief Admits bounded worker command execution. */
    DeviceParameterResult requestExecuteParameter(DeviceParameterTarget target,
        std::optional<std::uint64_t> expectedRevision, Completion completion) override;
    /** @brief Declares neutral discovery and connection tools without SDK work during metadata queries. */
    QList<ToolDescriptor> toolDescriptors() const override;
    /** @brief Routes standard calls through existing system/lifecycle authority and bounded workers. */
    ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments, IToolProvider::Completion completion) override;
signals:
    /** @brief Publishes copied discovered labels for queued GUI selector refresh. */
    void deviceCatalogChanged(QStringList names);

private:
    /** @brief Owns accepted worker lifetime independently of the caller's wait. */
    DeviceParameterResult scheduleParameterWork(std::function<void()> work);
    QSet<QThread*> _parameterWorkers;
    bool _parameterClosing = false;
    void registerCallbacks();
    void deregisterCallbacks();
    /** @brief Assigns observed revisions while holding the owning camera/node-map locks. */
    DeviceParameterDescriptor observeParameter(GenApi::INode* node, std::uint64_t generation);
    /** @brief Performs validation, SDK mutation and readback under the owning locks. */
    DeviceParameterResult changeParameter(const DeviceParameterTarget& target,
        const std::optional<DeviceParameterValue>& value, std::optional<std::uint64_t> revision);
    std::string _parameterInstance;
    std::uint64_t _parameterRevision = 0;
    std::uint64_t _observedConnectionGeneration = 0;
    std::map<std::string, DeviceParameterDescriptor> _parameterObservations;

    Camera* _camera;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<uint64_t> _frameSeq{0};

    Camera::CallbackId _statusCallbackId = 0;
    std::unique_ptr<PylonGraphicsFrameStream> _graphicsStream;
};
