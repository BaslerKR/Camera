## Unreleased

- Drop the device widget ABI and the QVariantMap discovery hand-back. `IDevicePluginSession` no longer implements `createDockWidgets()`, and `discoverDevices()`/`createSession()` no longer pass discovery data through a `QVariantMap`; discovery and session creation now take only an error message.

- Build against the widget-free device runtime SDK: the contract target is `Playground::DeviceRuntimeContract`, headers live under `DeviceRuntime/Contract`, and the interface is `IDeviceRuntime`. The plugin object derives directly from `QObject`/`IDeviceRuntime`, so the package needs neither the widget-side template nor Qt Widgets headers.

- Remove the Qt widget library and the widget tests. `QCameraWidget`, `CameraControlUi`, `CameraPresentation`, `CameraQtWidget` and `createDockWidgets()`'s widget path are gone; the plugin session creates no widget or dock and the host's common device panel owns the session UI. The package target links Qt Core and Concurrent only: no Qt Widgets, Resources widget or dialog library, and no OpenGL. Single during Live now refuses with "Stop Live before a single grab."

- Exclude the pylon Windows runtime's standalone Qt Widgets design tools (DataProcessing GUI and VTool editor DLLs), their OpenGL helper `glew32.dll`, and any private Qt payload from the staged package, so the widget-free package has no Qt Widgets or OpenGL dependency on Windows as it does on macOS.

