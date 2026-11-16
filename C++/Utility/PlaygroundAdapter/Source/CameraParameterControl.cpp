#include "CameraSourceController.h"
#include <QThread>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
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
        a.observationError == b.observationError && a.categoryPath == b.categoryPath &&
        a.visibility == b.visibility && a.representation == b.representation &&
        a.selectorRelations == b.selectorRelations && a.producerShared == b.producerShared;
}
/** @brief Copies a GenApi display name within the descriptor's byte bound. */
std::string boundedText(const GenICam::gcstring& value, std::size_t limit) {
    std::string text(value.c_str());
    if (text.size() > limit) text.resize(limit);
    return text;
}
/** @brief Walks the category ancestry once and reports the tree and shared-scope marker. */
struct NodeTree {
    std::vector<std::string> categoryPath;
    bool producerShared = false;
};
NodeTree nodeTree(GenApi::INode* node) {
    NodeTree tree;
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
        if (name == "TransportLayerControl") tree.producerShared = true;
        auto segment = boundedText(category->GetDisplayName(),
            deviceParameterCategorySegmentLimit);
        if (segment.empty()) segment = boundedText(category->GetName(),
            deviceParameterCategorySegmentLimit);
        if (!segment.empty()) tree.categoryPath.push_back(std::move(segment));
        current = category;
    }
    std::reverse(tree.categoryPath.begin(), tree.categoryPath.end());
    if (tree.categoryPath.size() > deviceParameterCategoryDepthLimit)
        tree.categoryPath.resize(deviceParameterCategoryDepthLimit);
    return tree;
}
DeviceParameterVisibility visibilityOf(GenApi::INode* node) {
    switch (node->GetVisibility()) {
    case GenApi::Expert: return DeviceParameterVisibility::Advanced;
    case GenApi::Guru: return DeviceParameterVisibility::Expert;
    case GenApi::Invisible: return DeviceParameterVisibility::Hidden;
    case GenApi::Beginner: return DeviceParameterVisibility::Primary;
    default: return DeviceParameterVisibility::Primary;
    }
}
/** @brief Derives the editor hint from the GenApi value representation. */
DeviceParameterRepresentation representationOf(GenApi::INode* node, DeviceParameterKind kind) {
    if (kind == DeviceParameterKind::Enumeration) return DeviceParameterRepresentation::Choice;
    if (kind == DeviceParameterKind::Boolean) return DeviceParameterRepresentation::Toggle;
    if (kind == DeviceParameterKind::Command) return DeviceParameterRepresentation::Automatic;
    if (kind == DeviceParameterKind::String) return DeviceParameterRepresentation::Text;
    GenApi::ERepresentation representation = GenApi::Linear;
    if (kind == DeviceParameterKind::Integer)
        representation = GenApi::CIntegerPtr(node)->GetRepresentation();
    else if (kind == DeviceParameterKind::Float)
        representation = GenApi::CFloatPtr(node)->GetRepresentation();
    else
        return DeviceParameterRepresentation::Automatic;
    switch (representation) {
    case GenApi::Logarithmic: return DeviceParameterRepresentation::Slider;
    case GenApi::Boolean: return DeviceParameterRepresentation::Toggle;
    case GenApi::HexNumber: return DeviceParameterRepresentation::Text;
    case GenApi::IPV4Address: return DeviceParameterRepresentation::Text;
    case GenApi::MACAddress: return DeviceParameterRepresentation::Text;
    case GenApi::Linear: return DeviceParameterRepresentation::Number;
    case GenApi::PureNumber: return DeviceParameterRepresentation::Number;
    default: return DeviceParameterRepresentation::Automatic;
    }
}
/** @brief Collects both selector directions observed from the GenApi node map. */
std::vector<DeviceParameterSelectorRelation> selectorRelations(GenApi::INode* node) {
    std::map<std::string, DeviceParameterSelectorRole> relations;
    const auto add = [&relations](GenApi::INode* feature, DeviceParameterSelectorRole role) {
        if (!feature) return;
        const auto name = boundedText(feature->GetName(),
            deviceParameterSelectorRelationNameLimit);
        if (name.empty() || relations.count(name)) return;
        relations.emplace(name, role);
    };
    GenApi::CSelectorPtr selector(node);
    if (selector) {
        GenApi::FeatureList_t selected;
        selector->GetSelectedFeatures(selected);
        for (const auto feature : selected) add(feature->GetNode(),
            DeviceParameterSelectorRole::Selects);
        GenApi::FeatureList_t selecting;
        selector->GetSelectingFeatures(selecting);
        for (const auto feature : selecting) add(feature->GetNode(),
            DeviceParameterSelectorRole::SelectedBy);
    }
    std::vector<DeviceParameterSelectorRelation> output;
    for (auto& entry : relations) {
        if (output.size() >= deviceParameterSelectorRelationLimit) break;
        output.push_back({entry.first, entry.second});
    }
    return output;
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
        const auto tree = nodeTree(node);
        state.categoryPath = tree.categoryPath;
        state.producerShared = tree.producerShared;
        state.visibility = visibilityOf(node);
        state.representation = representationOf(node, state.kind);
        state.selectorRelations = selectorRelations(node);
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

struct CameraSourceController::ParameterSubscription {
    std::string connectionId;
    std::string subscriptionId;
    IDeviceParameterControl::ChangeListener delivery;
    DeviceParameterChangeQueue queue;
    std::mutex deliveryMutex;
    bool active = true;
    bool awaitingSnapshot = true;
    std::map<std::string, std::uint64_t> deliveredRevisions;
    std::uint64_t nextSequence = 0;
};

void CameraSourceController::startChangeWorker() {
    if (_changeThread.joinable()) return;
    _changeThread = std::thread([this] { changeWorkerLoop(); });
}

void CameraSourceController::stopChangeWorker() {
    {
        std::lock_guard<std::mutex> lock(_changeMutex);
        _changeStopping = true;
        _pendingChangeNames.clear();
        _pendingSnapshotIds.clear();
    }
    _changeCondition.notify_all();
    if (_changeThread.joinable()) _changeThread.join();
    std::vector<std::shared_ptr<ParameterSubscription>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (auto& entry : _parameterSubscriptions) {
            entry.second->active = false;
            subscriptions.push_back(entry.second);
        }
        _parameterSubscriptions.clear();
    }
}

