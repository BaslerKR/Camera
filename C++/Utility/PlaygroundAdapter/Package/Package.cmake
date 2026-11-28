# Device-owned Playground plugin identity.
# Allowed commands are quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments.
# Host schema, ABI, and icon names come from DeviceRuntime.h.
# Repeat NAME=value to declare multiple environment paths for one variable.
set(PLAYGROUND_PLUGIN_ID "camera")
set(PLAYGROUND_PLUGIN_VERSION "1.4.0")
# Frame context transactions and the image writer require Core 0.7.0.
set(PLAYGROUND_PLUGIN_MINIMUM_CORE_VERSION "0.7.0")
set(PLAYGROUND_PLUGIN_DISPLAY_NAME "Camera")
# Optional presentation metadata; consumers must export it to manifests/catalogs.
set(PLAYGROUND_PLUGIN_SUBTITLE "Basler pylon")
set(PLAYGROUND_PLUGIN_DESCRIPTION "Discover and configure Basler cameras, and acquire 2D images and supported 3D payloads.")
set(PLAYGROUND_PLUGIN_ADD_ACTION_TEXT "Camera")
set(PLAYGROUND_PLUGIN_SESSION_TYPE "Camera")
set(PLAYGROUND_PLUGIN_MENU_ORDER 100)
set(PLAYGROUND_PLUGIN_INSTANCING "package")
set(PLAYGROUND_PLUGIN_SESSION_SERVICES "graphicsEngine" "scriptEditor" "threeD")
set(PLAYGROUND_PLUGIN_MODULE_WINDOWS "CameraPlugin.dll")
set(PLAYGROUND_PLUGIN_MODULE_LINUX "CameraPlugin.so")
set(PLAYGROUND_PLUGIN_MODULE_MACOS "CameraPlugin.so")
set(PLAYGROUND_PLUGIN_LIBRARY_DIRECTORIES
    "runtime"
    "runtime/DataProcessingPluginsB"
    "runtime/pylonDataProcessingPlugins"
    "runtime/stereo-mini")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS
    "GENICAM_GENTL64_PATH=runtime"
    "GENICAM_GENTL64_PATH=runtime/stereo-mini")

set(PLAYGROUND_PLUGIN_PANEL_SHOW_CONNECTION_ROOT ON)
