## v1.2.2 (2026-10-20)

- Use the shared neutral camera artwork for the package icon.

- Use the shared connect and disconnect artwork for connection controls.

- Use the shared refresh, single-grab, and live glyphs on the device toolbar.

## v1.2.1 (2026-10-15)

- Publish the independently built device package with installed SDK validation.

- Declare Windows pylon processing/stereo runtime paths only when those optional installer components are staged.
- Resolve Windows pylon 12 library defaults from the selected finder without replacing explicit library paths.
- Document explicit Release-only installed SDK configuration.
- Build the standalone device plugin against an installed consumer SDK and stage its module-owned runtime package.

- Discover Linux pylon config packages from explicit CMake paths, PYLON_ROOT, and /opt/pylon before trying the SDK find module; preserve Windows and macOS discovery.

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

- Link `Playground::DevicePlugin` for the `IDevicePlugin` MODULE.
- Own package identity in `Utility/PlaygroundAdapter/Package/Package.cmake`; the host emits `plugin.json` from `DevicePluginPackage.h`.
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
