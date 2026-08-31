## Unreleased

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