void CameraSourceController::enqueueParameterChange(const std::string& nodeName) {
    if (nodeName.empty() || _parameterClosing) return;
    {
        // Without a subscriber there is no one to notify; the next snapshot covers the state.
        std::lock_guard<std::mutex> subscriptions(_parameterSubscriptionMutex);
        if (_parameterSubscriptions.empty()) return;
    }
    {
        std::lock_guard<std::mutex> lock(_changeMutex);
        if (_changeStopping) return;
        if (nodeName == "Root") {
            _pendingChangeNames.clear();
            _pendingChangeNames.insert(nodeName);
        } else if (_pendingChangeNames.count("Root") == 0) {
            _pendingChangeNames.insert(nodeName);
        }
    }
    _changeCondition.notify_one();
}

void CameraSourceController::enqueueParameterConnectionLost(const std::string& message) {
    if (_parameterClosing) return;
    {
        std::lock_guard<std::mutex> lock(_changeMutex);
        if (_changeStopping) return;
        _connectionLost = true;
        _connectionLostMessage = message;
        _pendingChangeNames.clear();
        _pendingSnapshotIds.clear();
    }
    _changeCondition.notify_one();
}

bool CameraSourceController::observeParameterInventory(std::vector<DeviceParameterDescriptor>* output) {
    if (!output || !_camera) return false;
    std::vector<DeviceParameterDescriptor> copied;
    try {
        if (!_camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t generation) {
            GenApi::NodeList_t nodes;
            map.GetNodes(nodes);
            for (auto* node : nodes) if (supported(node)) copied.push_back(observeParameter(node, generation));
        })) return false;
    } catch (const Pylon::GenericException&) {
        return false;
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
    std::sort(copied.begin(), copied.end(),
        [](const auto& a, const auto& b) { return a.target.name < b.target.name; });
    *output = std::move(copied);
    return true;
}

