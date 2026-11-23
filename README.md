# Camera

Camera is a C++17 acquisition library for Basler cameras supported by the pylon SDK. It owns SDK initialization, discovery, connection state, 2D and multipart 3D grab callbacks, and the free-run latest-image drop policy.

## Capabilities

- Discover and open pylon cameras through `CameraSystem`.
- Receive 2D `CPylonImage` frames or multipart `CPylonDataContainer` payloads.
- Configure Blaze, Stereo ace, and Stereo mini stream families from the connected device class.
- Ship a widget-free Playground device package; the host's common device panel owns the session UI.
- Redirect module diagnostics through `CameraSystem::syslog()`.

## Requirements

- CMake 3.16 or newer and a C++17 compiler.
- A Basler pylon SDK supported by the target operating system. On macOS this is the SDK framework installed at `/Library/Frameworks/pylon.framework`; pylon Viewer's embedded framework is a runtime-only bundle and is not an SDK. The SDK and Viewer must come from the same pylon release so staged libraries match the build headers.
- `PYLON_ROOT`, `PYLON_DEV_DIR`, or `pylon_DIR` when CMake cannot discover pylon automatically. On macOS every explicit hint (`pylon_cmake`, `pylon_ROOT`, `pylon_DIR`, `PYLON_ROOT`, `PYLON_DIR`, from CMake or the environment) must resolve to a framework with both `Headers/` and `pylon-targets.cmake`; an unresolved hint is a configure error that names the path instead of falling back to another install, and hints that disagree fail configuration.
- The matching supplementary package and runtime producer for supported stereo 3D devices.
- Qt Core and Concurrent only when the Playground device plugin is built; no Qt Widgets, Resources widget or OpenGL dependency.

## Integration

Add the module from any consumer-owned location and link its core target:

```cmake
add_subdirectory(path/to/Camera/C++ Camera-build)
target_link_libraries(consumer PRIVATE Camera)
```

Configuring the core never searches for or links Qt. There is no widget target: the Playground device package is widget-free and the host device panel owns discovery, connect, Single, Live and feature browsing.

The core links the SDK through the module-owned `Camera::Pylon` interface target. The vendor `pylon::pylon` imported target is left unchanged; on Linux, Camera's loader-link policy is applied only to its own interface. Consumers should link `Camera` rather than modifying the vendor target.

The optional Playground adapter is disabled by default. Enable `CAMERA_BUILD_PLAYGROUND_ADAPTER` only when the neutral GraphicsEngine contract target is available. Consumers that want owned frames include `Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h` and link `Camera::PlaygroundAdapter`; pylon payload conversion stays in `PylonGraphicsFrameAdapter`.

The optional Playground device plugin is a separate MODULE. Enable `CAMERA_BUILD_PLAYGROUND_PLUGIN` only when `Playground::DeviceRuntimeContract` and the adapter are available; the plugin links Qt Core and Concurrent only. It does not belong in the adapter static library. Package identity lives in `Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DeviceRuntime/Contract/DeviceRuntime.h`. The host emits `plugin.json` from that identity plus `DeviceRuntime.h`.

With a runtime that exposes the optional `IDeviceImageDiagnostics` and `IDeviceRangeDiagnostics`, `CameraPlugin` installs the best-effort sinks before creating a session. The Qt-free adapter records the first failed 2D write rule through `writeGraphicsImageWithFailure()` and the first failed blaze, Stereo mini, or Stereo ace range rule as bounded numeric evidence with the SDK input sequence. `PylonGraphicsFrameStream` forwards neutral evidence to `CameraSourceController`, which maps it to the host enums and emits the first and every 100th failure of each reason per Live run, while Single emits every failure. No pixel payload, calibration coefficient or SDK string crosses the sink, and successful conversion adds no lock or formatting work.

## SDK discovery validation

Windows library defaults come from the selected `Findpylon.cmake` installation.
Explicit finder/module paths and library overrides take precedence over a
machine-wide pylon installation; pylon 12 name defaults fill unresolved values.

