project(cxxime-resources C)

add_library(cxxime-resources SHARED
    cxxime-resources.rc
)

set_source_files_properties(cxxime-resources.rc PROPERTIES
    COMPILE_FLAGS "/i${CMAKE_CURRENT_SOURCE_DIR}/.."
)

set_target_properties(cxxime-resources PROPERTIES
    LINKER_LANGUAGE C
    LINK_FLAGS "/NOENTRY"
    OUTPUT_NAME "zhiyu-resources"
    SUFFIX ".dll"
)

cxxime_add_version_resource(
    cxxime-resources
    "知语输入法 资源库"
    "zhiyu-resources.dll"
    VFT_DLL
)