void CameraSourceController::changeWorkerLoop() {
    for (;;) {
        std::set<std::string> names;
        std::vector<std::string> snapshotIds;
        bool lost = false;
        std::string lostMessage;
        {
            std::unique_lock<std::mutex> lock(_changeMutex);
            _changeCondition.wait(lock, [this] {
                return _changeStopping || _connectionLost || !_pendingChangeNames.empty()
                    || !_pendingSnapshotIds.empty();
            });
            if (_changeStopping) return;
            names.swap(_pendingChangeNames);
            snapshotIds.swap(_pendingSnapshotIds);
            lost = _connectionLost;
            _connectionLost = false;
            lostMessage = std::move(_connectionLostMessage);
            _connectionLostMessage.clear();
        }
        if (lost) {
            endParameterSubscriptions(DeviceParameterError::Disconnected,
                lostMessage.empty() ? "The camera is disconnected." : lostMessage);
            continue;
        }
        try {
            processParameterChanges(names, snapshotIds);
        } catch (const std::exception& error) {
            endParameterSubscriptions(DeviceParameterError::BackendFailure, error.what());
        } catch (...) {
            endParameterSubscriptions(DeviceParameterError::BackendFailure,
                "The camera parameter change observer failed.");
        }
    }
}

void CameraSourceController::processParameterChanges(const std::set<std::string>& names,
    const std::vector<std::string>& snapshotIds) {
    const bool fullObservation = names.count("Root") > 0;
    std::vector<DeviceParameterDescriptor> inventory;
    std::map<std::string, DeviceParameterDescriptor> observed;
    std::set<std::string> missing;
    if (fullObservation) {
        if (!observeParameterInventory(&inventory)) {
            enqueueParameterConnectionLost("The camera disconnected while observing parameter changes.");
            return;
        }
        for (const auto& descriptor : inventory) observed.emplace(descriptor.target.name, descriptor);
    } else if (!names.empty()) {
        if (!_camera) {
            enqueueParameterConnectionLost("The camera is disconnected.");
            return;
        }
        for (const auto& name : names) {
            bool found = false;
            try {
                if (!_camera->accessFeatures([&](GenApi::INodeMap& map, std::uint64_t generation) {
                    auto* node = map.GetNode(name.c_str());
                    if (!node || !supported(node)) return;
                    found = true;
                    observed[name] = observeParameter(node, generation);
                })) {
                    enqueueParameterConnectionLost(
                        "The camera disconnected while observing parameter changes.");
                    return;
                }
            } catch (const Pylon::GenericException&) {
                // A transient SDK failure leaves the node unobserved; consumers may re-read it.
                found = false;
            } catch (const std::exception&) {
                found = false;
            } catch (...) {
                found = false;
            }
            if (!found) missing.insert(name);
        }
    }
    std::vector<std::shared_ptr<ParameterSubscription>> snapshots;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (const auto& id : snapshotIds) {
            const auto found = _parameterSubscriptions.find(id);
            if (found != _parameterSubscriptions.end() && found->second->active
                && found->second->awaitingSnapshot)
                snapshots.push_back(found->second);
        }
    }
    if (!snapshots.empty()) {
        if (inventory.empty()) {
            if (!observeParameterInventory(&inventory)) {
                endParameterSubscriptions(DeviceParameterError::Disconnected,
                    "The camera disconnected while building a parameter snapshot.");
                return;
            }
        }
        std::uint64_t revision = 0;
        for (const auto& descriptor : inventory) revision = std::max(revision, descriptor.revision);
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (const auto& subscription : snapshots) {
            if (!subscription->active || !subscription->awaitingSnapshot) continue;
            DeviceParameterChange snapshot;
            snapshot.kind = DeviceParameterChangeKind::Snapshot;
            snapshot.connectionId = subscription->connectionId;
            snapshot.subscriptionId = subscription->subscriptionId;
            snapshot.sequence = ++subscription->nextSequence;
            snapshot.revision = revision;
            snapshot.descriptors = inventory;
            subscription->deliveredRevisions.clear();
            for (const auto& descriptor : inventory)
                subscription->deliveredRevisions[descriptor.target.name] = descriptor.revision;
            subscription->queue.push(std::move(snapshot));
            subscription->awaitingSnapshot = false;
        }
    }
    std::vector<std::string> resync;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (auto& entry : _parameterSubscriptions) {
            const auto& subscription = entry.second;
            if (!subscription->active || subscription->awaitingSnapshot) continue;
            bool overflowed = false;
            const auto queueDelta = [&](DeviceParameterChange delta) {
                if (overflowed) return;
                delta.kind = DeviceParameterChangeKind::Delta;
                delta.connectionId = subscription->connectionId;
                delta.subscriptionId = subscription->subscriptionId;
                delta.sequence = ++subscription->nextSequence;
                if (!subscription->queue.push(std::move(delta))) overflowed = true;
            };
            for (const auto& item : observed) {
                const auto previous = subscription->deliveredRevisions.find(item.first);
                if (previous != subscription->deliveredRevisions.end()
                    && previous->second == item.second.revision) continue;
                subscription->deliveredRevisions[item.first] = item.second.revision;
                DeviceParameterChange delta;
                delta.revision = item.second.revision;
                delta.descriptors.push_back(item.second);
                queueDelta(std::move(delta));
            }
            DeviceParameterChange invalidated;
            for (const auto& name : missing) {
                if (subscription->deliveredRevisions.erase(name) == 0) continue;
                DeviceParameterTarget target;
                target.connectionId = subscription->connectionId;
                target.scope = "remote";
                target.name = name;
                invalidated.invalidated.push_back(std::move(target));
            }
            if (fullObservation) {
                for (auto it = subscription->deliveredRevisions.begin();
                     it != subscription->deliveredRevisions.end();) {
                    if (observed.count(it->first)) {
                        ++it;
                        continue;
                    }
                    DeviceParameterTarget target;
                    target.connectionId = subscription->connectionId;
                    target.scope = "remote";
                    target.name = it->first;
                    invalidated.invalidated.push_back(std::move(target));
                    it = subscription->deliveredRevisions.erase(it);
                }
            }
            if (!invalidated.invalidated.empty()) queueDelta(std::move(invalidated));
            if (overflowed) resync.push_back(entry.first);
        }
    }
    if (!resync.empty()) {
        if (inventory.empty() && !observeParameterInventory(&inventory)) {
            endParameterSubscriptions(DeviceParameterError::Disconnected,
                "The camera disconnected while resynchronizing parameter changes.");
            return;
        }
        std::uint64_t revision = 0;
        for (const auto& descriptor : inventory) revision = std::max(revision, descriptor.revision);
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (const auto& id : resync) {
            const auto found = _parameterSubscriptions.find(id);
            if (found == _parameterSubscriptions.end() || !found->second->active) continue;
            const auto& subscription = found->second;
            DeviceParameterChange snapshot;
            snapshot.kind = DeviceParameterChangeKind::Snapshot;
            snapshot.connectionId = subscription->connectionId;
            snapshot.subscriptionId = subscription->subscriptionId;
            snapshot.sequence = ++subscription->nextSequence;
            snapshot.revision = revision;
            snapshot.descriptors = inventory;
            subscription->deliveredRevisions.clear();
            for (const auto& descriptor : inventory)
                subscription->deliveredRevisions[descriptor.target.name] = descriptor.revision;
            subscription->queue.replaceWithSnapshot(std::move(snapshot),
                "The camera parameter change queue overflowed; observe the full inventory again.");
        }
    }
    deliverParameterChanges();
}

