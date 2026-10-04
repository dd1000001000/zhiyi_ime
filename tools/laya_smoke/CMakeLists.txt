# Laya end-to-end smoke test (needs the built dictionary in data/ and the model in models/laya).
add_executable(laya_smoke laya_smoke.cc)
target_compile_definitions(laya_smoke PRIVATE CXXIME_PROJECT_DIR="${CMAKE_SOURCE_DIR}/")
target_compile_options(laya_smoke PRIVATE /utf-8)
target_link_libraries(laya_smoke PRIVATE cxxime-engine)
add_custom_command(TARGET laya_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LAYA_ORT_RUNTIME_FILES} $<TARGET_FILE_DIR:laya_smoke>
)

# C++ reranker accuracy / parity on evaluation samples (jsonl; the evaluation sets are private).
add_executable(laya_parity laya_parity.cc)
target_include_directories(laya_parity PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
target_compile_options(laya_parity PRIVATE /utf-8)
target_link_libraries(laya_parity PRIVATE cxxime-engine nlohmann_json)
add_custom_command(TARGET laya_parity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LAYA_ORT_RUNTIME_FILES} $<TARGET_FILE_DIR:laya_parity>
)
