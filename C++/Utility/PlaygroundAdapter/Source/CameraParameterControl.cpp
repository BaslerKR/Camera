#include "CameraSourceController.h"
#include <QThread>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
std::optional<DeviceParameterValue> readValue(GenApi::INode* node) {
    if (!GenApi::IsReadable(node)) return {};
    switch (node->GetPrincipalInterfaceType()) {
    case GenApi::intfIInteger: return std::int64_t(GenApi::CIntegerPtr(node)->GetValue());
    case GenApi::intfIFloat: {
        const auto value = GenApi::CFloatPtr(node)->GetValue();
        if (!std::isfinite(value)) throw std::runtime_error("The SDK returned a non-finite parameter value.");
        return value;
    }
    case GenApi::intfIBoolean: return GenApi::CBooleanPtr(node)->GetValue();
    case GenApi::intfIString: {
        const auto value = GenApi::CStringPtr(node)->GetValue();
        return std::string(value.c_str(), value.length());
    }
    case GenApi::intfIEnumeration: return std::string(GenApi::CEnumerationPtr(node)->ToString().c_str());
    default: return {};
    }
}
bool supported(GenApi::INode* node) {
    switch (node->GetPrincipalInterfaceType()) {
    case GenApi::intfIInteger: case GenApi::intfIFloat: case GenApi::intfIBoolean:
    case GenApi::intfIString: case GenApi::intfIEnumeration: case GenApi::intfICommand: return true;
    default: return false;
    }
}
void selectors(GenApi::INode* node, std::vector<DeviceParameterSelector>& output, std::set<std::string>& seen) {
    GenApi::CSelectorPtr selector(node);
    if (!selector) return;
    GenApi::FeatureList_t selecting;
    selector->GetSelectingFeatures(selecting);
    for (const auto feature : selecting) {
        auto* selectingNode = feature->GetNode();
        const std::string name = selectingNode->GetName().c_str();
        if (!seen.insert(name).second) continue;
        auto value = readValue(selectingNode);
        if (!value) throw std::runtime_error("A selecting feature cannot be read.");
        output.push_back({name, *value});
        selectors(selectingNode, output, seen);
    }
}
bool sameObservation(const DeviceParameterDescriptor& a, const DeviceParameterDescriptor& b) {
    return a.target.connectionId == b.target.connectionId && a.target.selectors == b.target.selectors &&
        a.kind == b.kind && a.available == b.available && a.readable == b.readable && a.writable == b.writable &&
        a.value == b.value && a.integerMinimum == b.integerMinimum && a.integerMaximum == b.integerMaximum &&
        a.integerIncrement == b.integerIncrement && a.floatMinimum == b.floatMinimum &&
        a.floatMaximum == b.floatMaximum && a.floatIncrement == b.floatIncrement && a.enumEntries == b.enumEntries &&
        a.unit == b.unit && a.displayName == b.displayName && a.description == b.description &&
        a.observationError == b.observationError;
}
template<class Error>
DeviceParameterResult failure(const Error& error, DeviceParameterEffect effect) {
    return {DeviceParameterError::BackendFailure, error.what(), effect};
}
bool notifyFeatures(Camera* camera) noexcept {
    try { camera->notifyFeaturesChanged(); return true; }
    catch (...) { return false; }
}
}