void CameraSourceController::deliverParameterChanges() {
    std::vector<std::shared_ptr<ParameterSubscription>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (auto& entry : _parameterSubscriptions)
            if (entry.second->active) subscriptions.push_back(entry.second);
    }
    for (const auto& subscription : subscriptions) {
        for (;;) {
            std::optional<DeviceParameterChange> change;
            {
                std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
                if (!subscription->active) break;
                change = subscription->queue.pop();
            }
            if (!change) break;
            std::lock_guard<std::mutex> delivery(subscription->deliveryMutex);
            {
                std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
                if (!subscription->active) break;
            }
            try {
                if (subscription->delivery) subscription->delivery(std::move(*change));
            } catch (...) {
                // A consumer exception must not stop the module's other subscriptions.
            }
        }
    }
}

void CameraSourceController::endParameterSubscriptions(DeviceParameterError error,
    const std::string& message) {
    std::vector<std::shared_ptr<ParameterSubscription>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        for (auto& entry : _parameterSubscriptions) {
            entry.second->active = false;
            subscriptions.push_back(entry.second);
        }
        _parameterSubscriptions.clear();
    }
    for (const auto& subscription : subscriptions) {
        DeviceParameterChange terminal;
        terminal.kind = DeviceParameterChangeKind::Delta;
        terminal.connectionId = subscription->connectionId;
        terminal.subscriptionId = subscription->subscriptionId;
        terminal.sequence = ++subscription->nextSequence;
        terminal.revision = 0;
        terminal.error = error;
        terminal.message = message;
        std::lock_guard<std::mutex> delivery(subscription->deliveryMutex);
        try {
            if (subscription->delivery) subscription->delivery(std::move(terminal));
        } catch (...) {
        }
    }
}

