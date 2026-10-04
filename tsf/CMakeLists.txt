project(cxxime-tsf CXX)

if(CXXIME_ENABLE_HOST_DIAGNOSTICS)
    set(CXXIME_TSF_DIAGNOSTIC_SOURCES
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_composition.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_activation.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_imm_mode.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_trace.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_context.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_window.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_imm_candidate_snapshot.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_imm_state.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_message.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_callsite.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_classification.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_sdl_message_hook.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_sdl_runtime.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_classification_message.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_host_ime_private_api.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_ui_element_identity.cc
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/tsf_ui_element_observer.cc
    )
else()
    set(CXXIME_TSF_DIAGNOSTIC_SOURCES
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/stubs/tsf.cc
    )
endif()

add_library(cxxime-tsf SHARED
    src/config_coordinator.cpp
    src/dllmain.cpp
    src/globals.cpp
    src/class_factory.cpp
    src/candidate_presentation.cpp
    src/effective_edit_target.cpp
    src/engine_response.cpp
    src/edit_target.cc
    src/text_service.cpp
    src/text_service_focus.cpp
    src/text_service_key.cpp
    src/text_service_response.cpp
    src/text_service_session.cpp
    src/text_service_context.cpp
    src/text_service_edit.cpp
    src/text_service_layout.cpp
    src/text_service_candidate.cpp
    src/text_service_composition.cpp
    src/text_service_compartment.cpp
    src/text_service_config.cpp
    src/text_service_activation.cpp
    src/text_service_trace.cpp
    src/ui_presentation_batch.cpp
    src/ui_presentation_bridge.cpp
    src/tsf_log_writer.cpp
    src/key_event_sink.cpp
    src/edit_session.cpp
    src/candidate_ui_element.cpp
    src/search_candidate_list.cpp
    src/reading_ui_element.cpp
    src/display_attribute.cpp
    src/register.cpp
    src/resource_loader.cc
    src/language_bar.cpp
    src/input_indicator_controller.cpp
    src/host_compatibility/host_classification_compatibility.cc
    src/host_compatibility/host_classification_runtime.cc
    src/host_compatibility/host_ime_private_api.cc
    ${CXXIME_TSF_DIAGNOSTIC_SOURCES}
    cxxime_tsf.def
)

target_include_directories(cxxime-tsf PRIVATE
    ${CXXIME_GENERATED_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/resource
    ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src
)

target_link_libraries(cxxime-tsf PRIVATE
    cxxime-shared
    cxxime-ipc
    cxxime-engine
    cxxime-ui
    ole32
    oleaut32
    uuid
    advapi32
    imm32
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CXXIME_TSF_OUTPUT_NAME "zhiyi_tsf_x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(CXXIME_TSF_OUTPUT_NAME "zhiyi_tsf_x86")
else()
    message(FATAL_ERROR "Unsupported TSF target pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()

set_target_properties(cxxime-tsf PROPERTIES
    OUTPUT_NAME "${CXXIME_TSF_OUTPUT_NAME}"
    SUFFIX ".dll"
)

cxxime_add_version_resource(
    cxxime-tsf
    "知意输入法 文本服务"
    "${CXXIME_TSF_OUTPUT_NAME}.dll"
    VFT_DLL
)
