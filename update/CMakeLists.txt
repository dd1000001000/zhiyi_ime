project(cxxime-update CXX)

# Settings > Updates: release check, download and signature (include/cxxime/update.h).
add_library(cxxime-update STATIC
    include/cxxime/update.h
    src/update_crypto.cc
    src/update_http.cc
    src/update_manifest.cc
    src/update_public_key.inc
)

target_include_directories(cxxime-update PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(cxxime-update PRIVATE
    ${CXXIME_GENERATED_INCLUDE_DIR}
)

target_link_libraries(cxxime-update
    PUBLIC cxxime-shared
    PRIVATE cxxime-installer-lock nlohmann_json bcrypt crypt32 winhttp shell32
)