DeviceParameterResult CameraSourceController::requestParameterChanges(std::string connectionId,
    std::string subscriptionId, ChangeListener delivery) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!delivery || validateDeviceParameterSubscription(connectionId, subscriptionId)
        != DeviceParameterError::None)
        return {DeviceParameterError::InvalidArgument,
            "The parameter change subscription is invalid.", DeviceParameterEffect::None, std::nullopt};
    if (_parameterClosing || !_camera)
        return {DeviceParameterError::Unavailable, "Camera controller is closing.",
            DeviceParameterEffect::None, std::nullopt};
    const auto state = _camera->connectionState();
    if (!state.connected)
        return {DeviceParameterError::Disconnected, "Camera is disconnected.",
            DeviceParameterEffect::None, std::nullopt};
    if (connectionId != connectionIdFor(state))
        return {DeviceParameterError::Conflict, "Camera connection changed.",
            DeviceParameterEffect::None, std::nullopt};
    auto subscription = std::make_shared<ParameterSubscription>();
    subscription->connectionId = std::move(connectionId);
    subscription->subscriptionId = std::move(subscriptionId);
    subscription->delivery = std::move(delivery);
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        if (_parameterSubscriptions.count(subscription->subscriptionId)) {
            return {DeviceParameterError::Busy,
                "The parameter change subscription is already active.",
                DeviceParameterEffect::None, std::nullopt};
        }
        if (_parameterSubscriptions.size() >= deviceParameterSubscriptionLimit) {
            return {DeviceParameterError::Busy,
                "The parameter change subscription bound is reached.",
                DeviceParameterEffect::None, std::nullopt};
        }
        _parameterSubscriptions.emplace(subscription->subscriptionId, subscription);
    }
    {
        std::lock_guard<std::mutex> lock(_changeMutex);
        if (_changeStopping) {
            std::lock_guard<std::mutex> subscriptionLock(_parameterSubscriptionMutex);
            _parameterSubscriptions.erase(subscription->subscriptionId);
            return {DeviceParameterError::Unavailable, "Camera controller is closing.",
                DeviceParameterEffect::None, std::nullopt};
        }
        _pendingSnapshotIds.push_back(subscription->subscriptionId);
    }
    _changeCondition.notify_one();
    return {};
}

