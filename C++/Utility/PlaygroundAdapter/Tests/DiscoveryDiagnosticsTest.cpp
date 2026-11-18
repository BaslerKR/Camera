#include "CameraDiscoveryDiagnostics.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/** @brief Fixed pylon state used in place of CTlFactory by the diagnostic test. */
class FakeBackend final : public CameraDiscoveryDiagnostics::Environment {
public:
    FakeBackend(std::string version,
        std::vector<CameraDiscoveryDiagnostics::TransportLayer> layers,
        bool available = true,
        std::vector<CameraDiscoveryDiagnostics::NetworkInterface> interfaces = {})
        : _version(std::move(version)), _layers(std::move(layers)), _available(available),
          _interfaces(std::move(interfaces)) {}

    std::string pylonVersion() const override { return _version; }
    CameraDiscoveryDiagnostics::TransportLayerSnapshot transportLayers() const override
    {
        return {_available, _layers};
    }
    std::vector<CameraDiscoveryDiagnostics::NetworkInterface> localInterfaces() const override
    {
        return _interfaces;
    }

private:
    std::string _version;
    std::vector<CameraDiscoveryDiagnostics::TransportLayer> _layers;
    bool _available;
    std::vector<CameraDiscoveryDiagnostics::NetworkInterface> _interfaces;
};

/** @brief Fails inside the query, as the production CTlFactory wrapper can. */
class ThrowingBackend final : public CameraDiscoveryDiagnostics::Environment {
public:
    std::string pylonVersion() const override { return "12.5.2.1565"; }
    CameraDiscoveryDiagnostics::TransportLayerSnapshot transportLayers() const override
    {
        try {
            throw std::runtime_error("TL query failed");
        } catch (const std::runtime_error&) {
            return {false, {}};
        }
    }
    std::vector<CameraDiscoveryDiagnostics::NetworkInterface> localInterfaces() const override
    {
        return {};
    }
};

bool contains(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

} // namespace

int main()
{
    const FakeBackend loaded("12.5.1.1234", {
        {"BaslerGigE", "1.2.3"},
        {"BaslerUsb", "2.0.0"},
    });
    const std::string message = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(loaded);
    assert(contains(message, "0 devices"));
    assert(contains(message, "12.5.1.1234"));
    assert(contains(message, "BaslerGigE"));
    assert(contains(message, "1.2.3"));
    assert(contains(message, "BaslerUsb"));
    assert(contains(message, "2.0.0"));
    assert(message.find("BaslerGigE") < message.find("BaslerUsb"));

    const FakeBackend empty("11.4.0.1134", {});
    const std::string emptyMessage = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(empty);
    assert(contains(emptyMessage, "11.4.0.1134"));
    assert(contains(emptyMessage, "loaded transport layers: none"));

    const FakeBackend noVersions("8.0.0", {{"BaslerCamEmu", ""}});
    const std::string noVersionMessage =
        CameraDiscoveryDiagnostics::emptyDiscoveryMessage(noVersions);
    assert(contains(noVersionMessage, "BaslerCamEmu"));
    assert(!contains(noVersionMessage, "()"));

    const FakeBackend failedQuery("12.5.2.1565", {}, false);
    const std::string failedMessage = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(failedQuery);
    assert(contains(failedMessage, "12.5.2.1565"));
    assert(contains(failedMessage, "loaded transport layers: unavailable"));
    assert(!contains(failedMessage, "loaded transport layers: none"));

    const ThrowingBackend throwing;
    const std::string thrownMessage = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(throwing);
    assert(contains(thrownMessage, "loaded transport layers: unavailable"));
    assert(!contains(thrownMessage, "loaded transport layers: none"));

    const FakeBackend subnetMismatch("12.5.2.1565", {{"GigE", ""}}, true,
        {{"en7", "192.168.0.146"}, {"lo0", "127.0.0.1"}});
    const std::string networkMessage = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(subnetMismatch);
    assert(contains(networkMessage, "local IPv4 interfaces: en7=192.168.0.146, lo0=127.0.0.1"));

    const FakeBackend noInterfaces("12.5.2.1565", {{"GigE", ""}});
    const std::string noNetworkMessage = CameraDiscoveryDiagnostics::emptyDiscoveryMessage(noInterfaces);
    assert(contains(noNetworkMessage, "local IPv4 interfaces: none"));
    return 0;
}
