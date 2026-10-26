# Camera

Camera is a C++17 acquisition library for Basler cameras supported by the pylon SDK. It owns SDK initialization, discovery, connection state, 2D and multipart 3D grab callbacks, and producer backpressure.

## Capabilities

- Discover and open pylon cameras through `CameraSystem`.
- Receive 2D `CPylonImage` frames or multipart `CPylonDataContainer` payloads.
- Configure Blaze, Stereo ace, and Stereo mini stream families from the connected device class.
- Use the optional Qt control widget when it is explicitly enabled with Qt 5 or Qt 6.
- Redirect module diagnostics through `CameraSystem::syslog()`.

## Requirements

- CMake 3.16 or newer and a C++17 compiler.
- A Basler pylon SDK supported by the target operating system.
- `PYLON_ROOT`, `PYLON_DEV_DIR`, or `pylon_DIR` when CMake cannot discover pylon automatically.
- The matching supplementary package and runtime producer for supported stereo 3D devices.
- Qt Core, Gui, and Widgets only when the optional Qt surface is enabled.

## Integration

Add the module from any consumer-owned location and link its core target:

```cmake
add_subdirectory(path/to/Camera/C++ Camera-build)
target_link_libraries(consumer PRIVATE Camera)
```

`CAMERA_BUILD_QT_WIDGET` defaults to `OFF`, so configuring the core does not search for or link Qt. To build the control widget and image converter for a Qt consumer:

```cmake
set(CAMERA_BUILD_QT_WIDGET ON CACHE BOOL "Build the Camera Qt widget")
add_subdirectory(path/to/Camera/C++ Camera-build)
target_link_libraries(qt_consumer PRIVATE Camera::QtWidget)
```

`Camera::QtWidget` provides `Utility/Qt/QCameraWidget.h` and `Utility/Qt/QtConverter.h` and links the `Camera` core transitively. Enabling the option requires Qt Core, Gui, and Widgets; configuration fails clearly when they are unavailable.

The core links the SDK through the module-owned `Camera::Pylon` interface target. The vendor `pylon::pylon` imported target is left unchanged; on Linux, Camera's loader-link policy is applied only to its own interface. Consumers should link `Camera` rather than modifying the vendor target.

The optional Playground adapter is disabled by default. Enable `CAMERA_BUILD_PLAYGROUND_ADAPTER` only when the neutral GraphicsEngine contract target is available. Consumers that want owned frames include `Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h` and link `Camera::PlaygroundAdapter`; pylon payload conversion stays in `PylonGraphicsFrameAdapter`.

The optional Playground device plugin is a separate MODULE. Enable `CAMERA_BUILD_PLAYGROUND_PLUGIN` only when `Playground::DevicePlugin`, the adapter, and `Camera::QtWidget` are available. It does not belong in the adapter static library. Package identity lives in `Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DevicePlugin.h`. The host emits `plugin.json` from that identity plus `DevicePluginPackage.h`.

## SDK discovery validation

Windows library defaults come from the selected `Findpylon.cmake` installation.
Explicit finder/module paths and library overrides take precedence over a
machine-wide pylon installation; pylon 12 name defaults fill unresolved values.

Linux first searches for the pylon config package using `pylon_DIR`, standard CMake prefixes, `PYLON_ROOT`, and `/opt/pylon` (including its `share/pylon/cmake` layout). If no config package is available, it tries the SDK-provided `Findpylon.cmake`. Windows retains find-module discovery; macOS retains framework-target discovery.

Run the configure-only regression suite with Python 3, CMake, and Ninja on `PATH`:

```sh
python path/to/Camera/C++/Tests/test_pylon_discovery.py --work-dir build/pylon-discovery-tests
```

The ten synthetic SDK-layout cases cover Linux default/custom prefixes, explicit config directories, config preference over a find module, module-only SDKs, missing SDKs, Windows find modules, macOS framework targets, and a consumer-provided SDK target. These checks do not compile vendor headers or establish native Linux/macOS or device acceptance.

## Acquisition Contract

`Camera::accessFeatures()` executes short feature work under the owning recursive camera and node-map locks, reporting a connection generation to the callback. It returns false while disconnected. Callbacks must not retain node pointers, change camera lifecycle or acquire unrelated locks. The optional device adapter exposes copied remote-node descriptors and typed parameter commands through `IDeviceParameterControl/1.0` on `CameraSourceController`; integer values stay signed 64-bit. Requests carry a controller-instance/connection-generation identity and the complete selecting-feature context. Observed revisions describe queried state, not every intermediate hardware event. Writes revalidate current access and constraints, retain SDK rounding in readback, and distinguish no effect, accepted effect and uncertain SDK completion. There is no implicit selector change, acquisition restart or multi-feature transaction. Root invalidation uses the existing coalesced UI path after SDK locks are released; live refresh remains deferred until stop.