DeviceParameterDescriptor CameraSourceController::observeParameter(GenApi::INode* node, std::uint64_t generation) {
    if (_observedConnectionGeneration != generation) {
        _parameterObservations.clear();
        _observedConnectionGeneration = generation;
    }
    DeviceParameterDescriptor state;
    state.target = {_parameterInstance + ":" + std::to_string(generation), "remote", node->GetName().c_str(), {}};
    state.displayName = node->GetDisplayName().c_str();
    state.description = node->GetDescription().c_str();
    state.available = GenApi::IsAvailable(node);
    state.readable = GenApi::IsReadable(node);
    state.writable = GenApi::IsWritable(node);
    state.unavailableReason = state.available ? "" : "The SDK reports this feature unavailable.";
    try {
        switch (node->GetPrincipalInterfaceType()) {
        case GenApi::intfIInteger: {
            state.kind = DeviceParameterKind::Integer;
            GenApi::CIntegerPtr number(node);
            if (state.available) {
                state.integerMinimum = number->GetMin(); state.integerMaximum = number->GetMax();
                state.integerIncrement = number->GetInc(); state.unit = number->GetUnit().c_str();
            }
            break;
        }
        case GenApi::intfIFloat: {
            state.kind = DeviceParameterKind::Float;
            GenApi::CFloatPtr number(node);
            if (state.available) {
                state.floatMinimum = number->GetMin(); state.floatMaximum = number->GetMax();
                if (number->HasInc()) state.floatIncrement = number->GetInc();
                state.unit = number->GetUnit().c_str();
            }
            break;
        }
        case GenApi::intfIBoolean: state.kind = DeviceParameterKind::Boolean; break;
        case GenApi::intfIString: state.kind = DeviceParameterKind::String; break;
        case GenApi::intfICommand: state.kind = DeviceParameterKind::Command; break;
        case GenApi::intfIEnumeration: {
            state.kind = DeviceParameterKind::Enumeration;
            if (state.available) {
                GenApi::NodeList_t entries;
                GenApi::CEnumerationPtr(node)->GetEntries(entries);
                for (auto* entry : entries) if (GenApi::IsAvailable(entry))
                    state.enumEntries.emplace_back(GenApi::CEnumEntryPtr(entry)->GetSymbolic().c_str());
            }
            break;
        }
        default: throw std::runtime_error("Unsupported parameter type.");
        }
        std::set<std::string> seen;
        selectors(node, state.target.selectors, seen);
        std::sort(state.target.selectors.begin(), state.target.selectors.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
        state.value = readValue(node);
    } catch (const Pylon::GenericException& error) {
        state.observationError = error.what();
        state.value.reset();
    } catch (const std::exception& error) {
        state.observationError = error.what();
        state.value.reset();
    }
    for (auto* number : {&state.floatMinimum, &state.floatMaximum, &state.floatIncrement}) {
        if (*number && !std::isfinite(**number)) {
            number->reset();
            state.observationError = "The SDK returned a non-finite numeric constraint.";
        }
    }
    const auto previous = _parameterObservations.find(state.target.name);
    if (previous != _parameterObservations.end() && sameObservation(previous->second, state))
        state.revision = previous->second.revision;
    else {
        if (_parameterRevision == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Parameter revision exhausted.");
        state.revision = ++_parameterRevision;
    }
    _parameterObservations[state.target.name] = state;
    return state;
}

DeviceParameterResult CameraSourceController::parameters(std::vector<DeviceParameterDescriptor>* output) {
    if (!output) return {DeviceParameterError::InvalidArgument, "Missing output."};
    output->clear();
    std::vector<DeviceParameterDescriptor> copied;
    bool busy = false;
    try {
        if (!_camera || !_camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t generation) {
            GenApi::NodeList_t nodes;
            map.GetNodes(nodes);
            for (auto* node : nodes) if (supported(node)) copied.push_back(observeParameter(node, generation));
        }, &busy)) return {busy ? DeviceParameterError::Busy : DeviceParameterError::Disconnected,
            busy ? "Camera feature/lifecycle work is busy." : "Camera is disconnected."};
        std::sort(copied.begin(), copied.end(), [](const auto& a, const auto& b) { return a.target.name < b.target.name; });
        *output = std::move(copied);
        return {};
    } catch (const Pylon::GenericException& error) { return failure(error, DeviceParameterEffect::None); }
    catch (const std::exception& error) { return failure(error, DeviceParameterEffect::None); }
    catch (...) { return {DeviceParameterError::BackendFailure, "Unexpected SDK failure."}; }
}

