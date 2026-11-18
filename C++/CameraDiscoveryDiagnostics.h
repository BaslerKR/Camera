#ifndef CAMERA_DISCOVERY_DIAGNOSTICS_H
#define CAMERA_DISCOVERY_DIAGNOSTICS_H

/**
 * @file CameraDiscoveryDiagnostics.h
 * @brief Operator detail for a pylon discovery run that enumerates no devices.
 */

#include <cstddef>
#include <string>
#include <vector>

namespace CameraDiscoveryDiagnostics {

/** @brief One transport layer reported by pylon's transport layer factory. */
struct TransportLayer {
    std::string name;
    std::string version;
};

/** @brief The transport layer query result and the layers it produced. */
struct TransportLayerSnapshot {
    bool available = true;
    std::vector<TransportLayer> layers;
};

/** @brief One local IPv4 interface, used to spot a camera on another subnet. */
struct NetworkInterface {
    std::string name;
    std::string address;
};

/**
 * @brief Supplies the pylon state observed at discovery time.
 * @note The camera module wraps CTlFactory here; tests supply a fixed fake.
 */
class Environment {
public:
    virtual ~Environment() = default;
    virtual std::string pylonVersion() const = 0;
    /** @brief Queries once; an unavailable result means the query itself failed. */
    virtual TransportLayerSnapshot transportLayers() const = 0;
    virtual std::vector<NetworkInterface> localInterfaces() const = 0;
};

/** @brief Appends the local interface suffix shared by every zero-device path. */
inline void appendLocalInterfaces(std::string& message,
    const std::vector<NetworkInterface>& interfaces)
{
    message += "; local IPv4 interfaces: ";
    if (interfaces.empty()) {
        message += "none";
        return;
    }
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
        if (i != 0U) message += ", ";
        message += interfaces[i].name + "=" + interfaces[i].address;
    }
}

/**
 * @brief Composes the zero-device diagnostic for the existing discovery log.
 * @param environment The pylon version, transport layers and interfaces to report.
 * @return One line naming the version, loaded layers and local IPv4 interfaces.
 */
inline std::string emptyDiscoveryMessage(const Environment& environment)
{
    const std::vector<NetworkInterface> interfaces = environment.localInterfaces();
    std::string message = "pylon initialized but enumerated 0 devices; pylon version: "
        + environment.pylonVersion() + "; loaded transport layers: ";
    const TransportLayerSnapshot snapshot = environment.transportLayers();
    if (!snapshot.available) {
        message += "unavailable";
        appendLocalInterfaces(message, interfaces);
        return message;
    }
    if (snapshot.layers.empty()) {
        message += "none";
        appendLocalInterfaces(message, interfaces);
        return message;
    }
    for (std::size_t i = 0; i < snapshot.layers.size(); ++i) {
        if (i != 0U) message += ", ";
        message += snapshot.layers[i].name;
        if (!snapshot.layers[i].version.empty()) message += " (" + snapshot.layers[i].version + ")";
    }
    appendLocalInterfaces(message, interfaces);
    return message;
}

} // namespace CameraDiscoveryDiagnostics

#endif // CAMERA_DISCOVERY_DIAGNOSTICS_H
