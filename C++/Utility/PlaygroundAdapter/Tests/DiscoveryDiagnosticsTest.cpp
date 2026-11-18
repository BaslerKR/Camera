#include "CameraDiscoveryDiagnostics.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

/** @brief Fixed pylon state used in place of CTlFactory by the diagnostic test. */
class FakeBackend final : public CameraDiscoveryDiagnostics::Environment {
public:
    FakeBackend(std::string version,
        std::vector<CameraDiscoveryDiagnostics::TransportLayer> layers)
        : _version(std::move(version)), _layers(std::move(layers)) {}

    std::string pylonVersion() const override { return _version; }
    std::vector<CameraDiscoveryDiagnostics::TransportLayer> transportLayers() const override
    {
        return _layers;
    }

private:
    std::string _version;
    std::vector<CameraDiscoveryDiagnostics::TransportLayer> _layers;
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
    return 0;
}
