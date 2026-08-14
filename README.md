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

The optional scene adapter is disabled by default. Enable it only after a neutral scene-contract target is available; the adapter converts SDK payloads without requiring the visualization renderer.

## Acquisition Contract

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

Grab callbacks run on an acquisition thread. Do not update GUI objects directly from a callback. For continuous acquisition, call `ready()` only after the consumer has finished with the current 2D or multipart frame; otherwise the next frame remains blocked. Deregister callbacks, stop acquisition, close the camera, and remove it from `CameraSystem` before destroying dependent consumer state.

Multipart 3D buffers remain SDK-owned for the callback duration. Consumers that retain data after the callback must create their own validated representation.

## Validation

Build the core against the installed SDK on every supported platform. Stereo decoding, trigger behavior, device configuration, and runtime producers require matching physical-hardware validation; a successful library build does not establish those contracts.
