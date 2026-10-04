project(cxxime-ime-host-probe CXX)

add_executable(cxxime-ime-host-probe WIN32
    main.cc
    compartment_probe.cc
    display_attribute_probe.cc
    probe_app.cc
    probe_candidate_click.cc
    profile_probe.cc
    probe_imm_candidates.cc
    probe_ui_elements.cc
    probe_visibility.cc
    probe_window.cc
    ui_element_sink.cc
    window_owner_probe.cc
)

if(NOT CXXIME_ENABLE_HOST_DIAGNOSTICS)
    target_sources(cxxime-ime-host-probe PRIVATE
        ${CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR}/src/host_trace.cc
    )
endif()

target_include_directories(cxxime-ime-host-probe PRIVATE
    ${CXXIME_GENERATED_INCLUDE_DIR}
)

target_link_libraries(cxxime-ime-host-probe PRIVATE
    cxxime-shared
    ole32
    oleaut32
    uuid
    imm32
    user32
    gdi32
    dwmapi
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CXXIME_PROBE_OUTPUT_NAME "zhiyi-ime-host-probe-x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(CXXIME_PROBE_OUTPUT_NAME "zhiyi-ime-host-probe-x86")
else()
    message(FATAL_ERROR "Unsupported Probe target pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()

set_target_properties(cxxime-ime-host-probe PROPERTIES
    OUTPUT_NAME "${CXXIME_PROBE_OUTPUT_NAME}"
)

cxxime_add_version_resource(
    cxxime-ime-host-probe
    "知意输入法 宿主探测工具"
    "${CXXIME_PROBE_OUTPUT_NAME}.exe"
    VFT_APP
)
