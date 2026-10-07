project(cxxime-shared CXX)

if(CXXIME_ENABLE_HOST_DIAGNOSTICS)
    set(CXXIME_SHARED_DIAGNOSTIC_SOURCES
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/host_trace.cc
    )
else()
    set(CXXIME_SHARED_DIAGNOSTIC_SOURCES
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/stubs/host_trace.cc
    )
endif()

add_library(cxxime-shared STATIC
    src/key_event.cc
    src/keyboard_shortcut.cc
    src/data_path.cc
    src/diagnostic_log_maintenance.cc
    src/diagnostic_log_path.cc
    src/dictionary_manifest.cc
    src/dictionary_monitor.cc
    src/diagnostics_config.cc
    src/mpscq.cc
    src/ordinary_candidate.cc
    src/query_trace_log.cc
    src/server_launcher.cc
    src/settings_launcher.cc
    src/settings_launcher_util.cc
    src/tsf_factory.cc
    src/ui_presentation_trace.cc
    src/user_backup.cc
    src/user_dict_validation.cc
    ${CXXIME_SHARED_DIAGNOSTIC_SOURCES}
)

target_include_directories(cxxime-shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/include
)
target_include_directories(cxxime-shared PRIVATE
    ${CXXIME_GENERATED_INCLUDE_DIR}
)

target_link_libraries(cxxime-shared PUBLIC shell32 bcrypt advapi32 user32)
if(CXXIME_ENABLE_HOST_DIAGNOSTICS)
    target_compile_definitions(cxxime-shared PRIVATE CXXIME_ENABLE_HOST_DIAGNOSTICS=1)
    target_link_libraries(cxxime-shared PUBLIC dwmapi)
endif()
target_link_libraries(cxxime-shared PUBLIC nlohmann_json)
target_link_libraries(cxxime-shared PRIVATE miniz)
