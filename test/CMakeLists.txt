project(cxxime-test CXX)

set(TEST_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/tsf/src)

add_library(cxxime-test-util STATIC support/testutil.cc)
target_include_directories(cxxime-test-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(cxxime-test-topn-data STATIC support/topn_test_data.cc)
target_include_directories(cxxime-test-topn-data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cxxime-test-topn-data PUBLIC
    cxxime-engine
    cxxime-topn-index-builder
)

add_library(cxxime-test-wubi-index-data STATIC support/wubi_index_test_data.cc)
target_include_directories(cxxime-test-wubi-index-data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(cxxime-test-wubi-index-data PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
)

function(add_cxxime_test name)
    cmake_parse_arguments(TEST "" "" "SOURCES;DEPS" ${ARGN})
    if(NOT TEST_SOURCES)
        message(FATAL_ERROR "add_cxxime_test(${name}) requires SOURCES")
    endif()
    if(TEST_UNPARSED_ARGUMENTS OR TEST_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Invalid arguments for add_cxxime_test(${name})")
    endif()

    add_executable(${name} ${TEST_SOURCES})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDE_DIRS})
    # Tests always use the project's data/ directory regardless of CXXIME_PRODUCTION_BUILD
    target_compile_definitions(${name} PRIVATE
        CXXIME_PROJECT_DIR="${CMAKE_SOURCE_DIR}/"
        CXXIME_DATA_DIR="${CMAKE_SOURCE_DIR}/data/"
    )
    target_link_libraries(${name} PRIVATE cxxime-test-util ${TEST_DEPS})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_cxxime_test(engine_test
    SOURCES
        engine/engine_test.cc
        engine/engine_pinyin_test.cc
        engine/engine_ascii_mode_test.cc
        engine/engine_ascii_composition_test.cc
        engine/pinyin_runtime_test.cc
        engine/pinyin_scheme_test.cc
        engine/pinyin_user_code_test.cc
        engine/engine_deadline_test.cc
        engine/composition_state_test.cc
        engine/user_data_merge_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
add_cxxime_test(segmented_selection_test
    SOURCES engine/segmented_selection_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
add_cxxime_test(composition_learning_test
    SOURCES engine/composition_learning_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
add_cxxime_test(segmentor_test SOURCES engine/segmentor_test.cc DEPS cxxime-engine)
add_cxxime_test(syllabifier_completion_test
    SOURCES engine/syllabifier_completion_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(pinyin_composer_test SOURCES engine/pinyin_composer_test.cc DEPS cxxime-engine)
add_cxxime_test(dict_test SOURCES engine/dict_test.cc DEPS cxxime-engine)
add_cxxime_test(user_data_separation_test
    SOURCES engine/user_data_separation_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(ordinary_candidate_test
    SOURCES engine/ordinary_candidate_test.cc
    DEPS cxxime-engine nlohmann_json
)
add_cxxime_test(disabled_system_lexicon_test
    SOURCES engine/disabled_system_lexicon_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
add_cxxime_test(dictionary_format_test
    SOURCES engine/dictionary_format_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
add_cxxime_test(dictionary_manifest_test
    SOURCES shared/dictionary_manifest_test.cc
    DEPS cxxime-shared
)
add_cxxime_test(user_backup_test SOURCES shared/user_backup_test.cc DEPS cxxime-shared)
target_link_libraries(user_backup_test PRIVATE miniz)
add_cxxime_test(config_test
    SOURCES engine/config_test.cc
    DEPS cxxime-engine cxxime-ipc cxxime-ui
)
add_cxxime_test(keyboard_shortcut_test
    SOURCES shared/keyboard_shortcut_test.cc
    DEPS cxxime-shared
)
add_cxxime_test(layout_test SOURCES ui/layout_test.cc DEPS cxxime-ui)
add_cxxime_test(preedit_mode_test SOURCES tsf/preedit_mode_test.cc)
add_cxxime_test(preedit_edit_test SOURCES engine/preedit_edit_test.cc DEPS cxxime-engine)
add_cxxime_test(edit_target_test SOURCES tsf/edit_target_test.cc DEPS cxxime-shared)
target_sources(edit_target_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tsf/src/edit_target.cc
    ${CMAKE_SOURCE_DIR}/tsf/src/globals.cpp
)
target_link_libraries(edit_target_test PRIVATE ole32 oleaut32 user32)
add_cxxime_test(text_service_test
    SOURCES
        tsf/text_service_composition_test.cc
        tsf/text_service_key_test.cc
        tsf/text_service_presentation_test.cc
)
# Exercise the real service and edit-session implementation; only the host COM
# interfaces are controlled by the test. Keep its sources/dependencies aligned
# with the DLL without importing its entry point, exports or version resource.
get_target_property(TSF_TEST_SOURCES cxxime-tsf SOURCES)
foreach(TSF_TEST_SOURCE IN LISTS TSF_TEST_SOURCES)
    if(TSF_TEST_SOURCE MATCHES "\\.(cpp|cc)$" AND
       NOT TSF_TEST_SOURCE MATCHES "[/\\\\]dllmain\\.cpp$")
        if(NOT IS_ABSOLUTE "${TSF_TEST_SOURCE}")
            set(TSF_TEST_SOURCE "${CMAKE_SOURCE_DIR}/tsf/${TSF_TEST_SOURCE}")
        endif()
        target_sources(text_service_test PRIVATE "${TSF_TEST_SOURCE}")
    endif()
endforeach()
get_target_property(TSF_TEST_INCLUDE_DIRS cxxime-tsf INCLUDE_DIRECTORIES)
get_target_property(TSF_TEST_LIBRARIES cxxime-tsf LINK_LIBRARIES)
target_include_directories(text_service_test PRIVATE ${TSF_TEST_INCLUDE_DIRS})
target_link_libraries(text_service_test PRIVATE ${TSF_TEST_LIBRARIES})
add_cxxime_test(effective_edit_target_test SOURCES tsf/effective_edit_target_test.cc)
target_sources(effective_edit_target_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tsf/src/effective_edit_target.cpp
)
add_cxxime_test(candidate_presentation_test
    SOURCES tsf/candidate_presentation_test.cc
    DEPS cxxime-shared
)
target_sources(candidate_presentation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tsf/src/candidate_presentation.cpp
)
add_cxxime_test(engine_response_test
    SOURCES tsf/engine_response_test.cc
    DEPS cxxime-shared
)
target_sources(engine_response_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tsf/src/engine_response.cpp
)
target_include_directories(engine_response_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tsf/src
)
add_cxxime_test(ipc_test
    SOURCES
        ipc/ipc_test.cc
        ipc/ipc_compatibility_test.cc
    DEPS cxxime-ipc
)
add_cxxime_test(wubi_test SOURCES engine/wubi_test.cc DEPS cxxime-engine)
add_cxxime_test(wubi_prefix_query_test
    SOURCES engine/wubi_prefix_query_test.cc
    DEPS cxxime-engine cxxime-test-wubi-index-data
)
add_cxxime_test(candidate_window_test
    SOURCES ui/candidate_window_test.cc
    DEPS cxxime-engine cxxime-ui
)
add_cxxime_test(candidate_quality_test
    SOURCES engine/candidate_quality_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(status_window_test SOURCES ui/status_window_test.cc DEPS cxxime-ui)
set_tests_properties(status_window_test PROPERTIES RUN_SERIAL TRUE)
add_cxxime_test(window_position_test SOURCES ui/window_position_test.cc DEPS cxxime-ui)
add_cxxime_test(benchmark_test SOURCES engine/benchmark_test.cc DEPS cxxime-engine)
add_cxxime_test(short_cache_test
    SOURCES engine/short_cache_test.cc
    DEPS cxxime-engine cxxime-test-topn-data
)
target_include_directories(short_cache_test PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
add_cxxime_test(trace_test SOURCES shared/trace_test.cc DEPS cxxime-engine)
add_cxxime_test(diagnostic_log_maintenance_test
    SOURCES shared/diagnostic_log_maintenance_test.cc
    DEPS cxxime-shared
)
if(CXXIME_ENABLE_HOST_DIAGNOSTICS)
    add_cxxime_test(host_trace_test SOURCES shared/host_trace_test.cc DEPS cxxime-shared)
endif()
add_cxxime_test(mpscq_test SOURCES shared/mpscq_test.cc DEPS cxxime-shared)
add_cxxime_test(control_channel_test SOURCES ipc/control_channel_test.cc DEPS cxxime-ipc)
add_cxxime_test(control_lifecycle_test
    SOURCES ipc/control_lifecycle_test.cc
    DEPS cxxime-ipc
)
add_cxxime_test(ui_channel_test SOURCES ipc/ui_channel_test.cc DEPS cxxime-ipc)
add_cxxime_test(ui_presentation_router_test
    SOURCES server/ui_presentation_router_test.cc
    DEPS cxxime-ipc
)
add_cxxime_test(ui_candidate_page_conversion_test
    SOURCES
        server/ui_candidate_page_conversion_test.cc
        ${CMAKE_SOURCE_DIR}/server/src/ui_candidate_page_conversion.cc
    DEPS cxxime-ipc
)
target_include_directories(ui_candidate_page_conversion_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)
target_sources(ui_presentation_router_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src/ui_presentation_router.cc
)
target_include_directories(ui_presentation_router_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)
add_cxxime_test(ui_presentation_controller_test
    SOURCES
        server/ui_presentation_controller_test.cc
        ${CMAKE_SOURCE_DIR}/server/src/ui_presentation_controller.cc
        ${CMAKE_SOURCE_DIR}/server/src/ui_candidate_page_conversion.cc
    DEPS cxxime-engine cxxime-ui cxxime-ipc ole32
)
target_include_directories(ui_presentation_controller_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)
set_tests_properties(ui_presentation_controller_test PROPERTIES RUN_SERIAL TRUE)
add_cxxime_test(system_lifecycle_monitor_test
    SOURCES server/system_lifecycle_monitor_test.cc
    DEPS cxxime-shared
)
target_sources(system_lifecycle_monitor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src/system_lifecycle_monitor.cc
)
target_include_directories(system_lifecycle_monitor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)
target_link_libraries(system_lifecycle_monitor_test PRIVATE
    wtsapi32
)

add_cxxime_test(config_write_coordinator_test
    SOURCES
        server/config_write_coordinator_test.cc
        ${CMAKE_SOURCE_DIR}/server/src/config_store.cc
        ${CMAKE_SOURCE_DIR}/server/src/config_write_coordinator.cc
    DEPS cxxime-engine cxxime-ipc
)
target_include_directories(config_write_coordinator_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)

add_cxxime_test(dictionary_monitor_test
    SOURCES shared/dictionary_monitor_test.cc
    DEPS cxxime-shared
)
add_cxxime_test(output_composer_test
    SOURCES engine/output_composer_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(engine_source_test SOURCES engine/engine_source_test.cc DEPS cxxime-engine)
add_cxxime_test(input_style_test SOURCES engine/input_style_test.cc DEPS cxxime-engine)
add_cxxime_test(symbol_input_test SOURCES engine/symbol_input_test.cc DEPS cxxime-engine)
add_cxxime_test(symbol_table_test SOURCES engine/symbol_table_test.cc DEPS cxxime-engine)
add_cxxime_test(wubi_engine_test
    SOURCES
        engine/wubi_engine_test.cc
        engine/wubi_engine_input_test.cc
    DEPS cxxime-engine cxxime-test-wubi-index-data
)
add_cxxime_test(processor_pagination_test
    SOURCES engine/processor_pagination_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(temporary_ascii_test
    SOURCES engine/temporary_ascii_test.cc
    DEPS cxxime-engine
)
add_cxxime_test(tsf_factory_test
    SOURCES tsf/tsf_factory_test.cc
    DEPS cxxime-shared ole32 uuid
)

add_cxxime_test(settings_launcher_util_test
    SOURCES shared/settings_launcher_test.cc
    DEPS cxxime-shared
)
target_include_directories(settings_launcher_util_test PRIVATE
    ${CMAKE_SOURCE_DIR}/shared/src
)

add_cxxime_test(installer_lock_test
    SOURCES installer/installer_lock_test.cc
    DEPS cxxime-installer-lock
)
add_cxxime_test(installer_lifecycle_test
    SOURCES installer/installer_lifecycle_test.cc
    DEPS cxxime-installer-lock
)
add_cxxime_test(installer_version_test
    SOURCES installer/installer_version_test.cc
    DEPS cxxime-installer-lock
)

add_cxxime_test(candidate_preference_save_worker_test
    SOURCES
        server/candidate_preference_save_worker_test.cc
        ${CMAKE_SOURCE_DIR}/server/src/candidate_preference_save_worker.cc
    DEPS cxxime-engine
)
target_include_directories(candidate_preference_save_worker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)

add_cxxime_test(session_manager_status_test
    SOURCES
        server/session_manager_status_test.cc
        ${CMAKE_SOURCE_DIR}/server/src/session_manager.cc
    DEPS
        cxxime-engine
        cxxime-ipc
        cxxime-test-topn-data
        cxxime-test-wubi-index-data
)
target_include_directories(session_manager_status_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)

add_cxxime_test(session_manager_integration_test
    SOURCES
        server/session_manager_integration_test.cc
        server/session_manager_candidate_test.cc
        server/ipc_response_builder_test.cc
        server/session_manager_segmented_test.cc
        server/session_manager_lexicon_test.cc
        server/session_manager_lifecycle_test.cc
        server/user_backup_service_test.cc
        server/session_manager_integration_test_support.cc
        ${CMAKE_SOURCE_DIR}/server/src/session_manager.cc
        ${CMAKE_SOURCE_DIR}/server/src/ipc_response_builder.cc
        ${CMAKE_SOURCE_DIR}/server/src/lexicon_control_handler.cc
        ${CMAKE_SOURCE_DIR}/server/src/config_store.cc
        ${CMAKE_SOURCE_DIR}/server/src/config_write_coordinator.cc
        ${CMAKE_SOURCE_DIR}/server/src/user_backup_service.cc
    DEPS
        cxxime-engine
        cxxime-ipc
        cxxime-test-topn-data
        cxxime-test-wubi-index-data
)
target_include_directories(session_manager_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/server/src
)

find_package(Python3 QUIET)
if(Python3_FOUND)
    add_test(
        NAME install_payload_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/installer/install_payload_test.py
    )
    add_test(
        NAME installer_flow_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/installer/installer_flow_test.py
    )
    add_test(
        NAME dictionary_format_verifier_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/dictionary_format_verifier_test.py
    )
    add_test(
        NAME reverse_index_pipeline_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/reverse_index_pipeline_test.py
    )
    add_test(
        NAME dictionary_symbol_policy_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/dictionary_symbol_policy_test.py
    )
    add_test(
        NAME symbol_catalog_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/symbol_catalog_test.py
    )
    add_test(
        NAME unicode_classification_generator_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode_classification_generator_test.py
    )
    add_test(
        NAME wubi_prefix_index_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/data/wubi_prefix_index_test.py
    )
    add_test(
        NAME wubi_symbol_ranking_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/wubi_symbol_ranking_test.py
    )
    add_test(
        NAME pinyin_topn_pipeline_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/data/pinyin_topn_pipeline_test.py
                --topn-builder $<TARGET_FILE:topn_builder>
    )
    find_program(POWERSHELL_EXECUTABLE powershell)
    if(POWERSHELL_EXECUTABLE)
        add_test(
            NAME host_trace_tools_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/host_trace_tools_test.py
                    --powershell ${POWERSHELL_EXECUTABLE}
        )
    endif()
endif()
