project(cxxime-settings CXX)

add_executable(cxxime-settings WIN32
    main.cc
    editor_about_panel.cc
    editor_app.cc
    editor_app.h
    editor_app_internal.h
    editor_controls.cc
    editor_general_panel.cc
    editor_learning_panel.cc
    editor_theme.cc
    editor_update_panel.cc
    glossary_packs.cc
    glossary_packs.h
    hotkey_check.cc
    hotkey_check.h
    i18n.cc
    i18n.h
    key_capture.cc
    key_capture.h
    settings.manifest
    settings.rc
)

set_source_files_properties(settings.rc PROPERTIES
    COMPILE_FLAGS "/i${CMAKE_SOURCE_DIR}"
    # The Ninja generator's resource compiler rule ignores the /i flag above; a source-level
    # include directory works with every generator. (Not added to the target: the repo root
    # contains a VERSION file that would shadow the C++ <version> header.)
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}
)

target_include_directories(cxxime-settings PRIVATE
    ${CXXIME_GENERATED_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/include
    ${CMAKE_SOURCE_DIR}/resource
    ${CMAKE_SOURCE_DIR}/shared/include
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
)

target_link_libraries(cxxime-settings PRIVATE
    cxxime-engine
    cxxime-ipc
    cxxime-shared
    cxxime-ui
    cxxime-update
    comctl32.lib
    imm32.lib
)

set_target_properties(cxxime-settings PROPERTIES OUTPUT_NAME "zhiyi-settings")
cxxime_add_version_resource(
    cxxime-settings
    "知意输入法 设置"
    "zhiyi-settings.exe"
    VFT_APP
)
