project(cxxime-server CXX)

add_executable(cxxime-server WIN32
    src/config_store.cc
    src/config_write_coordinator.cc
    src/continuation_worker.cc
    src/next_words_worker.cc
    src/candidate_preference_save_worker.cc
    src/diagnostic_log_maintenance_worker.cc
    src/input_method_hotkey.cc
    src/ipc_response_builder.cc
    src/main.cc
    src/server_app.cc
    src/session_manager.cc
    src/system_lifecycle_monitor.cc
    src/lexicon_control_handler.cc
    src/machine_translator.cc
    src/ui_candidate_page_conversion.cc
    src/ui_presentation_controller.cc
    src/ui_presentation_router.cc
    src/user_backup_service.cc
    src/voice_overlay.cc
    src/voice_recorder.cc
    src/voice_service.cc
    src/word_list_service.cc
    src/phrase_service.cc
)

target_include_directories(cxxime-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(cxxime-server PRIVATE
    cxxime-shared
    cxxime-engine
    cxxime-ipc
    cxxime-ui
    shell32
    advapi32
    ole32
    uuid
    wtsapi32
    d2d1
    dwrite
    shcore
)

# Laya: ONNX Runtime DLLs and (when present) the model next to the server executable.
if(LAYA_ORT_RUNTIME_FILES)
    add_custom_command(TARGET cxxime-server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LAYA_ORT_RUNTIME_FILES} $<TARGET_FILE_DIR:cxxime-server>
    )
endif()
set(LAYA_MODEL_SOURCE_DIR ${CMAKE_SOURCE_DIR}/models/laya CACHE PATH "Laya model directory (laya.int8g.onnx, tokenizer.json, rl_agent_config.json, unified_vocab.json)")
if(EXISTS ${LAYA_MODEL_SOURCE_DIR}/tokenizer.json)
    # Copied by a file-level rule (not POST_BUILD), so replacing the model updates the copy
    # without relinking the server.
    set(LAYA_MODEL_FILES tokenizer.json)
    # The unified model (choice + long-pinyin continuation) reads its units and characters
    # from unified_vocab.json; without it the server keeps to the word choice.
    if(EXISTS ${LAYA_MODEL_SOURCE_DIR}/unified_vocab.json)
        list(APPEND LAYA_MODEL_FILES unified_vocab.json)
    endif()
    # The one graph with the context's KV cache, run on the CPU instead of the whole graph
    # (engine/src/laya/reranker.h kOneGraphFiles); the next-word one graph comes with
    # next_rows.json (its rows' texts and each language's masks, engine/src/laya/next_words.h); a Lite
    # model's one graph (laya.one.w4.onnx) has its decoder beside it (laya.next.head / step.w4.onnx);
    # zhen-feather-1127 is one file (laya.feather.onnx). rl_agent_config.json: only the old Laya prompt format.
    foreach(file rl_agent_config.json laya.feather.onnx laya.one.next.slim.onnx laya.one.next.w8.onnx next_rows.json laya.one.w4.onnx
            laya.next.head.w4.onnx laya.next.step.w4.onnx laya.one.fused.int8g.onnx laya.one.int8g.onnx laya.int8g.onnx)
        if(EXISTS ${LAYA_MODEL_SOURCE_DIR}/${file})
            list(APPEND LAYA_MODEL_FILES ${file})
        endif()
    endforeach()
    set(LAYA_MODEL_OUTPUTS)
    foreach(file ${LAYA_MODEL_FILES})
        set(out ${CMAKE_CURRENT_BINARY_DIR}/laya/${file})
        add_custom_command(OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LAYA_MODEL_SOURCE_DIR}/${file} ${out}
            DEPENDS ${LAYA_MODEL_SOURCE_DIR}/${file}
            COMMENT "Copying Laya model file ${file}")
        list(APPEND LAYA_MODEL_OUTPUTS ${out})
    endforeach()
    add_custom_target(laya-model ALL DEPENDS ${LAYA_MODEL_OUTPUTS})
    add_dependencies(cxxime-server laya-model)
else()
    message(STATUS "Laya model not found in ${LAYA_MODEL_SOURCE_DIR}; reranking stays off until it is installed")
endif()

set_target_properties(cxxime-server PROPERTIES OUTPUT_NAME "zhiyi-server")
cxxime_add_version_resource(
    cxxime-server
    "知意输入法 后台服务"
    "zhiyi-server.exe"
    VFT_APP
)
