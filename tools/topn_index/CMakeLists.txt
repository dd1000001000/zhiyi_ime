project(topn_index_tools CXX)

add_library(cxxime-topn-index-builder STATIC
    candidate_store_file.cc
    index_writer.cc
    intermediate_reader.cc
)
target_include_directories(cxxime-topn-index-builder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/include
    ${CMAKE_SOURCE_DIR}/engine/src
)
target_link_libraries(cxxime-topn-index-builder PUBLIC darts_clone)

add_executable(topn_builder main.cc)
target_link_libraries(topn_builder PRIVATE cxxime-topn-index-builder)

if(CXXIME_BUILD_TOOLS OR CXXIME_BUILD_TESTS)
    add_library(cxxime-topn-index-reader STATIC index_reader.cc)
    target_include_directories(cxxime-topn-index-reader PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/engine/src
    )
endif()

if(CXXIME_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark.cc")
    add_executable(topn_benchmark benchmark.cc)
    target_link_libraries(topn_benchmark PRIVATE
        cxxime-topn-index-builder
        cxxime-topn-index-reader
    )
endif()

if(CXXIME_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/topn_index_test.cc")
    add_executable(topn_index_test topn_index_test.cc)
    target_link_libraries(topn_index_test PRIVATE
        cxxime-topn-index-builder
        cxxime-topn-index-reader
    )
    add_test(NAME topn_index_test COMMAND topn_index_test)
endif()