DeviceParameterResult CameraSourceController::releaseParameterChanges(std::string connectionId,
    std::string subscriptionId, Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!completion || validateDeviceParameterSubscription(connectionId, subscriptionId)
        != DeviceParameterError::None)
        return {DeviceParameterError::InvalidArgument,
            "The parameter change subscription is invalid.", DeviceParameterEffect::None, std::nullopt};
    std::shared_ptr<ParameterSubscription> subscription;
    {
        std::lock_guard<std::mutex> lock(_parameterSubscriptionMutex);
        const auto found = _parameterSubscriptions.find(subscriptionId);
        if (found == _parameterSubscriptions.end() || found->second->connectionId != connectionId)
            return {DeviceParameterError::NotFound,
                "The parameter change subscription is not active.",
                DeviceParameterEffect::None, std::nullopt};
        subscription = found->second;
        subscription->active = false;
        _parameterSubscriptions.erase(found);
    }
    {
        // Wait for an in-flight listener so no callback outlives the release.
        std::lock_guard<std::mutex> delivery(subscription->deliveryMutex);
    }
    completion({});
    return {};
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
    const auto acquisition = _camera->acquisitionCapabilities();
    const DeviceConnectionAvailability single{acquisition.singleAvailable,
        acquisition.singleReason.empty() ? "AcquisitionMode does not offer SingleFrame."
                                         : acquisition.singleReason};
    return {yes, yes, yes, yes, single, yes};
}

void CameraSourceController::reportAcquisitionStart(const CameraAcquisitionStart& start,
    const char* action)
{
    if (!start.started) {
        qCWarning(cameraSourceDiagnosticLog).noquote() << QStringLiteral(
            "[Camera] acquisition refused: action=%1 reason=%2")
            .arg(QString::fromLatin1(action), QString::fromStdString(start.reason));
        return;
    }
    if (!start.modeChanged) return;
    qCInfo(cameraSourceDiagnosticLog).noquote() << QStringLiteral(
        "[Camera] AcquisitionMode changed: action=%1 previous=%2 current=%3")
        .arg(QString::fromLatin1(action), QString::fromStdString(start.previousMode),
             QString::fromStdString(start.currentMode));
    qCDebug(cameraSourceDiagnosticLog).noquote() << "@diagnostic " + QString::fromUtf8(QJsonDocument(QJsonObject{
        {"event", "acquisition_mode"}, {"fields", QJsonObject{
            {"action", QString::fromLatin1(action)},
            {"previous", QString::fromStdString(start.previousMode)},
            {"current", QString::fromStdString(start.currentMode)},
            {"changed", true}}}}).toJson(QJsonDocument::Compact));
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
        // claims and the native connection move under one lock, so concurrent connect
        // requests in one session cannot release a claim another request already took.
        std::lock_guard<std::mutex> connectionLock(_connectionWorkMutex);
        auto state = _camera->connectionState();
        if (state.connected)
            return {DeviceConnectionError::Conflict, "Disconnect the current target explicitly before connecting another."};
        // A network or USB camera is the exclusive resource per session. The key is derived
        // from the exact target before the SDK open. A target that changed without a completed
        // disconnect releases the old key first; otherwise a removed camera would leave one
        // session holding A while it opens B.
        const auto claimKey = _claims
            ? playgroundCameraResourceKey(QString::fromStdString(_camera->deviceSerialNumber(targetId)))
            : QString();
        if (_claims && _claimKey != claimKey) releaseClaim();
        if (_claims && _claim && !*_claim && !claimKey.isEmpty()) {
            QString claimError;
            *_claim = _claims->claimResource(claimKey, &claimError);
            if (!*_claim) {
                return {DeviceConnectionError::Conflict, claimError.toStdString(),
                    DeviceConnectionEffect::None, std::nullopt};
            }
            _claimKey = claimKey;
        }
        const auto outcome = _camera->controlConnection(true, targetId, state.generation, &state);
        if (outcome != CameraConnectionOutcome::Success) releaseClaim();
        return connectionOutcome(outcome, state);
    }, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, connectionId = std::move(connectionId)]() -> DeviceConnectionResult {
        // claims and the native connection move under one lock, so concurrent connect
        // requests in one session cannot release a claim another request already took.
        std::lock_guard<std::mutex> connectionLock(_connectionWorkMutex);
        auto state = _camera->connectionState();
        if (!state.connected) {
            // A device removal can invalidate the connection without a disconnect request.
            releaseClaim();
            return {DeviceConnectionError::Disconnected, "The camera is disconnected."};
        }
        if (connectionIdFor(state) != connectionId) return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
        const auto outcome = _camera->controlConnection(false, {}, state.generation, &state);
        if (outcome == CameraConnectionOutcome::Success) releaseClaim();
        return connectionOutcome(outcome, state);
    }, std::move(completion));
}

