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
        # load a second CXP GenTL producer for the same board.
        file(COPY "${PLAYGROUND_LOCAL_PYLON_RUNTIME_DIR}/"
            DESTINATION "${payload_dir}"
            PATTERN "pylonCXP" EXCLUDE
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
        set(pylon_framework "")
        foreach(pylon_target pylon::PylonBase pylon::PylonUtility)
            if(NOT TARGET ${pylon_target})
                continue()
            endif()
            foreach(pylon_location_property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION)
                get_target_property(pylon_location ${pylon_target} ${pylon_location_property})
                if(pylon_location AND NOT pylon_location MATCHES "-NOTFOUND$")
                    string(REGEX REPLACE "^(.*\\.framework)/.*$" "\\1"
                        pylon_candidate_framework "${pylon_location}")
                    if(IS_DIRECTORY "${pylon_candidate_framework}")
                        set(pylon_framework "${pylon_candidate_framework}")
                        break()
                    endif()
                endif()
            endforeach()
            if(pylon_framework)
                break()
            endif()
        endforeach()
        if(NOT pylon_framework)
            set(pylon_framework_candidates
                "$ENV{PYLON_ROOT}/pylon.framework"
                "/Library/Frameworks/pylon.framework")
            if(DEFINED pylon_DIR AND NOT "${pylon_DIR}" STREQUAL "")
                list(PREPEND pylon_framework_candidates "${pylon_DIR}/../..")
            endif()
            foreach(pylon_candidate IN LISTS pylon_framework_candidates)
                if(IS_DIRECTORY "${pylon_candidate}")
                    set(pylon_framework "${pylon_candidate}")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT pylon_framework)
            message(FATAL_ERROR
                "[Camera] CameraPlugin requires pylon.framework for macOS bundling. "
                "Set PYLON_ROOT or install pylon.framework in /Library/Frameworks.")
        endif()
        get_filename_component(pylon_framework_parent "${pylon_framework}" DIRECTORY)
        list(APPEND search_paths "${pylon_framework_parent}")
        set(payload_dir "${CMAKE_CURRENT_BINARY_DIR}/plugin-runtime")
        file(REMOVE_RECURSE "${payload_dir}")
        file(MAKE_DIRECTORY "${payload_dir}")
        file(COPY "${pylon_framework}" DESTINATION "${payload_dir}")
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
endfunction()
