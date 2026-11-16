#pragma once
#include "DeviceRuntime/Contract/AbstractSourceController.h"
#include "Camera.h"
#include "DeviceRuntime/Contract/DeviceConnectionControl.h"
#include "DeviceRuntime/Contract/DeviceRuntime.h"
#include "DeviceRuntime/Contract/DeviceParameterControl.h"
#include "DeviceRuntime/Contract/FrameStorage.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <QString>
#include <QSet>
#include <QThread>
#include <QLoggingCategory>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

class PylonGraphicsFrameStream;

Q_DECLARE_LOGGING_CATEGORY(cameraSourceDiagnosticLog)

class CameraSourceController : public AbstractSourceController, public IDeviceParameterControl, public IDeviceConnectionControl,
                               public IFrameStorageClient {
    Q_OBJECT
    Q_INTERFACES(IDeviceParameterControl IDeviceConnectionControl IFrameStorageClient)
public:
    void setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider) override;
    using Completion = IDeviceParameterControl::Completion;
    using ChangeListener = IDeviceParameterControl::ChangeListener;
    using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
    CameraSourceController(Camera* camera, IDevicePluginResourceClaims* claims = nullptr,
        std::shared_ptr<IDevicePluginResourceClaim>* claim = nullptr,
        QObject* parent = nullptr);
    ~CameraSourceController() override;

    /** @brief Copies producer counters used by the device statusbar. */
    std::optional<SourceRuntimeDiagnostics> sourceRuntimeDiagnostics(unsigned sourceIndex) const override;
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
    /** @brief Admits one snapshot-plus-delta subscription for the exact connection. */
    DeviceParameterResult requestParameterChanges(std::string connectionId,
        std::string subscriptionId, ChangeListener delivery) override;
    /** @brief Stops one subscription after its in-flight delivery drains. */
    DeviceParameterResult releaseParameterChanges(std::string connectionId,
        std::string subscriptionId, Completion completion) override;
    /** @brief Declares Single from the observed AcquisitionMode and Live as continuous. */
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
    /** @brief Writes SingleFrame and completes after its one frame is delivered. */
    DeviceConnectionResult requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** @brief Writes Continuous and starts continuous acquisition. */
    DeviceConnectionResult requestLiveStart(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
    /** @brief Stops the exact connection's continuous acquisition without changing AcquisitionMode. */
    DeviceConnectionResult requestLiveStop(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
signals:
    /** @brief Publishes copied discovered labels for queued GUI selector refresh. */
    void deviceCatalogChanged(QStringList names);

private:
    struct ParameterSubscription;
    /** @brief Owns accepted worker lifetime independently of the caller's wait. */
    DeviceParameterResult scheduleParameterWork(std::function<void()> work);
    /** @brief Starts the module-owned observer and delivery threads. */
    void startChangeWorker();
    /** @brief Stops both change threads after the last delivery finishes. */
    void stopChangeWorker();
    /** @brief Drains enqueued node names and snapshot requests on the observer thread. */
    void changeWorkerLoop();
    /** @brief Drains queued snapshot/delta messages on the delivery thread. */
    void deliveryWorkerLoop();
    /** @brief Wakes the delivery thread after the observer queued messages. */
    void notifyParameterDelivery();
    /** @brief Re-observes changed nodes and queues snapshot/delta messages. */
    void processParameterChanges(const std::deque<std::string>& names,
        const std::vector<std::string>& snapshotIds);
    /** @brief Ends every subscription with one terminal message. */
    void endParameterSubscriptions(DeviceParameterError error, const std::string& message);
    /** @brief Wakes the change thread for a node name emitted by an SDK callback. */
    void enqueueParameterChange(const std::string& nodeName);
    /** @brief Wakes the change thread with the disconnected terminal state. */
    void enqueueParameterConnectionLost(const std::string& message);
    /** @brief Builds one full inventory copy under the camera feature locks. */
    bool observeParameterInventory(std::vector<DeviceParameterDescriptor>* output);
    /** @brief Delivers every queued message; the caller is the delivery thread. */
    void deliverParameterChanges();
    /** @brief Runs lifecycle work on the same bounded workers and delivers it exactly once. */
    DeviceConnectionResult scheduleConnectionWork(std::function<DeviceConnectionResult()> work,
        IDeviceConnectionControl::Completion completion);
    DeviceConnectionResult catalogWork(bool refresh, TargetsCompletion completion);
    DeviceConnectionResult connectionOutcome(CameraConnectionOutcome outcome, const CameraConnectionState& state) const;
    /** @brief Records an applied mode change in the log and structured diagnostics. */
    void reportAcquisitionStart(const CameraAcquisitionStart& start, const char* action);
    /** @brief Matches the parameter target connection id, so one id addresses both capabilities. */
    std::string connectionIdFor(const CameraConnectionState& state) const;
    DeviceConnectionState copiedConnectionState(const CameraConnectionState& state) const;
    QSet<QThread*> _parameterWorkers;
    std::atomic<bool> _parameterClosing{false};
    void registerCallbacks();
    /** @brief Releases the held device claim, if any. */
    void releaseClaim();
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
    /** @brief Serializes change observation with API feature work without blocking on lifecycle work. */
    std::mutex _featureObservationMutex;

    std::thread _changeThread;
    std::mutex _changeMutex;
    std::condition_variable _changeCondition;
    bool _changeStopping = false;
    bool _connectionLost = false;
    std::string _connectionLostMessage;
    std::deque<std::string> _pendingChangeNames;
    std::vector<std::string> _pendingSnapshotIds;
    std::thread _deliveryThread;
    std::mutex _deliveryStateMutex;
    std::condition_variable _deliveryCondition;
    bool _deliveryStopping = false;
    bool _deliveryPending = false;
    std::mutex _parameterSubscriptionMutex;
    std::map<std::string, std::shared_ptr<ParameterSubscription>> _parameterSubscriptions;
    Camera::CallbackId _nodeCallbackId = 0;

    Camera* _camera;
    IDevicePluginResourceClaims* _claims = nullptr;
    std::shared_ptr<IDevicePluginResourceClaim>* _claim = nullptr;
    QString _claimKey;
    mutable std::mutex _connectionWorkMutex;
    FrameConsumer _frameConsumer;
    std::atomic<bool> _isGrabbing{false};
    std::atomic<uint64_t> _frameSeq{0};

    Camera::CallbackId _statusCallbackId = 0;
    std::unique_ptr<PylonGraphicsFrameStream> _graphicsStream;
};