DeviceParameterResult CameraSourceController::readParameter(const DeviceParameterTarget& target) {
    DeviceParameterResult result;
    bool busy = false;
    try {
        if (!_camera || !_camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t generation) {
            if (target.connectionId != _parameterInstance + ":" + std::to_string(generation)) {
                result = {DeviceParameterError::Conflict, "Camera connection changed."}; return;
            }
            auto* node = target.scope == "remote" ? map.GetNode(target.name.c_str()) : nullptr;
            if (!node) { result = {DeviceParameterError::NotFound, "Parameter not found."}; return; }
            if (!supported(node)) { result = {DeviceParameterError::Unsupported, "Unsupported parameter type."}; return; }
            auto state = observeParameter(node, generation);
            if (target.selectors != state.target.selectors) {
                result = {DeviceParameterError::Conflict, "Selector context changed."}; return;
            }
            result.descriptor = state;
            if (!state.observationError.empty()) {
                result.error = DeviceParameterError::BackendFailure;
                result.message = state.observationError;
            }
            else if (!state.available) result.error = DeviceParameterError::Unavailable;
            else if (!state.readable) result.error = DeviceParameterError::Unreadable;
        }, &busy)) return {busy ? DeviceParameterError::Busy : DeviceParameterError::Disconnected,
            busy ? "Camera feature/lifecycle work is busy." : "Camera is disconnected."};
        return result;
    } catch (const Pylon::GenericException& error) { return failure(error, DeviceParameterEffect::None); }
    catch (const std::exception& error) { return failure(error, DeviceParameterEffect::None); }
    catch (...) { return {DeviceParameterError::BackendFailure, "Unexpected SDK failure."}; }
}

DeviceParameterResult CameraSourceController::changeParameter(const DeviceParameterTarget& target,
    const std::optional<DeviceParameterValue>& value, std::optional<std::uint64_t> revision) {
    DeviceParameterResult result;
    bool busy = false;
    try {
        if (!_camera || !_camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t generation) {
            if (target.connectionId != _parameterInstance + ":" + std::to_string(generation)) {
                result = {DeviceParameterError::Conflict, "Camera connection changed."}; return;
            }
            auto* node = target.scope == "remote" ? map.GetNode(target.name.c_str()) : nullptr;
            if (!node) { result = {DeviceParameterError::NotFound, "Parameter not found."}; return; }
            if (!supported(node)) { result = {DeviceParameterError::Unsupported, "Unsupported parameter type."}; return; }
            const auto state = observeParameter(node, generation);
            result.error = validateDeviceParameterWrite(state, target, value, revision);
            if (result.error != DeviceParameterError::None) { result.message = "Parameter validation failed before effects."; return; }
            result.effect = DeviceParameterEffect::Unknown;
            switch (state.kind) {
            case DeviceParameterKind::Integer: GenApi::CIntegerPtr(node)->SetValue(std::get<std::int64_t>(*value)); break;
            case DeviceParameterKind::Float: GenApi::CFloatPtr(node)->SetValue(std::get<double>(*value)); break;
            case DeviceParameterKind::Boolean: GenApi::CBooleanPtr(node)->SetValue(std::get<bool>(*value)); break;
            case DeviceParameterKind::String: {
                const auto& string = std::get<std::string>(*value);
                GenApi::CStringPtr(node)->SetValue(GenICam::gcstring(string.data(), string.size())); break;
            }
            case DeviceParameterKind::Enumeration: {
                const auto& string = std::get<std::string>(*value);
                GenApi::CEnumerationPtr(node)->FromString(GenICam::gcstring(string.data(), string.size())); break;
            }
            case DeviceParameterKind::Command: GenApi::CCommandPtr(node)->Execute(); break;
            }
            result.effect = DeviceParameterEffect::Applied;
            result.descriptor = observeParameter(node, generation);
            if (!result.descriptor->observationError.empty()) {
                result.error = DeviceParameterError::BackendFailure;
                result.message = result.descriptor->observationError;
            }
        }, &busy)) return {busy ? DeviceParameterError::Busy : DeviceParameterError::Disconnected,
            busy ? "Camera feature/lifecycle work is busy." : "Camera is disconnected."};
        if (result.effect != DeviceParameterEffect::None && !notifyFeatures(_camera)) {
            result.error = DeviceParameterError::BackendFailure;
            result.message = "Parameter changed; a feature observer failed.";
        }
        return result;
    } catch (const Pylon::GenericException& error) {
        if (_camera && result.effect != DeviceParameterEffect::None) notifyFeatures(_camera);
        return failure(error, result.effect);
    } catch (const std::exception& error) {
        if (_camera && result.effect != DeviceParameterEffect::None) notifyFeatures(_camera);
        return failure(error, result.effect);
    } catch (...) {
        return {DeviceParameterError::BackendFailure, "Unexpected SDK failure.", result.effect};
    }
}

