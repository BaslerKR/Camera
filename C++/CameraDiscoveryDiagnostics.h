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

/**
 * @brief Supplies the pylon state observed at discovery time.
 * @note The camera module wraps CTlFactory here; tests supply a fixed fake.
 */
class Environment {
public:
    virtual ~Environment() = default;
    virtual std::string pylonVersion() const = 0;
    virtual std::vector<TransportLayer> transportLayers() const = 0;
    /** @brief False when transport layer enumeration itself failed. */
    virtual bool transportLayersAvailable() const = 0;
};

/**
 * @brief Composes the zero-device diagnostic for the existing discovery log.
 * @param environment The pylon version and loaded transport layers to report.
 * @return One line naming the version and every loaded transport layer.
 */
inline std::string emptyDiscoveryMessage(const Environment& environment)
{
    std::string message = "pylon initialized but enumerated 0 devices; pylon version: "
        + environment.pylonVersion() + "; loaded transport layers: ";
    if (!environment.transportLayersAvailable()) {
        message += "unavailable";
        return message;
    }
    const std::vector<TransportLayer> layers = environment.transportLayers();
    if (layers.empty()) {
        message += "none";
        return message;
    }
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i != 0U) message += ", ";
        message += layers[i].name;
        if (!layers[i].version.empty()) message += " (" + layers[i].version + ")";
    }
    return message;
}

} // namespace CameraDiscoveryDiagnostics

#endif // CAMERA_DISCOVERY_DIAGNOSTICS_H
