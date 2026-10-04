project(query_bench CXX)

add_executable(query_bench main.cc)

target_link_libraries(query_bench PRIVATE cxxime-engine)
