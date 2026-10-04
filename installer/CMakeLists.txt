project(cxxime-installer-helper CXX)

add_library(cxxime-installer-lock STATIC
    src/installer_lock.cc
    src/installer_lifecycle.cc
    src/installer_lifecycle_cleanup.cc
    src/installer_lifecycle_state.cc
    src/installer_lifecycle_internal.h
    src/installer_version.cc
    src/installer_ui_language.cc
    include/cxxime/installer_version.h
    include/cxxime/installer_ui_language.h
)

target_include_directories(cxxime-installer-lock PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cxxime-installer-lock PUBLIC Rstrtmgr.lib nlohmann_json ole32.lib)

add_executable(cxxime-installer-helper
    installer.manifest
    include/cxxime/installer_path_security.h
    include/cxxime/installer_lifecycle.h
    include/cxxime/installer_server_process.h
    include/cxxime/installer_tsf.h
    include/cxxime/installer_version.h
    src/main.cc
    src/installer_lifecycle_command.cc
    src/installer_lifecycle_command.h
    src/installer_path_security.cc
    src/installer_server_process.cc
    src/installer_tsf.cc
)

target_include_directories(cxxime-installer-helper PRIVATE
    ${CMAKE_SOURCE_DIR}/shared/include
)

target_link_libraries(cxxime-installer-helper PRIVATE
    cxxime-installer-lock
    cxxime-ipc
    ole32.lib
    uuid.lib
    advapi32.lib
)

set_target_properties(cxxime-installer-helper PROPERTIES OUTPUT_NAME "zhiyi-installer-helper")
cxxime_add_version_resource(
    cxxime-installer-helper
    "知意输入法 安装助手"
    "zhiyi-installer-helper.exe"
    VFT_APP
)
