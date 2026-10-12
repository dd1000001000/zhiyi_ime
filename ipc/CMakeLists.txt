project(cxxime-ipc CXX)

add_library(cxxime-ipc STATIC
    src/control_client.cc
    src/control_protocol.cc
    src/control_server.cc
    src/ipc_server.cc
    src/ipc_client.cc
    src/pipe_names.cc
    src/ui_channel_client.cc
    src/ui_channel_server.cc
    src/ui_pipe.cc
    src/ui_protocol.cc
    src/lexicon_control.cc
    src/user_backup_control.cc
    src/word_list_control.cc
)

target_include_directories(cxxime-ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cxxime-ipc PUBLIC
    cxxime-shared
)

target_link_libraries(cxxime-ipc PRIVATE
    nlohmann_json
)
