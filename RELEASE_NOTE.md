## Unreleased

- Split the opt-in Qt control widget and image converter into `Camera::QtWidget`; the `Camera` core target no longer discovers or publicly links Qt.
- Updated the optional scene adapter to consume a neutral scene-contract target without inheriting the visualization runtime; its conversion output is unchanged.
- Replace the corrupted host-layout README with a standalone, code-aligned acquisition and integration contract.

## v1.0.1

- Disable the camera selector when discovery returns no cameras and enable it again when a camera is available.
- Declare millimeter XYZ output explicitly for direct and reconstructed organized 3D scene adapters.
