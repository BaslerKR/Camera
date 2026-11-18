cmake_minimum_required(VERSION 3.16)
project(Camera VERSION 1.4.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(CAMERA_BUILD_PLAYGROUND_ADAPTER
    "Build the optional Camera Playground adapter target."
    OFF
)
option(CAMERA_BUILD_PLAYGROUND_PLUGIN
    "Build the Camera Playground device plugin MODULE."
    OFF
)
option(CAMERA_BUILD_TESTS "Build focused Camera adapter tests." OFF)
if(CAMERA_BUILD_PLAYGROUND_PLUGIN)
    set(CAMERA_BUILD_PLAYGROUND_ADAPTER ON)
endif()
if(PROJECT_NAME STREQUAL "Camera" AND PROJECT_IS_TOP_LEVEL AND
   (CAMERA_BUILD_PLAYGROUND_ADAPTER OR CAMERA_BUILD_PLAYGROUND_PLUGIN))
    find_package(PlaygroundPluginSDK 0.3 CONFIG REQUIRED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(HEADERS
    Camera.h
    CameraDiscoveryDiagnostics.h
    CameraGrabDiagnostics.h
    CameraSystem.h
    PylonScene3DProfile.h
)
set(SOURCES
    Camera.cpp
    CameraSystem.cpp
)

add_library(${PROJECT_NAME} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()


# pylon Loading
# Locate the cmake find module for pylon ...

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

# A macOS pylon SDK is one framework tree that carries both the public headers
# and pylon's CMake targets. Mixing headers from one install with a runtime from
# another is not ABI-safe, so an explicit hint must resolve to such a tree or
# configuration stops with the rejected path. Nothing silently falls back.
function(camera_pylon_framework_is_complete framework out_result)
    set(complete FALSE)
    if(EXISTS "${framework}/Headers/PylonIncludes.h")
        if(EXISTS "${framework}/Resources/CMake/pylon-targets.cmake"
           OR EXISTS "${framework}/Versions/A/Resources/CMake/pylon-targets.cmake")
            set(complete TRUE)
        endif()
    endif()
    set(${out_result} ${complete} PARENT_SCOPE)
endfunction()

function(camera_pylon_bundle_root path out_root)
    get_filename_component(absolute "${path}" ABSOLUTE)
    if(absolute MATCHES "^(.*\\.framework)(/.*)?$")
        set(${out_root} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_root} "" PARENT_SCOPE)
    endif()
endfunction()

function(camera_pylon_framework_from_hint hint out_framework)
    get_filename_component(start "${hint}" ABSOLUTE)
    camera_pylon_bundle_root("${start}" bundle)
    if(NOT "${bundle}" STREQUAL "")
        camera_pylon_framework_is_complete("${bundle}" complete)
        if(complete)
            set(${out_framework} "${bundle}" PARENT_SCOPE)
        else()
            set(${out_framework} "" PARENT_SCOPE)
        endif()
        return()
    endif()
    set(candidate "${start}")
    foreach(step RANGE 0 5)
        camera_pylon_framework_is_complete("${candidate}" complete)
        if(complete)
            set(${out_framework} "${candidate}" PARENT_SCOPE)
            return()
        endif()
        camera_pylon_framework_is_complete("${candidate}/pylon.framework" complete)
        if(complete)
            set(${out_framework} "${candidate}/pylon.framework" PARENT_SCOPE)
            return()
        endif()
        get_filename_component(parent "${candidate}" DIRECTORY)
        if(parent STREQUAL candidate)
            break()
        endif()
        set(candidate "${parent}")
    endforeach()
    set(${out_framework} "" PARENT_SCOPE)
endfunction()

function(camera_pylon_framework_from_target out_framework)
    set(framework "")
    foreach(target IN ITEMS pylon::PylonBase pylon::PylonUtility pylon::pylon)
        if(NOT TARGET ${target})
            continue()
        endif()
        foreach(property IN ITEMS IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION)
            get_target_property(location ${target} ${property})
            if(location AND NOT location MATCHES "-NOTFOUND$")
                camera_pylon_bundle_root("${location}" bundle)
                if(NOT "${bundle}" STREQUAL "")
                    set(framework "${bundle}")
                    break()
                endif()
            endif()
        endforeach()
        if(NOT "${framework}" STREQUAL "")
            break()
        endif()
    endforeach()
    set(${out_framework} "${framework}" PARENT_SCOPE)
endfunction()

function(camera_pylonbase_version framework out_version)
    set(best_version "")
    set(best_score -1)
    file(GLOB dylibs
        "${framework}/Libraries/libpylonbase.*.dylib"
        "${framework}/Versions/A/Libraries/libpylonbase.*.dylib")
    foreach(dylib IN LISTS dylibs)
        if(dylib MATCHES "libpylonbase\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.dylib$")
            set(major ${CMAKE_MATCH_1})
            set(minor ${CMAKE_MATCH_2})
            set(patch ${CMAKE_MATCH_3})
            math(EXPR score "${major} * 1000000 + ${minor} * 1000 + ${patch}")
            if(score GREATER best_score)
                set(best_score ${score})
                set(best_version "${major}.${minor}.${patch}")
            endif()
        endif()
    endforeach()
    if("${best_version}" STREQUAL "")
        set(best_version "unavailable")
    endif()
    set(${out_version} "${best_version}" PARENT_SCOPE)
endfunction()

if(APPLE)
    set(camera_pylon_hints)
    foreach(name IN ITEMS pylon_cmake pylon_ROOT pylon_DIR)
        if(DEFINED ${name} AND NOT "${${name}}" STREQUAL "")
            list(APPEND camera_pylon_hints "${${name}}")
        endif()
    endforeach()
    foreach(name IN ITEMS PYLON_ROOT PYLON_DIR)
        if(NOT "$ENV{${name}}" STREQUAL "")
            list(APPEND camera_pylon_hints "$ENV{${name}}")
        endif()
    endforeach()

    set(camera_pylon_macos_framework "")
    foreach(hint IN LISTS camera_pylon_hints)
        camera_pylon_framework_from_hint("${hint}" resolved)
        if("${resolved}" STREQUAL "")
            message(FATAL_ERROR
                "[${PROJECT_NAME}] The pylon hint '${hint}' does not resolve to a "
                "complete pylon SDK framework (Headers/PylonIncludes.h and "
                "pylon-targets.cmake are required). No other install is used.")
        endif()
        if("${camera_pylon_macos_framework}" STREQUAL "")
            set(camera_pylon_macos_framework "${resolved}")
        elseif(NOT "${resolved}" STREQUAL "${camera_pylon_macos_framework}")
            message(FATAL_ERROR
                "[${PROJECT_NAME}] Conflicting pylon hints resolve to different frameworks: "
                "'${camera_pylon_macos_framework}' and '${resolved}'.")
        endif()
    endforeach()

    if("${camera_pylon_macos_framework}" STREQUAL "")
        camera_pylon_framework_from_target(camera_pylon_macos_framework)
        if(NOT "${camera_pylon_macos_framework}" STREQUAL "")
            camera_pylon_framework_is_complete("${camera_pylon_macos_framework}" complete)
            if(NOT complete)
                message(FATAL_ERROR
                    "[${PROJECT_NAME}] The pylon target's framework "
                    "'${camera_pylon_macos_framework}' is not a complete pylon SDK "
                    "(Headers/PylonIncludes.h and pylon-targets.cmake are required).")
            endif()
        else()
            set(camera_pylon_macos_framework "/Library/Frameworks/pylon.framework")
            camera_pylon_framework_is_complete("${camera_pylon_macos_framework}" complete)
            if(NOT complete)
                message(FATAL_ERROR
                    "[${PROJECT_NAME}] The default macOS pylon SDK "
                    "'${camera_pylon_macos_framework}' is missing or incomplete "
                    "(Headers/PylonIncludes.h and pylon-targets.cmake are required). "
                    "Install the pylon SDK or set PYLON_ROOT or pylon_DIR.")
            endif()
        endif()
    endif()

    if(EXISTS "${camera_pylon_macos_framework}/Resources/CMake/pylon-targets.cmake")
        set(camera_pylon_cmake_dir "${camera_pylon_macos_framework}/Resources/CMake")
    else()
        set(camera_pylon_cmake_dir "${camera_pylon_macos_framework}/Versions/A/Resources/CMake")
    endif()
    camera_pylonbase_version("${camera_pylon_macos_framework}" camera_pylonbase_version)
    message(STATUS "[${PROJECT_NAME}] macOS pylon framework: ${camera_pylon_macos_framework}")
    message(STATUS "[${PROJECT_NAME}] macOS pylonbase version: ${camera_pylonbase_version}")
endif()

if(NOT pylon_FOUND)
    if(APPLE)
        set(pylon_cmake "${camera_pylon_cmake_dir}")
        file(REAL_PATH "${pylon_cmake}" pylon_cmake_real)
        include("${pylon_cmake_real}/pylon-config-version.cmake")
        include("${pylon_cmake_real}/genicam-targets.cmake")
        include("${pylon_cmake_real}/pylon-targets.cmake")
        set(pylon_FOUND TRUE)
        set(pylon_VERSION "${PACKAGE_VERSION}")
    else()
        # Linux pylon 8 ships a config package under share/pylon/cmake.
        # Try it before the find-module discovery used by other SDK layouts.
        if(UNIX)
            find_package(pylon CONFIG QUIET
                HINTS "$ENV{PYLON_ROOT}" "/opt/pylon"
            )
        endif()
        if(NOT pylon_FOUND)
            set(camera_windows_pylon_hints)
            if(WIN32)
                list(APPEND camera_windows_pylon_hints
                    "$ENV{ProgramFiles}/Basler/pylon/Development/lib/cmake")
            endif()
            find_path(pylon_cmake Findpylon.cmake
                HINTS ${CMAKE_MODULE_PATH} "${pylon_DIR}"
                "$ENV{PYLON_DEV_DIR}/lib/cmake"
                "$ENV{PYLON_ROOT}/lib/cmake"
                "$ENV{PYLON_ROOT}/Development/lib/cmake"
                ${camera_windows_pylon_hints}
                "$ENV{ProgramFiles}/Basler/pylon 8/Development/lib/cmake"
                "$ENV{ProgramFiles}/Basler/pylon 7/Development/lib/cmake"
                "/opt/pylon/lib/cmake"
            )
            if(pylon_cmake)
                if(WIN32)
                    # Derive defaults from the selected finder, preserving explicit
                    # library paths even when another pylon installation is present.
                    get_filename_component(camera_pylon_lib "${pylon_cmake}/../../lib/x64" ABSOLUTE)
                    if(pylon_LIBRARY_DIR AND NOT pylon_LIBRARY_DIR MATCHES "-NOTFOUND$")
                        set(camera_pylon_lib "${pylon_LIBRARY_DIR}")
                    endif()
                    if(EXISTS "${camera_pylon_lib}/PylonBase_v12.lib")
                        if(NOT pylon_LIBRARY_DIR OR pylon_LIBRARY_DIR MATCHES "-NOTFOUND$")
                            set(pylon_LIBRARY_DIR "${camera_pylon_lib}" CACHE PATH "pylon library directory" FORCE)
                        endif()
                        foreach(name IN ITEMS Base GUI Utility)
                            if(NOT pylon${name} OR pylon${name} MATCHES "-NOTFOUND$")
                                set(pylon${name} "${camera_pylon_lib}/Pylon${name}_v12.lib" CACHE FILEPATH "pylon ${name} library" FORCE)
                            endif()
                        endforeach()
                        foreach(name IN ITEMS GCBase GenApi)
                            if(NOT ${name} OR ${name} MATCHES "-NOTFOUND$")
                                set(${name} "${camera_pylon_lib}/${name}_MD_VC142_v3_5_Basler_pylon_v1.lib"
                                    CACHE FILEPATH "pylon ${name} library" FORCE)
                            endif()
                        endforeach()
                    endif()
                endif()
                list(PREPEND CMAKE_MODULE_PATH "${pylon_cmake}")
            endif()
            find_package(pylon REQUIRED)
        endif()
    endif()
endif()

if(pylon_FOUND)
    if(NOT TARGET pylon::pylon)
        message(FATAL_ERROR
            "[${PROJECT_NAME}] pylon_FOUND is true, but the pylon::pylon target was not created."
        )
    endif()

    # Keep the vendor imported target immutable. Camera owns the platform-specific
    # link policy through this interface target, including Linux loader behavior.
    add_library(CameraPylonRuntime INTERFACE)
    add_library(Camera::Pylon ALIAS CameraPylonRuntime)

    foreach(_camera_pylon_interface_property IN ITEMS
        INTERFACE_COMPILE_DEFINITIONS
        INTERFACE_COMPILE_FEATURES
        INTERFACE_COMPILE_OPTIONS
        INTERFACE_INCLUDE_DIRECTORIES
        INTERFACE_LINK_DEPENDS
        INTERFACE_LINK_DIRECTORIES
        INTERFACE_LINK_LIBRARIES
        INTERFACE_LINK_OPTIONS
        INTERFACE_PRECOMPILE_HEADERS
        INTERFACE_SOURCES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    )
        get_target_property(
            _camera_pylon_property
            pylon::pylon
            "${_camera_pylon_interface_property}"
        )
        if(NOT _camera_pylon_property STREQUAL "_camera_pylon_property-NOTFOUND")
            if(UNIX AND NOT APPLE)
                if(_camera_pylon_interface_property STREQUAL "INTERFACE_LINK_LIBRARIES"
                   OR _camera_pylon_interface_property STREQUAL "INTERFACE_LINK_OPTIONS")
                    string(REPLACE
                        "-Wl,--enable-new-dtags"
                        ""
                        _camera_pylon_property
                        "${_camera_pylon_property}"
                    )
                endif()
            endif()
            set_property(
                TARGET CameraPylonRuntime
                PROPERTY "${_camera_pylon_interface_property}"
                "${_camera_pylon_property}"
            )
        endif()
    endforeach()

    target_link_libraries(${PROJECT_NAME} PUBLIC CameraPylonRuntime)
    message(STATUS "[${PROJECT_NAME}] pylon SDK ${pylon_VERSION} found")
endif()

if(CAMERA_BUILD_PLAYGROUND_ADAPTER)
    if(NOT TARGET GraphicsEngine::Contracts)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_ADAPTER=ON requires "
            "GraphicsEngine::Contracts. Add GraphicsEngine first or disable the adapter."
        )
    endif()

    find_package(OpenMP REQUIRED COMPONENTS CXX)

    add_library(CameraPlaygroundAdapter STATIC
        Utility/PlaygroundAdapter/BlazeGraphicsFrameAdapter.h
        Utility/PlaygroundAdapter/BlazeGraphicsFrameAdapter.cpp
        Utility/PlaygroundAdapter/PylonGraphicsFrameAdapter.h
        Utility/PlaygroundAdapter/PylonGraphicsFrameAdapter.cpp
        Utility/PlaygroundAdapter/PylonGraphicsFrameStream.h
        Utility/PlaygroundAdapter/PylonGraphicsFrameStream.cpp
    )
    add_library(Camera::PlaygroundAdapter ALIAS CameraPlaygroundAdapter)
    target_compile_features(CameraPlaygroundAdapter PUBLIC cxx_std_17)
    target_include_directories(CameraPlaygroundAdapter
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter"
    )
    target_link_libraries(CameraPlaygroundAdapter
        PUBLIC
            Camera
            GraphicsEngine::Contracts
        PRIVATE
            OpenMP::OpenMP_CXX
    )
    message(STATUS "[Camera] Camera::PlaygroundAdapter enabled.")

    if(CAMERA_BUILD_TESTS)
        enable_testing()
        add_executable(camera_graphics_image_adapter_test
            Utility/PlaygroundAdapter/Tests/GraphicsFrameAdapterTest.cpp
        )
        target_link_libraries(camera_graphics_image_adapter_test
            PRIVATE
                Camera::PlaygroundAdapter
        )
        add_test(
            NAME camera_graphics_image_adapter_test
            COMMAND camera_graphics_image_adapter_test
        )

        add_executable(camera_grab_diagnostics_test
            Utility/PlaygroundAdapter/Tests/GrabDiagnosticsTest.cpp
        )
        target_link_libraries(camera_grab_diagnostics_test
            PRIVATE
                Camera::PlaygroundAdapter
        )
        add_test(
            NAME camera_grab_diagnostics_test
            COMMAND camera_grab_diagnostics_test
        )

        add_executable(camera_discovery_diagnostics_test
            Utility/PlaygroundAdapter/Tests/DiscoveryDiagnosticsTest.cpp
        )
        target_link_libraries(camera_discovery_diagnostics_test
            PRIVATE
                Camera::PlaygroundAdapter
        )
        add_test(
            NAME camera_discovery_diagnostics_test
            COMMAND camera_discovery_diagnostics_test
        )
    endif()
endif()

if(CAMERA_BUILD_PLAYGROUND_PLUGIN)
    if(NOT CAMERA_BUILD_PLAYGROUND_ADAPTER)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "CAMERA_BUILD_PLAYGROUND_ADAPTER."
        )
    endif()
    if(NOT TARGET Playground::DeviceRuntimeContract)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DeviceRuntimeContract."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)

    add_library(CameraPlugin MODULE
        Utility/PlaygroundAdapter/Plugin/CameraPlugin.cpp
        Utility/PlaygroundAdapter/Source/CameraSourceController.h
        Utility/PlaygroundAdapter/Source/CameraSourceController.cpp
        Utility/PlaygroundAdapter/Source/CameraParameterControl.cpp
    )
    playground_sdk_add_runtime_contract(CameraPlugin)
    set_target_properties(CameraPlugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME CameraPlugin
        AUTOMOC ON
        PLAYGROUND_PLUGIN_PACKAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/Package.cmake"
    )
    target_compile_features(CameraPlugin PRIVATE cxx_std_17)
    target_include_directories(CameraPlugin PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Plugin"
    )
    target_link_libraries(CameraPlugin PRIVATE
        Playground::DeviceRuntimeContract
        Camera::PlaygroundAdapter
        Qt6::Core
        Qt6::Concurrent
    )
    if(CAMERA_BUILD_TESTS)
        # The controller sources are compiled into the plugin MODULE, so the test compiles them too.
        find_package(Qt6 REQUIRED COMPONENTS Test)
        add_executable(camera_parameter_control_test
            Utility/PlaygroundAdapter/Tests/ParameterControlTest.cpp
            Utility/PlaygroundAdapter/Source/CameraSourceController.cpp
            Utility/PlaygroundAdapter/Source/CameraParameterControl.cpp
        )
        playground_sdk_add_runtime_contract(camera_parameter_control_test)
        set_target_properties(camera_parameter_control_test PROPERTIES AUTOMOC ON)
        target_compile_features(camera_parameter_control_test PRIVATE cxx_std_17)
        target_include_directories(camera_parameter_control_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Source"
        )
        target_link_libraries(camera_parameter_control_test PRIVATE
            Playground::DeviceRuntimeContract
            Camera::PlaygroundAdapter
            Qt6::Core
            Qt6::Concurrent
            Qt6::Test
        )
        # Emulator-backed cases skip when the pylon camera emulator is not installed.
        add_test(NAME camera_parameter_control_test COMMAND camera_parameter_control_test)
        set_tests_properties(camera_parameter_control_test PROPERTIES
            ENVIRONMENT "PYLON_CAMEMU=1;QT_QPA_PLATFORM=offscreen"
            TIMEOUT 300
        )
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    camera_prepare_playground_plugin_runtime(CameraPlugin)
    if(PROJECT_NAME STREQUAL "Camera" AND PROJECT_IS_TOP_LEVEL)
        if(APPLE)
            set_target_properties(CameraPlugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "@loader_path/runtime;@executable_path/../Frameworks")
        elseif(UNIX)
            set_target_properties(CameraPlugin PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                INSTALL_RPATH "$ORIGIN/runtime")
        endif()
        get_target_property(camera_package_file CameraPlugin PLAYGROUND_PLUGIN_PACKAGE)
        playground_sdk_package_plugin(CameraPlugin
            PACKAGE_FILE "${camera_package_file}"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/icon.png")
    endif()
    message(STATUS "[Camera] CameraPlugin enabled.")
endif()

# Keep contract assertions active in Release validation.
foreach(module_test IN ITEMS camera_graphics_image_adapter_test camera_grab_diagnostics_test camera_discovery_diagnostics_test)
    if(TARGET ${module_test})
        target_compile_options(${module_test} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    endif()
endforeach()
