project(cxxime-update-tool CXX)

# Checks a published release the way Settings > Updates does:
#   zhiyi-update-tool check              latest.json, signature, and whether it is newer
#   zhiyi-update-tool download <folder>  also downloads and verifies the installer
add_executable(cxxime-update-tool main.cc)
target_link_libraries(cxxime-update-tool PRIVATE cxxime-update)
set_target_properties(cxxime-update-tool PROPERTIES OUTPUT_NAME "zhiyi-update-tool")