```cpp
#include "CameraSystem.h"

CameraSystem system;
Camera* camera = system.addCamera();

const auto callbackId = camera->registerGrabCallback(
    [camera](const Pylon::CPylonImage& image, std::size_t sequence) {
        if (image.IsValid()) {
            // Consume the SDK-backed image before returning the credit.
        }
        camera->ready();
    });

if (camera->open()) {
    camera->grab();
}
```

Grab callbacks run on an acquisition thread. Do not update GUI objects directly from a callback. For continuous free-run acquisition, call `ready()` only after the consumer has finished with the current 2D or multipart frame; the credit is bounded and the next frame otherwise remains blocked. The Qt control coalesces duplicate feature-node notifications and defers live feature-tree refresh until grabbing stops, preventing camera-event traffic from growing the GUI event queue. Deregister callbacks, stop acquisition, close the camera, and remove it from `CameraSystem` before destroying dependent consumer state.

Long-running acquisition logs a sampled worker-progress record every ten seconds and warns when a frame callback takes at least 500 ms. These diagnostics distinguish SDK receive activity from consumer/presentation backpressure without changing ordered frame delivery.

Multipart 3D buffers remain SDK-owned for the callback duration. Consumers that retain data after the callback must create their own validated representation.

## Validation

The feature authority also serializes connection setup and teardown. Supplying the optional `busy` output to `accessFeatures()` selects nonwaiting admission; competing feature/lifecycle work returns false with busy set. Without that output, existing worker writes wait for their serialized turn. SDK calls inside an admitted turn retain vendor timeout behavior. Invoke the entry point from consumer command workers or the owning GUI adapter, never recursively from its callback or from a vendor lifecycle callback. The software emulator test covers exact readback, competing work rejection and reconnect identities; it opens only a device with the pylon emulation class, even when other devices are discoverable.

Build the core against the installed SDK on every supported platform. Stereo decoding, trigger behavior, device configuration, and runtime producers require matching physical-hardware validation; a successful library build does not establish those contracts.
# Declared device lifecycle capability

The optional source-controller adapter declares `device.targets`, `device.discover`, `device.connection`, `device.connect` and `device.disconnect` through the neutral `IToolProvider` contract. Discovery returns copied full transport identities and labels. Connection commands select exact identities, refuse implicit replacement, and fence stale generations under the existing lifecycle lock. SDK work uses the existing bounded adapter workers; GUI catalog updates receive copied queued results. These generations identify connection lifetimes, not settings-to-acquisition causality.

## Installed consumer SDK plugin build

Build the existing plugin project using this repository, an installed Plugin
SDK, the matching Qt kit and its vendor SDK. No consumer source tree or
renderer is needed. The plugin switch enables its owning widget/adapter
(and vendor SDK implementation where required). Theme installation remains
consumer-owned.

```sh
cmake -S C++ -B build/plugin -DCMAKE_PREFIX_PATH="<PluginSDK>;<Qt-prefix>" -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release -DCAMERA_BUILD_PLAYGROUND_PLUGIN=ON -DCAMERA_BUILD_TESTS=ON
cmake --build build/plugin --config Release --parallel 4
ctest --test-dir build/plugin -C Release --output-on-failure
cmake --build build/plugin --config Release --target CameraPluginPackageStage
```

Supply the existing vendor SDK options when it is not in the documented default
location. The generated package is `build/plugin/packages/Release/plugins/<id>/current`
on multi-configuration generators and retains the module-owned runtime layout.
It includes the manifest, icon and SDK identity. Staging reconciles only this
generated package. Use a fresh build directory or explicitly update
`PlaygroundPluginSDK_DIR` when selecting another SDK. This Release software
gate does not establish physical-device, Debug or cross-platform acceptance.
Set `CMAKE_BUILD_TYPE=Release` for a single-configuration generator.

## Qt presentation

`C++/Utility/Qt` provides the SDK-independent `CameraPresentation` target.
The runtime Qt widget and isolated presentation consumers use the same
`CameraControlUi` composition; device discovery, feature access and
acquisition remain in the runtime widget. The presentation target does not
install an application theme.