DeviceParameterResult CameraSourceController::writeParameter(const DeviceParameterTarget& target,
    const DeviceParameterValue& value, std::optional<std::uint64_t> revision) {
    return changeParameter(target, value, revision);
}
DeviceParameterResult CameraSourceController::executeParameter(const DeviceParameterTarget& target,
    std::optional<std::uint64_t> revision) {
    return changeParameter(target, {}, revision);
}

DeviceParameterResult CameraSourceController::scheduleParameterWork(std::function<void()> work) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (_parameterClosing) return {DeviceParameterError::Unavailable, "Camera controller is closing."};
    if (_parameterWorkers.size() >= 8) return {DeviceParameterError::Busy, "Camera parameter worker limit reached."};
    auto* worker = QThread::create([work = std::move(work)] { work(); });
    worker->setParent(this);
    _parameterWorkers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        _parameterWorkers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
    return {};
}

DeviceParameterResult CameraSourceController::requestParameters(InventoryCompletion completion) {
    if (!completion) return {DeviceParameterError::InvalidArgument, "Missing completion callback."};
    return scheduleParameterWork([this, completion = std::move(completion)] {
        DeviceParameterInventory inventory;
        inventory.result = parameters(&inventory.parameters);
        completion(std::move(inventory));
    });
}
DeviceParameterResult CameraSourceController::requestReadParameter(DeviceParameterTarget target, Completion completion) {
    if (!completion) return {DeviceParameterError::InvalidArgument, "Missing completion callback."};
    return scheduleParameterWork([this, target = std::move(target), completion = std::move(completion)] {
        completion(readParameter(target));
    });
}
DeviceParameterResult CameraSourceController::requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
    std::optional<std::uint64_t> revision, Completion completion) {
    if (!completion) return {DeviceParameterError::InvalidArgument, "Missing completion callback."};
    return scheduleParameterWork([this, target = std::move(target), value = std::move(value), revision, completion = std::move(completion)] {
        completion(writeParameter(target, value, revision));
    });
}
DeviceParameterResult CameraSourceController::requestExecuteParameter(DeviceParameterTarget target,
    std::optional<std::uint64_t> revision, Completion completion) {
    if (!completion) return {DeviceParameterError::InvalidArgument, "Missing completion callback."};
    return scheduleParameterWork([this, target = std::move(target), revision, completion = std::move(completion)] {
        completion(executeParameter(target, revision));
    });
}

namespace {
DeviceConnectionError connectionError(DeviceParameterError error) {
    return error == DeviceParameterError::Busy ? DeviceConnectionError::Busy : DeviceConnectionError::Unavailable;
}
}

std::string CameraSourceController::connectionIdFor(const CameraConnectionState& state) const {
    return state.connected ? _parameterInstance + ":" + std::to_string(state.generation) : std::string();
}

DeviceConnectionState CameraSourceController::copiedConnectionState(const CameraConnectionState& state) const {
    return {state.connected, state.connected && _isGrabbing.load(), connectionIdFor(state),
        state.connected ? state.deviceId : std::string(), state.connected ? state.displayName : std::string(), std::nullopt};
}

DeviceConnectionCapabilities CameraSourceController::connectionCapabilities() const {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!_camera || _parameterClosing) {
        const DeviceConnectionAvailability closed{false, "Camera controller lifetime is unavailable."};
        return {closed, closed, closed, closed, closed, closed};
    }
    const DeviceConnectionAvailability yes{true, {}};
    const DeviceConnectionAvailability acquisition{false, "Camera acquisition uses source.start and source.stop."};
    return {yes, yes, yes, yes, acquisition, acquisition};
}