Linux first searches for the pylon config package using `pylon_DIR`, standard CMake prefixes, `PYLON_ROOT`, and `/opt/pylon` (including its `share/pylon/cmake` layout). If no config package is available, it tries the SDK-provided `Findpylon.cmake`. Windows retains find-module discovery.

On macOS a single framework supplies both the build headers and the staged runtime. Without a hint, CMake uses `/Library/Frameworks/pylon.framework`, or the framework behind an already-provided, located pylon target; an unlocatable preloaded target is rejected so linking and staging cannot diverge. An explicit hint must resolve to a framework containing `Headers/PylonIncludes.h` and `pylon-targets.cmake`, or configuration fails with the rejected path; a hint inside a `.framework` bundle selects that bundle root, so a realpath under `Versions/A` stages as `pylon.framework`. A hint that disagrees with an already-provided target's framework is also a configure error. There is no fallback to another install and no search inside application bundles. Configure prints the chosen framework and its `libpylonbase` version, and the package stages that same framework. Each enumeration that finds zero devices logs the pylon version, the loaded transport layers and the local IPv4 interfaces through `CameraSystem::syslog()`, marking the layer list `unavailable` when transport-layer enumeration itself fails. The interface addresses let an operator see when a GigE camera is on a different subnet from every local adapter. A failed interface query is logged as `unavailable` instead of `none`; Windows does not enumerate interfaces through this wrapper and reports `local IPv4 interfaces: none`.

Run the configure-only regression suite with Python 3 and CMake on `PATH`. It
uses Ninja when available; Linux and macOS hosts without Ninja use `make`:

```sh
python path/to/Camera/C++/Tests/test_pylon_discovery.py --work-dir build/pylon-discovery-tests
```

The synthetic SDK-layout cases cover Linux default/custom prefixes, explicit config directories, config preference over a find module, module-only SDKs, missing SDKs, Windows find modules, macOS framework targets with root and CMake-directory hints, macOS hints that miss headers or targets, and a consumer-provided SDK target. These checks do not compile vendor headers or establish native Linux/macOS or device acceptance.

## Acquisition Contract

`Camera::accessFeatures()` executes short feature work under the owning recursive camera and node-map locks, reporting a connection generation to the callback. It returns false while disconnected. Callbacks must not retain node pointers, change camera lifecycle or acquire unrelated locks. The optional device adapter exposes copied remote-node descriptors and typed parameter commands through `IDeviceParameterControl/1.0` on `CameraSourceController`; integer values stay signed 64-bit. Descriptors carry the GenApi category path, GenICam visibility mapped to the API's presentation level, the value representation hint, both selector-relationship directions and the transport-layer shared-scope marker, so a consumer never walks the node map itself. Requests carry a controller-instance/connection-generation identity and the complete selecting-feature context. Observed revisions describe queried state, not every intermediate hardware event. Writes revalidate current access and constraints, retain SDK rounding in readback, and distinguish no effect, accepted effect and uncertain SDK completion. There is no implicit selector change, acquisition restart or multi-feature transaction. A change subscription starts with one full snapshot and then delivers revision-stamped deltas from the Camera's GenApi node and acquisition-status callbacks; its bounded queue replaces undelivered deltas with an explicit resync snapshot. Writability is the observed GenICam access mode, never a cached flag: acquisition start and stop re-observe it, and a refused write returns the SDK reason. Root invalidation uses the existing coalesced UI path after SDK locks are released; live refresh remains deferred until stop.

`Camera::startAcquisition()` is the single acquisition entry point used by the controller. Single writes `AcquisitionMode=SingleFrame` and targets one frame; Live writes `AcquisitionMode=Continuous` and continues until stopped. A missing, unavailable or unwritable mode, or an `AcquisitionMode` without `SingleFrame`, refuses the action with that reason. `acquisitionCapabilities()` caches the observed Single availability without an SDK wait, so `device.single` and the host panel's Single action follow the node map; stopping Live re-observes it so a mode locked during acquisition does not leave Single unavailable. A Single request during Live is refused with "Stop Live before a single grab."

