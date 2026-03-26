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

    # vTools
    find_path(pylon_vTools FindpylonDataProcessing.cmake
        HINTS "/opt/pylon/share/pylon/Samples/pylonDataProcessing/C++/cmake"
        HINTS "$ENV{PYLON_DEV_DIR}/Samples/pylonDataProcessing/C++/cmake"
    )
    list(APPEND CMAKE_MODULE_PATH ${pylon_vTools})
    find_package(pylonDataProcessing QUIET)
    if(pylonDataProcessing_FOUND)
        target_link_libraries(${PROJECT_NAME} PUBLIC pylon::DataProcessing)
        message(STATUS "[${PROJECT_NAME}] pylonDataProcessing SDK found")
    endif()

    # find_path(OpenCV_DIR OpenCVConfig.cmake
    #     HINTS "C:/dev/opencv/build"
    # )
    # find_package(OpenCV QUIET)
    # if(OpenCV_FOUND)
    #     message(STATUS "[${PROJECT_NAME}] OpenCV ${OpenCV_VERSION} found")
    #     target_link_libraries(${PROJECT_NAME} PUBLIC ${OpenCV_LIBS})
    #     target_include_directories(${PROJECT_NAME} PUBLIC ${OpenCV_INCLUDE_DIRS})
    #     target_compile_definitions(${PROJECT_NAME} PUBLIC OPENCV_ENABLED)
    # else()
    #     message(WARNING "[${PROJECT_NAME}] OpenCV not found.")
    # endif()

    # # PCL Loading
    # find_package(PCL QUIET COMPONENTS common io filters visualization)
    # if(PCL_FOUND)
    #     message(STATUS "PCL ${PCL_VERSION} found")
    #     target_link_libraries(${PROJECT_NAME} PUBLIC ${PCL_LIBRARIES})
    #     target_include_directories(${PROJECT_NAME} PUBLIC ${PCL_INCLUDE_DIRS})
    #     target_compile_definitions(${PROJECT_NAME} PUBLIC PCL_ENABLED)
    #     #VTK
    #     find_package(VTK REQUIRED)
    #     if(VTK_FOUND)
    #         message(STATUS "VTK ${VTK_VERSION} found")
    #         target_link_libraries(${PROJECT_NAME} PUBLIC ${VTK_LIBRARIES})
    #         target_include_directories(${PROJECT_NAME} PUBLIC ${VTK_INCLUDE_DIRS})
    #     endif()
    # endif()
endif()