DeviceConnectionResult CameraSourceController::scheduleConnectionWork(std::function<DeviceConnectionResult()> work,
    IDeviceConnectionControl::Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Missing completion callback."};
    if (!_camera) return {DeviceConnectionError::Unavailable, "Camera controller is unavailable."};
    const auto admitted = scheduleParameterWork([work = std::move(work), completion = std::move(completion)] {
        DeviceConnectionResult result;
        try { result = work(); }
        catch (const Pylon::GenericException& error) { result = {DeviceConnectionError::BackendFailure, error.GetDescription(), DeviceConnectionEffect::Unknown}; }
        catch (const std::exception& error) { result = {DeviceConnectionError::BackendFailure, error.what(), DeviceConnectionEffect::Unknown}; }
        catch (...) { result = {DeviceConnectionError::BackendFailure, "The owning device command failed.", DeviceConnectionEffect::Unknown}; }
        // A consumer exception is not a backend failure and must never redeliver completion.
        try { completion(std::move(result)); } catch (...) {}
    });
    if (admitted.error == DeviceParameterError::None) return {};
    return {connectionError(admitted.error), admitted.message};
}

DeviceConnectionResult CameraSourceController::catalogWork(bool refresh, TargetsCompletion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion) return {DeviceConnectionError::InvalidArgument, "Missing completion callback."};
    if (!_camera) return {DeviceConnectionError::Unavailable, "Camera controller is unavailable."};
    const auto admitted = scheduleParameterWork([this, refresh, completion = std::move(completion)] {
        DeviceConnectionTargets copied;
        try {
            QStringList labels;
            for (const auto& item : _camera->deviceCatalog(refresh)) {
                copied.targets.push_back({item.id, item.displayName, {}});
                labels.append(QString::fromStdString(item.displayName));
            }
            if (refresh) emit deviceCatalogChanged(labels);
        } catch (const std::exception& error) {
            copied = {{DeviceConnectionError::BackendFailure, error.what()}, {}};
        } catch (...) {
            copied = {{DeviceConnectionError::BackendFailure, "The camera discovery authority failed."}, {}};
        }
        try { completion(std::move(copied)); } catch (...) {}
    });
    if (admitted.error == DeviceParameterError::None) return {};
    return {connectionError(admitted.error), admitted.message};
}

DeviceConnectionResult CameraSourceController::requestConnectionTargets(TargetsCompletion completion) {
    return catalogWork(false, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestDiscover(TargetsCompletion completion) {
    return catalogWork(true, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestConnectionState(IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this]() -> DeviceConnectionResult {
        return {{}, {}, {}, copiedConnectionState(_camera->connectionState())};
    }, std::move(completion));
}

/** @brief Maps lifecycle outcomes; only an SDK command failure leaves the effect uncertain. */
DeviceConnectionResult CameraSourceController::connectionOutcome(CameraConnectionOutcome outcome, const CameraConnectionState& state) const {
    switch (outcome) {
    case CameraConnectionOutcome::Success: return {{}, {}, DeviceConnectionEffect::Applied, copiedConnectionState(state)};
    case CameraConnectionOutcome::Busy: return {DeviceConnectionError::Busy, "The existing lifecycle/feature authority is busy."};
    case CameraConnectionOutcome::Conflict: return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
    case CameraConnectionOutcome::AlreadyConnected:
        return {DeviceConnectionError::Conflict, "Disconnect the current target explicitly before connecting another."};
    case CameraConnectionOutcome::NotFound: return {DeviceConnectionError::NotFound, "The exact target is absent from the module catalog."};
    case CameraConnectionOutcome::Failed: break;
    }
    return {DeviceConnectionError::BackendFailure, "The SDK connection command failed; accepted effects may have occurred.",
        DeviceConnectionEffect::Unknown, copiedConnectionState(state)};
}

DeviceConnectionResult CameraSourceController::requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, targetId = std::move(targetId)]() -> DeviceConnectionResult {
        auto state = _camera->connectionState();
        if (state.connected)
            return {DeviceConnectionError::Conflict, "Disconnect the current target explicitly before connecting another."};
        const auto outcome = _camera->controlConnection(true, targetId, state.generation, &state);
        return connectionOutcome(outcome, state);
    }, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, connectionId = std::move(connectionId)]() -> DeviceConnectionResult {
        auto state = _camera->connectionState();
        if (!state.connected) return {DeviceConnectionError::Disconnected, "The camera is disconnected."};
        if (connectionIdFor(state) != connectionId) return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
        const auto outcome = _camera->controlConnection(false, {}, state.generation, &state);
        return connectionOutcome(outcome, state);
    }, std::move(completion));
}
