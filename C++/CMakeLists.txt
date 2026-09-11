cmake_minimum_required(VERSION 3.16)
project(Camera LANGUAGES CXX)
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
option(CAMERA_BUILD_QT_WIDGET
    "Build the optional Camera Qt widget target."
    OFF
)
set(HEADERS
    Camera.h
    CameraGrabDiagnostics.h
    CameraSystem.h
    PylonScene3DProfile.h
)
set(SOURCES
    Camera.cpp
    CameraSystem.cpp
)

set(QT_HEADERS
    Utility/Qt/QCameraWidget.h
)
set(QT_SOURCES
    Utility/Qt/QCameraWidget.cpp
)

add_library(${PROJECT_NAME} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(CAMERA_BUILD_QT_WIDGET)
    if(TARGET Qt6::Core)
        set(_camera_qt_package Qt6)
    elseif(TARGET Qt5::Core)
        set(_camera_qt_package Qt5)
    else()
        find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
        set(_camera_qt_package Qt${QT_VERSION_MAJOR})
    endif()

    find_package(${_camera_qt_package} REQUIRED COMPONENTS Core Gui Widgets)

    add_library(CameraQtWidget STATIC ${QT_HEADERS} ${QT_SOURCES})
    add_library(Camera::QtWidget ALIAS CameraQtWidget)
    set_target_properties(CameraQtWidget PROPERTIES AUTOMOC ON)
    target_compile_features(CameraQtWidget PUBLIC cxx_std_17)
    target_include_directories(CameraQtWidget PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(CameraQtWidget
        PUBLIC
            Camera
            ${_camera_qt_package}::Core
            ${_camera_qt_package}::Gui
            ${_camera_qt_package}::Widgets
    )
    message(STATUS "[${PROJECT_NAME}] Camera::QtWidget enabled with ${_camera_qt_package}.")
else()
    message(STATUS "[${PROJECT_NAME}] Camera::QtWidget disabled.")
endif()


# pylon Loading
# Locate the cmake find module for pylon ...

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
if(NOT pylon_FOUND)
    if(APPLE)
        find_path(pylon_cmake
            NAMES pylon-targets.cmake
            HINTS "${pylon_DIR}"
                  "$ENV{PYLON_ROOT}/Resources/CMake"
                  "/Library/Frameworks/pylon.framework/Resources/CMake"
            REQUIRED
        )
        file(REAL_PATH "${pylon_cmake}" pylon_cmake_real)
        include("${pylon_cmake_real}/pylon-config-version.cmake")
        include("${pylon_cmake_real}/genicam-targets.cmake")
        include("${pylon_cmake_real}/pylon-targets.cmake")
        set(pylon_FOUND TRUE)
        set(pylon_VERSION "${PACKAGE_VERSION}")
    else()
        find_path(pylon_cmake Findpylon.cmake
            HINTS "$ENV{PYLON_DEV_DIR}/lib/cmake"
            "$ENV{PYLON_ROOT}/lib/cmake"
            "$ENV{PYLON_ROOT}/Development/lib/cmake"
            "$ENV{ProgramFiles}/Basler/pylon 8/Development/lib/cmake"
            "$ENV{ProgramFiles}/Basler/pylon 7/Development/lib/cmake"
            "/opt/pylon/lib/cmake"
        )
        list(APPEND CMAKE_MODULE_PATH ${pylon_cmake})
        find_package(pylon REQUIRED)
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
    endif()
endif()

if(CAMERA_BUILD_PLAYGROUND_PLUGIN)
    if(NOT CAMERA_BUILD_PLAYGROUND_ADAPTER)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "CAMERA_BUILD_PLAYGROUND_ADAPTER."
        )
    endif()
    if(NOT CAMERA_BUILD_QT_WIDGET)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "CAMERA_BUILD_QT_WIDGET."
        )
    endif()
    if(NOT TARGET Playground::DevicePlugin)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires "
            "Playground::DevicePlugin."
        )
    endif()
    if(NOT TARGET Qt6::Widgets)
        message(FATAL_ERROR
            "[Camera] CAMERA_BUILD_PLAYGROUND_PLUGIN=ON requires Qt 6."
        )
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)

    add_library(CameraPlugin MODULE
        Utility/PlaygroundAdapter/Plugin/CameraPlugin.cpp
        Utility/PlaygroundAdapter/Source/CameraSourceController.h
        Utility/PlaygroundAdapter/Source/CameraSourceController.cpp
    )
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
        Playground::DevicePlugin
        Camera::QtWidget
        Camera::PlaygroundAdapter
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Concurrent
    )
    include("${CMAKE_CURRENT_SOURCE_DIR}/Utility/PlaygroundAdapter/Package/RuntimeStaging.cmake")
    camera_prepare_playground_plugin_runtime(CameraPlugin)
    message(STATUS "[Camera] CameraPlugin enabled.")
endif()
