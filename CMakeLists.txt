cmake_minimum_required(VERSION 3.15)

set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CXXIME_VERSION LIMIT_COUNT 1)
string(STRIP "${CXXIME_VERSION}" CXXIME_VERSION)
if(NOT CXXIME_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must contain a valid semantic version")
endif()
set(CXXIME_VERSION_BASE "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(cxxime VERSION "${CXXIME_VERSION_BASE}" LANGUAGES C CXX)

set(CXXIME_VERSION_NUMERIC
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0"
)
set(CXXIME_VERSION_NUMERIC_DOTTED
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0"
)
set(CXXIME_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${CXXIME_GENERATED_INCLUDE_DIR}/cxxime")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${CXXIME_GENERATED_INCLUDE_DIR}/cxxime/version.h"
    @ONLY
)
include(cmake/CxximeVersion.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
    # Static link VC++ runtime — no vcredist dependency on target machines
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()

option(CXXIME_BUILD_TESTS "Build tests" ON)
option(CXXIME_BUILD_TOOLS "Build development tools" ON)
option(CXXIME_BUILD_HOST_PROBE "Build the IME host diagnostic probe" OFF)
option(CXXIME_ENABLE_HOST_DIAGNOSTICS "Compile host diagnostic instrumentation" OFF)
set(CXXIME_HOST_TAKEOVER_DIAGNOSTICS_DIR
    "${CMAKE_SOURCE_DIR}/diagnostics/host_takeover"
)

# Data directory resolution:
#   Production (default): CXXIME_PRODUCTION_BUILD=ON  → <exe_dir>/data/
#   Development:          CXXIME_PRODUCTION_BUILD=OFF → ${CMAKE_SOURCE_DIR}/data/
option(CXXIME_PRODUCTION_BUILD "Build for production install" ON)

if(NOT CXXIME_PRODUCTION_BUILD)
    add_compile_definitions(CXXIME_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")
endif()

add_subdirectory(third_party/sqlite3)
add_subdirectory(third_party/nlohmann)
add_subdirectory(third_party/darts-clone)
add_subdirectory(third_party/miniz)
add_subdirectory(shared)
add_subdirectory(engine)
add_subdirectory(ipc)
add_subdirectory(legacy_ime)
add_subdirectory(resource)
add_subdirectory(tsf)
add_subdirectory(server)
add_subdirectory(ui)
add_subdirectory(installer)
add_subdirectory(update)

if(CXXIME_BUILD_HOST_PROBE)
    add_subdirectory(diagnostics/host_takeover/probe)
endif()

add_custom_target(cxxime-platform-modules
    DEPENDS cxxime-ime cxxime-tsf
)
if(CXXIME_BUILD_HOST_PROBE)
    add_dependencies(cxxime-platform-modules cxxime-ime-host-probe)
endif()

if(CXXIME_BUILD_TESTS)
    enable_testing()
endif()

# The release dictionary pipeline always needs topn_builder. Benchmark tools
# remain conditional inside this subdirectory.
add_subdirectory(tools/topn_index)

if(CXXIME_BUILD_TESTS)
    add_subdirectory(test)
endif()

if(CXXIME_BUILD_TOOLS)
    add_subdirectory(tools/dict_query)
    add_subdirectory(tools/laya_smoke)
    add_subdirectory(tools/sqlite_query)
    add_subdirectory(tools/ipc_test)
    add_subdirectory(tools/candidate_window_tool)
    add_subdirectory(tools/tsf_position_tool)
    add_subdirectory(tools/status_window_tool)
    add_subdirectory(tools/query_bench)
    add_subdirectory(tools/tray_icon_tool)
    add_subdirectory(tools/punct_test)
    add_subdirectory(tools/update_tool)
endif()

add_subdirectory(settings)
