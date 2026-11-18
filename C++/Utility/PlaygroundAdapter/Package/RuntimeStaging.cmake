# Camera-owned Playground plugin runtime payload. The host copies
# PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR into plugins/camera/current/runtime.

function(camera_prepare_playground_plugin_runtime target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "[Camera] ${target_name} must exist before its plugin runtime is prepared.")
    endif()

    set(search_paths)
    set(host_bundle_files)
    set(payload_dir "")

    if(WIN32)
        set(PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR
            "C:/Program Files/Basler/pylon/Runtime/x64" CACHE PATH
            "Directory containing local Basler Pylon runtime DLLs and CTI files for Windows bundling."
        )
        if(NOT IS_DIRECTORY "${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}")
            message(FATAL_ERROR
                "[Camera] Required local Pylon runtime directory not found at "
                "'${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}'. "
                "Set PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR.")
        endif()
        list(APPEND search_paths "${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}")
        set(payload_dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
        file(REMOVE_RECURSE "${payload_dir}")
        file(MAKE_DIRECTORY "${payload_dir}")
        # CXP is owned by FramegrabberPlugin. Exclude pylonCXP so Camera cannot
        # load a second CXP GenTL producer for the same board. Qt is Core-owned:
        # a private Qt in the package can shadow the worker's Qt and break the
        # plugin ABI. The pylon DataProcessing GUI and VTool editor DLLs are
        # standalone Qt Widgets design tools, not part of the widget-free capture
        # runtime; no staged capture payload imports them. Their OpenGL helper
        # glew32.dll imports opengl32.dll and is excluded for the same reason.
        file(COPY "${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}/"
            DESTINATION "${payload_dir}"
            PATTERN "pylonCXP" EXCLUDE
            PATTERN "Qt5*" EXCLUDE
            PATTERN "Qt6*" EXCLUDE
            PATTERN "qt*" EXCLUDE
            PATTERN "*DataProcessingGui*" EXCLUDE
            PATTERN "*Editor*" EXCLUDE
            PATTERN "*glew*" EXCLUDE
        )
        set_target_properties(${target_name} PROPERTIES
            PLAYGROUND_PLUGIN_RUNTIME_CANONICAL_SDK_DIR
                "${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}"
        )
    endif()

    if(UNIX AND NOT APPLE)
        set(linux_pylon_lib_dir "")
        foreach(candidate
                "/opt/pylon/lib" "/opt/pylon/lib64" "/opt/pylon5/lib" "/opt/pylon5/lib64")
            if(IS_DIRECTORY "${candidate}")
                set(linux_pylon_lib_dir "${candidate}")
                break()
            endif()
        endforeach()
        if(linux_pylon_lib_dir)
            list(APPEND search_paths "${linux_pylon_lib_dir}")
            file(GLOB pylon_so_files "${linux_pylon_lib_dir}/*.so*")
            list(APPEND host_bundle_files ${pylon_so_files})
        else()
            message(WARNING
                "[Camera] Basler Pylon library directory not found on Linux. "
                "Standalone bundling might miss pylon dependencies.")
        endif()
    endif()

    if(APPLE)
        # The same framework that supplied the headers and CMake targets at
        # configure time supplies the staged runtime. There is no second search.
        if("${camera_pylon_macos_framework}" STREQUAL ""
           OR NOT IS_DIRECTORY "${camera_pylon_macos_framework}")
            message(FATAL_ERROR
                "[Camera] CameraPlugin requires the macOS pylon SDK framework selected "
                "at configure time. Set PYLON_ROOT or pylon_DIR to a complete framework "
                "with Headers/ and pylon-targets.cmake, or install the SDK in "
                "/Library/Frameworks.")
        endif()
        set(pylon_framework "${camera_pylon_macos_framework}")
        get_filename_component(pylon_framework_parent "${pylon_framework}" DIRECTORY)
        list(APPEND search_paths "${pylon_framework_parent}")
        set(payload_dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
        file(REMOVE_RECURSE "${payload_dir}")
        file(MAKE_DIRECTORY "${payload_dir}")
        # The host rejects archive symlinks during plugin installation. Use
        # macOS cp's dereference mode while staging the framework payload.
        execute_process(
            COMMAND /bin/cp -R -L "${pylon_framework}" "${payload_dir}"
            RESULT_VARIABLE copy_result)
        if(copy_result)
            message(FATAL_ERROR
                "[Camera] Failed to stage pylon.framework (cp exit ${copy_result}).")
        endif()
    endif()

    set_target_properties(${target_name} PROPERTIES
        PLAYGROUND_PLUGIN_RUNTIME_DEPENDENCY_DEST "runtime"
        PLAYGROUND_PLUGIN_RUNTIME_SEARCH_PATHS "${search_paths}"
        PLAYGROUND_PLUGIN_HOST_BUNDLE_FILES "${host_bundle_files}"
    )
    if(payload_dir)
        set_target_properties(${target_name} PROPERTIES
            PLAYGROUND_PLUGIN_RUNTIME_PAYLOAD_DIR "${payload_dir}"
        )
    endif()
    if(payload_dir)
        # pylon's processing and stereo components are optional Windows installer
        # features, and the macOS framework payload has none of them. The host
        # rejects a declared directory that is missing, so declare search paths
        # only for components actually included in this payload.
        get_target_property(package_file ${target_name} PLAYGROUND_PLUGIN_PACKAGE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${package_file}")
        file(READ "${package_file}" package_content)
        set(library_directories "runtime")
        set(environment_paths "GENICAM_GENTL64_PATH=runtime")
        foreach(component DataProcessingPluginsB pylonDataProcessingPlugins stereo-mini)
            if(IS_DIRECTORY "${payload_dir}/${component}")
                list(APPEND library_directories "runtime/${component}")
                if(component STREQUAL "stereo-mini")
                    list(APPEND environment_paths "GENICAM_GENTL64_PATH=runtime/stereo-mini")
                endif()
            endif()
        endforeach()
        string(APPEND package_content
            "\nset(PLAYGROUND_PLUGIN_LIBRARY_DIRECTORIES \"${library_directories}\")\n"
            "set(PLAYGROUND_PLUGIN_ENVIRONMENT_PATHS \"${environment_paths}\")\n")
        set(resolved_package "${CMAKE_CURRENT_BINARY_DIR}/camera-package/Package.cmake")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/camera-package")
        file(WRITE "${resolved_package}" "${package_content}")
        set_property(TARGET ${target_name} PROPERTY PLAYGROUND_PLUGIN_PACKAGE "${resolved_package}")
    endif()
endfunction()
