cmake_minimum_required(VERSION 3.16)
project(Camera LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(HEADERS
    Camera.h
    CameraSystem.h
)
set(SOURCES
    Camera.cpp
    CameraSystem.cpp
)

set(QT_HEADERS
    Utility/Qt/QtConverter.h
    Utility/Qt/QCameraWidget.h
)
set(QT_SOURCES
    Utility/Qt/QCameraWidget.cpp
)

add_library(${PROJECT_NAME} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(QT NAMES Qt6 Qt5 QUIET)
if(QT_FOUND)
    find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS Core Gui Widgets Xml)
    set(CMAKE_AUTOMOC ON)
    message(STATUS "[${PROJECT_NAME}] Configuring for Qt ${QT_VERSION_MAJOR}")
    target_sources(${PROJECT_NAME} PRIVATE ${QT_HEADERS} ${QT_SOURCES})
    target_link_libraries(${PROJECT_NAME} PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
    )
    message(STATUS "[${PROJECT_NAME}] QtConverter.h features will be enabled for users")
else()
    message(STATUS "[${PROJECT_NAME}] Qt Not Found")
    message(STATUS "[${PROJECT_NAME}] QtConverter.h features will be disabled")
endif()


# pylon Loading
# Locate the cmake find module for pylon ...

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
if(NOT pylon_FOUND)
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
    if(pylon_FOUND)
        target_link_libraries(${PROJECT_NAME} PUBLIC pylon::pylon)
        message(STATUS "[${PROJECT_NAME}] pylon SDK ${pylon_VERSION} found")
    endif()
endif()