```cpp
#include "CameraSystem.h"

CameraSystem system;
Camera* camera = system.addCamera();

const auto callbackId = camera->registerGrabCallback(
    [](const Pylon::CPylonImage& image, std::size_t sequence) {
        if (image.IsValid()) {
            // Own a copy before returning; the SDK reuses the buffer afterwards.
        }
    });

if (camera->open()) {
    camera->startAcquisition(CameraAcquisitionAction::Live);
}
```

Grab callbacks run on an acquisition thread. Do not update GUI objects directly from a callback. Free-run acquisition uses `GrabStrategy_LatestImageOnly` with six SDK buffers; a callback return does not release a host admission credit, and the acquisition worker never waits for consumer processing. When the consumer falls behind, pylon keeps the newest completed image and overwrites older buffered images instead of stalling acquisition. The parameter controller coalesces duplicate feature-node notifications and defers live feature refresh until grabbing stops, preventing camera-event traffic from growing the host's event queue. Deregister callbacks, stop acquisition, close the camera, and remove it from `CameraSystem` before destroying dependent consumer state.

Long-running acquisition logs a sampled worker-progress record every ten seconds and warns when a frame callback takes at least 500 ms. These diagnostics distinguish SDK receive activity from consumer/presentation backpressure without changing ordered frame delivery.

Multipart 3D buffers remain SDK-owned for the callback duration. Consumers that retain data after the callback must create their own validated representation. Blaze range diagnostics retain the SDK's readable byte count when available and distinguish the SDK-reported row stride from the stride chosen by the adapter.

## Validation

The feature authority also serializes connection setup and teardown. Supplying the optional `busy` output to `accessFeatures()` selects nonwaiting admission; competing feature/lifecycle work returns false with busy set. Without that output, existing worker writes wait for their serialized turn. SDK calls inside an admitted turn retain vendor timeout behavior. Invoke the entry point from consumer command workers or the owning GUI adapter, never recursively from its callback or from a vendor lifecycle callback. The software emulator test covers exact readback, competing work rejection and reconnect identities; it opens only a device with the pylon emulation class, even when other devices are discoverable.

Build the core against the installed SDK on every supported platform. Stereo decoding, trigger behavior, device configuration, and runtime producers require matching physical-hardware validation; a successful library build does not establish those contracts.
# Declared device lifecycle capability

The source-controller adapter implements the SDK `IDeviceConnectionControl` capability; Playground declares the canonical `device.targets`, `device.discover`, `device.connection`, `device.connect`, `device.disconnect`, `device.single`, `device.live-start` and `device.live-stop` tools from it. `device.single` is declared available when the remote node map has a writable `AcquisitionMode` with `SingleFrame`, and unavailable with that reason otherwise. Single writes `AcquisitionMode=SingleFrame` before it starts one native frame; Live writes `AcquisitionMode=Continuous` before continuous acquisition. Only `AcquisitionMode` is written, and a changed value is logged at INFO and recorded in the `diagnostics.Camera` structured journal. Single is never emulated by stopping a continuous stream. Discovery returns copied full transport identities and labels. Connect selects one exact `targetId` and refuses implicit replacement. The opaque `connectionId` is this controller's instance plus its reconnect generation, matches the parameter targets of the same connection, and fences stale calls under the existing lifecycle lock. SDK work uses the existing bounded adapter workers; GUI catalog updates receive copied queued results. Connection ids identify connection lifetimes, not settings-to-acquisition causality.

## Installed consumer SDK plugin build

Build the existing plugin project using this repository, an installed Plugin
SDK, the matching Qt kit and its vendor SDK. No consumer source tree or
renderer is needed. The plugin switch enables its adapter and vendor SDK
implementation; no widget or theme installation is pulled into the package.

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
Set `CMAKE_BUILD_TYPE=Release` for a single-configuration generator. The staged
package links no Qt Widgets, Resources widget/dialog library or OpenGL; check
its binaries with `otool -L` on macOS, `readelf -d` on Linux or `dumpbin` on
Windows.
