#pragma once
#include "AbstractSourceController.h"
#include "Camera.h"
#include "DeviceDiagnostics.h"
#include "DeviceConnectionControl.h"
#include "DeviceParameterControl.h"
#include <map>
#include <QSet>
#include <QThread>
#include <atomic>
#include <memory>

class PylonGraphicsFrameStream;

class CameraSourceController : public AbstractSourceController, public IDeviceParameterControl, public IDeviceConnectionControl {
    Q_OBJECT
    Q_INTERFACES(IDeviceParameterControl IDeviceConnectionControl)
public:
    using Completion = IDeviceParameterControl::Completion;
    using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
    explicit CameraSourceController(Camera* camera, QObject* parent = nullptr);
    ~CameraSourceController() override;

    void start() override;
    void stop() override;
    bool isGrabbing() const override;
    void setFrameConsumer(FrameConsumer consumer) override;
    bool supports3D() const override;
    /** @brief Returns producer counters for the device statusbar. */
    [[nodiscard]] SourceRuntimeDiagnostics runtimeDiagnostics(unsigned sourceIndex) const;
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
    /** @brief Declares Single/Live unavailable: acquisition uses the common source controls. */
    DeviceConnectionCapabilities connectionCapabilities() const override;
    /** @brief Copies the cached system catalog on a bounded worker. */
    DeviceConnectionResult requestConnectionTargets(TargetsCompletion completion) override;
    /** @brief Refreshes the catalog through the existing system discovery authority. */
    DeviceConnectionResult requestDiscover(TargetsCompletion completion) override;
    /** @brief Copies the logical connection; the id is this controller plus its reconnect generation. */
    DeviceConnectionResult requestConnectionState(IDeviceConnectionControl::Completion completion) override;
    /** @brief Connects an exact cached target while disconnected. */
    DeviceConnectionResult requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) override;
    /** @brief Uses the existing stopped/drained teardown for the exact current connection. */
    DeviceConnectionResult requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
signals:
    /** @brief Publishes copied discovered labels for queued GUI selector refresh. */
    void deviceCatalogChanged(QStringList names);

private:
    /** @brief Owns accepted worker lifetime independently of the caller's wait. */
    DeviceParameterResult scheduleParameterWork(std::function<void()> work);
    /** @brief Runs lifecycle work on the same bounded workers and delivers it exactly once. */
    DeviceConnectionResult scheduleConnectionWork(std::function<DeviceConnectionResult()> work,
        IDeviceConnectionControl::Completion completion);
    DeviceConnectionResult catalogWork(bool refresh, TargetsCompletion completion);
    DeviceConnectionResult connectionOutcome(CameraConnectionOutcome outcome, const CameraConnectionState& state) const;
    /** @brief Matches the parameter target connection id, so one id addresses both capabilities. */
    std::string connectionIdFor(const CameraConnectionState& state) const;
    DeviceConnectionState copiedConnectionState(const CameraConnectionState& state) const;
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