namespace {
/** @brief Waits for the one-shot grab worker without holding a camera or controller lock. */
struct SingleGrabWait {
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    std::size_t delivered = 0;
};
}

DeviceConnectionResult CameraSourceController::requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, connectionId = std::move(connectionId)]() -> DeviceConnectionResult {
        auto state = _camera->connectionState();
        if (!state.connected) return {DeviceConnectionError::Disconnected, "The camera is disconnected."};
        if (connectionIdFor(state) != connectionId) return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
        if (_isGrabbing.load()) return {DeviceConnectionError::Busy, "Stop Live explicitly before single acquisition."};
        auto wait = std::make_shared<SingleGrabWait>();
        const auto started = _camera->startAcquisition(CameraAcquisitionAction::Single,
            [wait](const std::size_t delivered) {
                std::lock_guard<std::mutex> lock(wait->mutex);
                wait->delivered = delivered;
                wait->finished = true;
                wait->condition.notify_all();
            });
        reportAcquisitionStart(started, "single");
        if (!started.started)
            return {DeviceConnectionError::Unavailable, started.reason,
                started.modeChanged ? DeviceConnectionEffect::Applied : DeviceConnectionEffect::None,
                copiedConnectionState(_camera->connectionState())};
        std::unique_lock<std::mutex> lock(wait->mutex);
        if (!wait->condition.wait_for(lock, std::chrono::seconds(30), [&] { return wait->finished; })) {
            lock.unlock();
            _camera->stop();
            return {DeviceConnectionError::Timeout,
                "Single acquisition did not finish within 30 s; accepted effects may have occurred.",
                DeviceConnectionEffect::Applied, copiedConnectionState(_camera->connectionState())};
        }
        if (wait->delivered == 0) {
            lock.unlock();
            _camera->stop();
            return {DeviceConnectionError::BackendFailure, "Single acquisition delivered no frame.",
                DeviceConnectionEffect::Applied, copiedConnectionState(_camera->connectionState())};
        }
        return {{}, {}, DeviceConnectionEffect::Applied, copiedConnectionState(_camera->connectionState())};
    }, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestLiveStart(std::string connectionId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, connectionId = std::move(connectionId)]() -> DeviceConnectionResult {
        auto state = _camera->connectionState();
        if (!state.connected) return {DeviceConnectionError::Disconnected, "The camera is disconnected."};
        if (connectionIdFor(state) != connectionId) return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
        if (_isGrabbing.load()) return {DeviceConnectionError::Conflict, "Live acquisition is already running."};
        const auto started = _camera->startAcquisition(CameraAcquisitionAction::Live);
        reportAcquisitionStart(started, "live");
        if (!started.started)
            return {DeviceConnectionError::Unavailable, started.reason,
                started.modeChanged ? DeviceConnectionEffect::Applied : DeviceConnectionEffect::None,
                copiedConnectionState(_camera->connectionState())};
        return {{}, {}, DeviceConnectionEffect::Applied, copiedConnectionState(_camera->connectionState())};
    }, std::move(completion));
}

DeviceConnectionResult CameraSourceController::requestLiveStop(std::string connectionId, IDeviceConnectionControl::Completion completion) {
    return scheduleConnectionWork([this, connectionId = std::move(connectionId)]() -> DeviceConnectionResult {
        auto state = _camera->connectionState();
        if (!state.connected) return {DeviceConnectionError::Disconnected, "The camera is disconnected."};
        if (connectionIdFor(state) != connectionId) return {DeviceConnectionError::Conflict, "The connection changed; observe it again."};
        if (!_isGrabbing.load()) return {DeviceConnectionError::Conflict, "Live acquisition is not running."};
        _camera->stop();
        return {{}, {}, DeviceConnectionEffect::Applied, copiedConnectionState(_camera->connectionState())};
    }, std::move(completion));
}
