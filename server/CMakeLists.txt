project(cxxime-server CXX)

add_executable(cxxime-server WIN32
    src/config_store.cc
    src/config_write_coordinator.cc
    src/candidate_preference_save_worker.cc
    src/diagnostic_log_maintenance_worker.cc
    src/input_method_hotkey.cc
    src/ipc_response_builder.cc
    src/main.cc
    src/server_app.cc
    src/session_manager.cc
    src/system_lifecycle_monitor.cc
    src/lexicon_control_handler.cc
    src/ui_candidate_page_conversion.cc
    src/ui_presentation_controller.cc
    src/ui_presentation_router.cc
    src/user_backup_service.cc
)

target_include_directories(cxxime-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cxxime-server PRIVATE
    cxxime-shared
    cxxime-engine
    cxxime-ipc
    cxxime-ui
    shell32
    advapi32
    ole32
    uuid
    wtsapi32
)

cxxime_add_version_resource(
    cxxime-server
    "CxxIME Server"
    "cxxime-server.exe"
    VFT_APP
)
