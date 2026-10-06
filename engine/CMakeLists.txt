project(cxxime-engine CXX)

add_library(cxxime-engine STATIC
    src/composition_state.cc
    src/composition_learning.cc
    src/composition_presentation.cc
    src/context.cc
    src/pinyin_segment_paths.cc
    src/pinyin_segmentor.cc
    src/pinyin_resource.cc
    src/pinyin_scheme.cc
    src/pinyin_user_code.cc
    src/dict.cc
    src/dict_user_data.cc
    src/dict_span_query.cc
    src/mapped_file.cc
    src/dict_wubi_query.cc
    src/user_data_file.cc
    src/disabled_system_lexicon.cc
    src/user_lexicon.cc
    src/user_lexicon_state.cc
    src/user_lexicon_query.cc
    src/user_data_merge.cc
    src/candidate_preference.cc
    src/manual_candidate_order.cc
    src/wubi_prefix_index.cc
    src/spellings_index.cc
    src/syllabifier.cc
    src/pinyin_composer.cc
    src/pinyin_path_filter.cc
    src/pinyin_partial_candidates.cc
    src/pinyin_translator.cc
    src/pinyin_processor.cc
    src/wubi_translator.cc
    src/wubi_processor.cc
    src/wubi_input_policy.cc
    src/config.cc
    src/engine.cc
    src/engine_runtime.cc
    src/ascii_composer.cc
    src/short_code_cache.cc
    src/output_composer.cc
    src/symbol_processor.cc
    src/symbol_table.cc
)

target_include_directories(cxxime-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cxxime-engine PUBLIC
    cxxime-shared
    nlohmann_json
)

# ---- Laya context reranking (ONNX Runtime, CPU) ----
# onnxruntime.dll is delay-loaded: executables that link the engine but never load a model
# (tests, tools, settings) start without it. LayaRerank only calls into ONNX Runtime after
# checking that the model files exist and the DLL can be loaded.
# The x86 build only produces the in-process modules for 32-bit applications (the server is
# 64-bit), so it gets a no-op LayaRerank and no ONNX Runtime.
target_sources(cxxime-engine PRIVATE src/english_lexicon.cc src/english_candidates.cc src/english_learning.cc
    src/experience_log.cc)
target_include_directories(cxxime-engine PRIVATE ${CXXIME_GENERATED_INCLUDE_DIR})
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LAYA_ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime CACHE PATH "ONNX Runtime (onnxruntime-win-x64) root")
    if(NOT EXISTS ${LAYA_ORT_DIR}/include/onnxruntime_cxx_api.h)
        message(FATAL_ERROR "ONNX Runtime not found in ${LAYA_ORT_DIR}; run python scripts/fetch_onnxruntime.py")
    endif()
    target_sources(cxxime-engine PRIVATE
        src/laya_rerank.cc
        src/laya/text_util.cpp
        src/laya/tokenizer.cpp
        src/laya/reranker.cpp
    )
    target_include_directories(cxxime-engine PRIVATE ${LAYA_ORT_DIR}/include)
    target_link_libraries(cxxime-engine PUBLIC ${LAYA_ORT_DIR}/lib/onnxruntime.lib delayimp)
    target_link_options(cxxime-engine INTERFACE /DELAYLOAD:onnxruntime.dll)
    set(LAYA_ORT_RUNTIME_FILES
        ${LAYA_ORT_DIR}/lib/onnxruntime.dll
        ${LAYA_ORT_DIR}/lib/onnxruntime_providers_shared.dll
        CACHE INTERNAL "ONNX Runtime DLLs shipped next to cxxime-server.exe")
else()
    target_sources(cxxime-engine PRIVATE src/laya_rerank_stub.cc)
    set(LAYA_ORT_RUNTIME_FILES "" CACHE INTERNAL "ONNX Runtime DLLs shipped next to cxxime-server.exe")
endif()
