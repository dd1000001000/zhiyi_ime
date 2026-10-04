// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "support/testutil.h"

#include <cxxime/diagnostics_config.h>
#include <cxxime/host_trace.h>

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

TEST(HostTrace, writes_versioned_jsonl_without_text_payload) {
    wchar_t temp_root[MAX_PATH] = {};
    ASSERT_TRUE(GetTempPathW(ARRAYSIZE(temp_root), temp_root) > 0);
    const std::wstring directory = std::wstring(temp_root) + L"zhiyi-host-trace-" +
                                   std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    ASSERT_TRUE(SetEnvironmentVariableW(L"ZHIYIIME_HOST_TRACE_DIR", directory.c_str()) != FALSE);

    cxxime::DiagnosticsConfig diagnostics;
    diagnostics.trace_mode = cxxime::DiagnosticTraceMode::kNormal;
    cxxime::set_diagnostics_config(diagnostics);
    cxxime::write_host_trace("trace-test", "candidate.snapshot", {
        {"input_id", 42},
        {"composition_id", 43},
        {"count", 2},
        {"selection", 0},
        {"text_lengths", {1, 2}},
        {"result", "captured"},
    });

    const std::wstring arch = std::string(cxxime::host_trace_arch()) == "x64" ? L"x64" : L"x86";
    const std::wstring path = directory + L"\\host-trace-test-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" + arch + L".jsonl";
    FILE* file = nullptr;
    ASSERT_EQ(_wfopen_s(&file, path.c_str(), L"rb"), 0);
    ASSERT_TRUE(file != nullptr);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    ASSERT_TRUE(size > 0);
    fseek(file, 0, SEEK_SET);
    std::vector<char> bytes(static_cast<size_t>(size) + 1, '\0');
    ASSERT_EQ(fread(bytes.data(), 1, static_cast<size_t>(size), file), static_cast<size_t>(size));
    fclose(file);

    const nlohmann::json record = nlohmann::json::parse(bytes.data());
    ASSERT_EQ(record["schema_version"].get<int>(), cxxime::kHostTraceSchemaVersion);
    ASSERT_EQ(record["product_version"].get<std::string>(),
              std::string(cxxime::host_trace_product_version()));
    ASSERT_EQ(record["event"].get<std::string>(), std::string("candidate.snapshot"));
    ASSERT_EQ(record["count"].get<int>(), 2);
    ASSERT_TRUE(!record.contains("text"));
    ASSERT_TRUE(!record.contains("candidates"));

    SetEnvironmentVariableW(L"ZHIYIIME_HOST_TRACE_DIR", nullptr);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());
}

TEST(HostTrace, correlates_callbacks_for_the_same_key_message) {
    const uint64_t first = cxxime::host_trace_input_id('A', 0x00010001);
    const uint64_t second = cxxime::host_trace_input_id('A', 0x40010001);
    const uint64_t other_key = cxxime::host_trace_input_id('B', 0x00010001);
    ASSERT_EQ(first, second);
    ASSERT_NE(first, other_key);
}

TEST(HostTrace, hashes_utf16_without_recording_text) {
    const std::wstring first = L"candidate-a";
    const std::wstring second = L"candidate-b";
    const std::string first_digest = cxxime::host_trace_digest_utf16(first);
    ASSERT_EQ(first_digest.size(), static_cast<size_t>(64));
    ASSERT_EQ(first_digest, cxxime::host_trace_digest_utf16(first.data(), first.size()));
    ASSERT_NE(first_digest, cxxime::host_trace_digest_utf16(second));
    ASSERT_TRUE(first_digest.find("candidate") == std::string::npos);
}

RUN_ALL_TESTS()