- Favourites saved by the retired widget are dropped (owner decision in issue #440). The Host panel uses its default `Plugins/camera/Favorites` key; no legacy key or migration is shipped.

- Publish the GenApi feature tree through parameter descriptors: category path, GenICam visibility mapped to the API presentation level, representation hint, selector relationships in both directions and the transport-layer shared-scope marker. A bounded change subscription delivers one full snapshot and revision-stamped deltas from the Camera's node and acquisition-status callbacks, resynchronizes with a snapshot when its queue overflows, and re-observes writability when acquisition starts or stops; the GUI does not poll.

- Claim one network or USB camera per session through Playground's host hardware-resource claims. The key is the camera serial, so a second session of this package is refused with the owning session named instead of reaching the transport. The claim is released on disconnect and session close, and a camera without a serial keeps the SDK's native arbitration.
- Select the acquisition mode from the requested action. Single (`device.single` and the Host panel's Single action) writes `AcquisitionMode=SingleFrame` before it acquires one native frame; Live (`source.start`, `device.live-start` and the Host panel's Live action) writes `AcquisitionMode=Continuous`. Only `AcquisitionMode` is written, so trigger and other user settings stay. A changed value is logged at INFO and recorded in `diagnostics.Camera`; Single is declared unavailable with the node-map reason when `AcquisitionMode` is missing, unavailable, unwritable or has no `SingleFrame`, and an unwritable mode refuses the action instead of stopping a continuous stream.
- `Camera::startAcquisition()` replaces the public raw `grab()` entry point used by the controller. `acquisitionCapabilities()` reports Single availability from the observed remote node map without an SDK wait, and Live stop re-observes it before offering Single again.

- Deliver Basler blaze 3D range data through the GraphicsEngine range writer into Playground shared frame storage. The range channels are written once into provided storage; an empty result drops the range. A blaze frame no longer carries a second explicit point cloud of the same points: GraphicsEngine derives it from the range, and the range RGB channel carries the grayscale intensity colours the explicit cloud used.

- Report the connection's owned source and its delivered frame count in `device.connection` and `device.connections`, and publish the same source through `source.sources`. The connection state copies the device identity as the stable source id at index 0 and the delivered count from the same producer snapshot as `source.diagnostics`, so a per-connection status bar can show a counter and map a partial start back to its connection.

## v1.4.0 (2026-11-14)

- Require Playground 0.7.0 or later; this package uses the frame storage interface introduced in that release.

- Deliver 2D live images directly into Playground's shared frame storage, so scripts and viewers can reuse each image without an extra copy. A frame is skipped when Playground cannot provide storage; 3D scenes are unchanged.

## v1.3.0 (2026-11-13)

- Require Playground 0.6.6 or later; this package uses the device plugin interface introduced in that release.

- Expose producer runtime counters through the common source diagnostics query, using the same observations as the device panel.

- Show live producer telemetry in the device statusbar while Playground Debug is on: an `fps` bubble over the delivered-frame delta and a warning `failed` bubble only when a grab result failed. Turning Debug on or off during Live takes effect within one second and shows current values, because counting continues while the bubbles are hidden. Both disappear when grabbing stops; `RetrieveResult` timeouts are not counted as failures.
- Write the five-second `[Camera UI] diagnostics` line to System Logs only while Debug is on. Incomplete-grab warnings and errors are still always logged.
- Remove the free-run consumer `ready()` permit. Grab callbacks no longer wait for downstream processing, and free-run acquisition keeps the newest pylon result instead of pacing the SDK worker behind the consumer. The free-run `MaxNumBuffer` rises from 5 to 6, adding about 127 MB of SDK buffer storage at 127 MP.
- Connect, disconnect and discover cameras through Playground's standard device connection tools, which work the same for every device. The connection is identified by one id that changes on every reconnect and also addresses device features; Single and Live stay on the common source controls. Builds against Plugin SDK 0.2 and requires a Playground release with the standard device connection interface.
- Run the adapter connection and parameter tests (`camera_parameter_control_test`) with the owner tests; their emulator cases skip when the pylon camera emulator is not installed.
- Declare what kind of data each automation tool returns, so Playground can limit what connected assistants receive. Requires a Playground release with the updated automation interface; earlier releases do not list this package's automation tools.

- Take the binary display name from the package definition.

## v1.2.1 (2026-10-20)

- Use the shared neutral camera artwork for the package icon.

- Use the shared connect and disconnect artwork for connection controls.

- Use the shared refresh, single-grab, and live glyphs on the device toolbar.

- Publish the independently built device package with installed SDK validation.

- Declare Windows pylon processing/stereo runtime paths only when those optional installer components are staged.
- Resolve Windows pylon 12 library defaults from the selected finder without replacing explicit library paths.
- Document explicit Release-only installed SDK configuration.
- Build the standalone device plugin against an installed consumer SDK and stage its module-owned runtime package.

- Discover Linux pylon config packages from explicit CMake paths, PYLON_ROOT, and /opt/pylon before trying the SDK find module; preserve Windows and macOS discovery.

## v1.2.2 (2026-10-29)

- Disable connection changes and single-grab commands during acquisition; recompute availability after pending operations and restore it after stop. Reject connection toggles while acquisition is active.

- Show the next Live grab command with cameras/stop artwork in all pointer and focus states, and keep its tooltip and accessible name synchronized with acquisition state.

- Connect icons name the current state (`disconnected`/`connect`); a consumer theme can preview the other state on hover.

- Declare semantic layout, device-panel and action-density scopes through Qt properties; preserve independent plain-Qt builds without a theme dependency.

- Declare only the staged runtime directories in the macOS package, so the host admits it.

- Run the configure-only pylon discovery checks with make on Linux and macOS hosts without Ninja.

- Share the SDK-independent Qt control composition between the runtime widget and isolated presentation consumers.

## v1.2.0 (2026-10-13)

- Deliver declared device-tool completion exactly once even when a consumer callback throws; drain accepted workers on controller teardown.
- Release cached SDK device descriptors before terminating the pylon runtime.
- Declare neutral device discovery/connection tools on the source controller; select exact transport identities, refuse stale/busy requests or implicit device switching, and share existing SDK lifecycle authority with GUI controls. Refresh GUI device labels through queued copied results.

- Expose copied remote-device parameter descriptions, typed reads/writes and commands through an optional source-controller capability. Revalidate connection generations, selector context and observed revisions under SDK locks; preserve accepted/uncertain effects when readback fails.
- Admit parameter requests on the consumer thread and execute SDK access on bounded module-owned workers. Deliver copied completion values and drain accepted work before releasing the controller and SDK.
- Serialize Qt parameter writes through the same owning camera/node-map locks and invalidate dependent feature views after external commands without changing acquisition or live refresh policy.

## v1.1.0 (2026-10-08)

- Report acquisition lifecycle to consumers and retain opt-in stopped-device feature snapshots at the current selector state without changing capture policy.

- Declare the Camera display name, Basler pylon subtitle, and acquisition summary in package metadata.

- Publish image and range output as identified GraphicsFrame resources with an explicit source group and validated pixel-registration edge.

- Limit the control statusbar to Idle, Connected, and Live; route operation messages to application logs.

- Publish the camera controls through the generic host-managed plugin dock contract.

- Link `CameraPlugin` only through `Camera::QtWidget` and `Camera::PlaygroundAdapter` so `libCamera.a` is not passed twice to the linker.

- Surface incomplete, timed-out, and SDK-reported grab failures with camera
  identity and Pylon error details; sample repeated diagnostics after the first
  failure to keep acquisition logs bounded.

- Require ABI / Qt IID 5.0 and recompilation; older packages are rejected. Declare immutable GraphicsEngine + Script Editor capabilities directly on the session.

- Stage `pylon.framework` in the Playground plugin `runtime` payload on macOS instead of the host `Contents/Frameworks` extra list.

- Link `Playground::DeviceRuntimeContract` for the `IDeviceRuntime` MODULE.
- Own package identity in `Utility/PlaygroundAdapter/Package/Package.cmake`; the host emits `plugin.json` from `DeviceRuntime.h`.
- Keep the session source controller in `Utility/PlaygroundAdapter/Source` as `CameraSourceController`.
- Declare the Playground plugin runtime payload from this module; the host copies it into the package.
- Move the GraphicsFrame stream/adapter into `Utility/PlaygroundAdapter` as `Camera::PlaygroundAdapter`.
- Publish GraphicsFrame through `PylonGraphicsFrameStream`; the converter header stays in the adapter translation unit.

- Drain in-flight GraphicsFrame adapter callbacks before stream destruction and cover the shared callback gate contract.
- Remove the unused Qt converter header from the core source list.
- Extend the adapter test with empty multipart 3D dispatch coverage for Blaze, Stereo mini, and Stereo ace profiles.
- Move SDK image/3D callback registration and GraphicsFrame conversion into the module adapter stream; the parent receives only owned GraphicsFrame values.
- Move 2D pylon conversion into the GraphicsFrame adapter, keep image storage owned, and remove Qt from the adapter target.
- Mark direct XYZ conversions as `ExplicitXY` so analysis uses producer coordinates instead of uniform-axis inference.
- Reject malformed stride layouts and non-finite or non-positive disparity calibration before conversion.
- Route camera 3D handoff through the host-owned `GraphicsFrame` payload contract.
- Remove an unused component-mapping helper and correct camera-list index typing; keep the optional Qt refresh path free of dead state.
- Initialize range-field `bitsPerSample` with `std::uint8_t` literals so brace initialization stays well-formed on Clang and GCC.
- Keep long-running grabs responsive by coalescing feature-node notifications, deferring feature-tree refresh until grab stop, bounding free-run credits, and logging sampled worker/callback latency diagnostics.

- Preserve semantic Z coordinate, Depth, Intensity, and Confidence field metadata in converted 3D payloads, including source bit depth where available.
- Split the opt-in Qt control widget and image converter into `Camera::QtWidget`; the `Camera` core target no longer discovers or publicly links Qt.
- Updated the optional scene adapter to consume a neutral scene-contract target without inheriting the visualization runtime; its conversion output is unchanged.
- Keep the vendor `pylon::pylon` imported target unchanged by applying the Linux loader-link policy through the module-owned `Camera::Pylon` interface target. Pre-discovered pylon targets are linked consistently.
- Replace the corrupted host-layout README with a standalone, code-aligned acquisition and integration contract.

## v1.0.1

- Disable the camera selector when discovery returns no cameras and enable it again when a camera is available.
- Declare millimeter XYZ output explicitly for direct and reconstructed organized 3D scene adapters.
