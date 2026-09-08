# Device-owned Playground plugin identity.
# Allowed commands are quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments.
# Host schema, ABI, and icon names come from DevicePluginPackage.h.
# Repeat NAME=value to declare multiple environment paths for one variable.
set(PLAYGROUND_PLUGIN_ID "camera")
set(PLAYGROUND_PLUGIN_VERSION "1.0.2")
set(PLAYGROUND_PLUGIN_DISPLAY_NAME "Basler Camera")
set(PLAYGROUND_PLUGIN_ADD_ACTION_TEXT "Camera")
set(PLAYGROUND_PLUGIN_SESSION_TYPE "Camera")
set(PLAYGROUND_PLUGIN_MENU_ORDER 100)
set(PLAYGROUND_PLUGIN_LOAD_ORDER 200)
set(PLAYGROUND_PLUGIN_LIBRARY_WINDOWS "CameraPlugin.dll")
set(PLAYGROUND_PLUGIN_LIBRARY_LINUX "CameraPlugin.so")
set(PLAYGROUND_PLUGIN_LIBRARY_MACOS "CameraPlugin.so")
set(PLAYGROUND_PLUGIN_LIBRARY_DIRECTORIES
    "runtime"
    "runtime/DataProcessingPluginsB"
    "runtime/pylonDataProcessingPlugins"
    "runtime/stereo-mini")
set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS
    "GENICAM_GENTL64_PATH=runtime"
    "GENICAM_GENTL64_PATH=runtime/stereo-mini")
