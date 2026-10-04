// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <json.hpp>

#include <cxxime/control_client.h>
#include <cxxime/control_protocol.h>
#include <cxxime/user_backup_control.h>
#include <cxxime/control_server.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/lexicon_control.h>
#include <cxxime/pipe_names.h>

#include "support/ipc_baseline_fixture.h"
#include "support/testutil.h"

namespace {

bool wait_for(const std::function<bool()>& condition, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::wstring test_pipe_name() {
    static std::atomic<unsigned long> sequence{0};
    return L"\\\\.\\pipe\\ZhiyiIME-Control-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(sequence.fetch_add(1));
}

} // namespace

TEST(ControlChannel, protocol_round_trip) {
    const std::string payload = R"({"theme":"azure"})";
    cxxime::ConfigGeneration generation{123, 456};
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_control_packet(cxxime::ControlMessageType::kConfigSnapshot,
                                              generation, payload.data(), payload.size(), &packet));

    cxxime::ControlMessage message;
    ASSERT_TRUE(cxxime::parse_control_packet(packet.data(), packet.size(), &message));
    ASSERT_EQ(message.type, cxxime::ControlMessageType::kConfigSnapshot);
    ASSERT_EQ(message.generation.server_epoch, 123ULL);
    ASSERT_EQ(message.generation.revision, 456ULL);
    ASSERT_TRUE(message.payload == payload);

    cxxime::ControlHeader header = {};
    std::memcpy(&header, packet.data(), sizeof(header));
    header.protocol_version = cxxime::CONTROL_PROTOCOL_VERSION + 1;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(cxxime::parse_control_packet(packet.data(), packet.size(), &message));
    ASSERT_TRUE(message.payload == payload);
    header.message_type = static_cast<cxxime::ControlMessageType>(0xffff);
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(cxxime::parse_control_packet(packet.data(), packet.size(), &message));
    ASSERT_EQ(static_cast<std::uint16_t>(message.type), static_cast<std::uint16_t>(0xffff));
}

TEST(ControlChannel, protocol_rejects_invalid_header_and_oversized_payload) {
    std::vector<std::uint8_t> packet(sizeof(cxxime::ControlHeader), 0);
    cxxime::ControlMessage message;
    ASSERT_TRUE(!cxxime::parse_control_packet(packet.data(), packet.size(), &message));

    std::vector<std::uint8_t> payload(cxxime::CONTROL_MAX_PAYLOAD + 1, 0);
    ASSERT_TRUE(!cxxime::build_control_packet(cxxime::ControlMessageType::kConfigSnapshot, {},
                                               payload.data(), payload.size(), &packet));
}

TEST(ControlChannel, explicit_user_pipe_name_is_not_rescoped) {
    const std::wstring scoped =
        cxxime::make_user_pipe_name(cxxime::CONTROL_PIPE_BASE_NAME, L"interactive-user");
    ASSERT_TRUE(scoped == L"\\\\.\\pipe\\interactive-user\\ZhiyiIME-Control");
    ASSERT_TRUE(cxxime::make_user_pipe_name(scoped, L"elevated-admin") == scoped);
}

TEST(ControlChannel, fixed_payloads_accept_baseline_prefix_and_future_tail) {
    cxxime::ControlSubscribe subscribe;
    std::string subscribe_payload(cxxime::CONTROL_SUBSCRIBE_BASELINE_SIZE, '\0');
    const std::uint32_t process_id = 42;
    const std::uint16_t pointer_size = 8;
    std::memcpy(&subscribe_payload[0], &process_id, sizeof(process_id));
    std::memcpy(&subscribe_payload[sizeof(process_id)], &pointer_size, sizeof(pointer_size));
    ASSERT_TRUE(cxxime::decode_control_subscribe(subscribe_payload, &subscribe));
    ASSERT_EQ(subscribe.process_id, process_id);
    ASSERT_EQ(subscribe.pointer_size, pointer_size);
    subscribe_payload.append(4, '\0');
    ASSERT_TRUE(cxxime::decode_control_subscribe(subscribe_payload, &subscribe));

    cxxime::ControlMutationResult result;
    std::string result_payload(cxxime::CONTROL_MUTATION_RESULT_BASELINE_SIZE, '\0');
    const std::uint32_t succeeded = 1;
    std::memcpy(&result_payload[0], &succeeded, sizeof(succeeded));
    ASSERT_TRUE(cxxime::decode_control_mutation_result(result_payload, &result));
    ASSERT_EQ(result.succeeded, succeeded);
    result_payload.append(4, '\0');
    ASSERT_TRUE(cxxime::decode_control_mutation_result(result_payload, &result));

    ASSERT_TRUE(!cxxime::decode_control_subscribe(
        std::string(cxxime::CONTROL_SUBSCRIBE_BASELINE_SIZE - 1, '\0'), &subscribe));
    ASSERT_TRUE(!cxxime::decode_control_mutation_result(
        std::string(cxxime::CONTROL_MUTATION_RESULT_BASELINE_SIZE - 1, '\0'), &result));
}

TEST(ControlChannel, publishes_initial_and_replaced_snapshots) {
    const std::wstring pipe_name = test_pipe_name();
    const std::string initial = R"({"theme":"azure"})";
    const std::string reloaded = R"({"theme":"dark"})";

    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        initial,
        [&](cxxime::UserConfigMutationKind kind, const std::string& payload,
            std::string* config_json, unsigned long* error_code) {
            ASSERT_EQ(kind, cxxime::UserConfigMutationKind::kReplace);
            ASSERT_TRUE(payload == reloaded);
            *config_json = reloaded;
            *error_code = ERROR_SUCCESS;
            return true;
        },
        pipe_name));

    std::atomic<int> callback_count{0};
    std::mutex result_mutex;
    cxxime::ConfigGeneration last_generation;
    std::string last_json;
    cxxime::ControlClient client;
    ASSERT_TRUE(client.start(
        [&](cxxime::ConfigGeneration generation, const std::string& config_json) {
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                last_generation = generation;
                last_json = config_json;
            }
            callback_count.fetch_add(1);
        },
        pipe_name));

    ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 1; }));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_EQ(last_generation.revision, 1ULL);
        ASSERT_TRUE(last_json == initial);
    }

    cxxime::ConfigGeneration replaced_generation;
    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(
        cxxime::replace_user_config(reloaded, &replaced_generation, &error_code, 3000, pipe_name));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_SUCCESS));
    ASSERT_EQ(replaced_generation.revision, 2ULL);
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 2; }));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_TRUE(last_generation == replaced_generation);
        ASSERT_TRUE(last_json == reloaded);
    }

    client.stop();
    server.stop();
}

TEST(ControlChannel, failed_mutation_keeps_current_generation) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})",
        [](cxxime::UserConfigMutationKind, const std::string&, std::string*,
           unsigned long* error_code) {
            *error_code = ERROR_INVALID_DATA;
            return false;
        },
        pipe_name));

    cxxime::ConfigGeneration generation;
    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(!cxxime::replace_user_config(R"({"theme":"dark"})", &generation, &error_code, 3000,
                                             pipe_name));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_INVALID_DATA));
    ASSERT_EQ(generation.revision, 1ULL);
    ASSERT_TRUE(generation == server.generation());
    server.stop();
}

TEST(ControlChannel, persistent_client_can_send_merge_patch) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> callback_count{0};

    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})",
        [](cxxime::UserConfigMutationKind kind, const std::string& payload,
           std::string* config_json, unsigned long* error_code) {
            ASSERT_EQ(kind, cxxime::UserConfigMutationKind::kMergePatch);
            ASSERT_TRUE(payload == R"({"theme":"dark"})");
            *config_json = R"({"theme":"dark"})";
            *error_code = ERROR_SUCCESS;
            return true;
        },
        pipe_name));

    cxxime::ControlClient client;
    ASSERT_TRUE(client.start(
        [&](cxxime::ConfigGeneration, const std::string&) { callback_count.fetch_add(1); },
        pipe_name));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 1; }));

    client.patch_user_config(R"({"theme":"dark"})");
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 2; }));
    ASSERT_EQ(server.generation().revision, 2ULL);

    client.stop();
    server.stop();
}

TEST(ControlChannel, multiple_subscribers_receive_the_same_generation) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> first_count{0};
    std::atomic<int> second_count{0};
    std::mutex generation_mutex;
    cxxime::ConfigGeneration first_generation;
    cxxime::ConfigGeneration second_generation;

    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})",
        [](cxxime::UserConfigMutationKind kind, const std::string&, std::string* config_json,
           unsigned long* error_code) {
            ASSERT_EQ(kind, cxxime::UserConfigMutationKind::kReplace);
            *config_json = R"({"theme":"dark"})";
            *error_code = ERROR_SUCCESS;
            return true;
        },
        pipe_name));

    cxxime::ControlClient first;
    cxxime::ControlClient second;
    ASSERT_TRUE(first.start(
        [&](cxxime::ConfigGeneration generation, const std::string&) {
            std::lock_guard<std::mutex> lock(generation_mutex);
            first_generation = generation;
            first_count.fetch_add(1);
        },
        pipe_name));
    ASSERT_TRUE(second.start(
        [&](cxxime::ConfigGeneration generation, const std::string&) {
            std::lock_guard<std::mutex> lock(generation_mutex);
            second_generation = generation;
            second_count.fetch_add(1);
        },
        pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.subscriber_count() == 2; }));

    ASSERT_TRUE(
        cxxime::replace_user_config(R"({"theme":"dark"})", nullptr, nullptr, 3000, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return first_count.load() >= 2 && second_count.load() >= 2; }));
    {
        std::lock_guard<std::mutex> lock(generation_mutex);
        ASSERT_TRUE(first_generation == second_generation);
        ASSERT_TRUE(first_generation == server.generation());
    }

    first.stop();
    second.stop();
    server.stop();
}

TEST(ControlChannel, client_stops_promptly_without_server) {
    cxxime::ControlClient client;
    ASSERT_TRUE(client.start({}, test_pipe_name()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto start = std::chrono::steady_clock::now();
    client.stop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_TRUE(elapsed.count() < 1000);
}

TEST(ControlChannel, queued_patch_is_sent_after_server_connects) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> mutation_count{0};
    std::mutex snapshot_mutex;
    std::string last_snapshot;

    cxxime::ControlClient client;
    ASSERT_TRUE(client.start(
        [&](cxxime::ConfigGeneration, const std::string& config_json) {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            last_snapshot = config_json;
        },
        pipe_name));
    client.patch_user_config(R"({"theme":"dark"})");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})",
        [&](cxxime::UserConfigMutationKind kind, const std::string&, std::string* config_json,
            unsigned long* error_code) {
            ASSERT_EQ(kind, cxxime::UserConfigMutationKind::kMergePatch);
            mutation_count.fetch_add(1);
            *config_json = R"({"theme":"dark"})";
            *error_code = ERROR_SUCCESS;
            return true;
        },
        pipe_name));

    ASSERT_TRUE(wait_for([&]() { return mutation_count.load() == 1; }, 5000));
    ASSERT_TRUE(wait_for(
        [&]() {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            return last_snapshot == R"({"theme":"dark"})";
        },
        5000));
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        ASSERT_TRUE(last_snapshot == R"({"theme":"dark"})");
    }

    client.stop();
    server.stop();
}

TEST(ControlChannel, server_restart_changes_epoch) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> callback_count{0};
    std::mutex generation_mutex;
    cxxime::ConfigGeneration first_generation;
    cxxime::ConfigGeneration latest_generation;

    cxxime::ControlClient client;
    ASSERT_TRUE(client.start(
        [&](cxxime::ConfigGeneration generation, const std::string&) {
            std::lock_guard<std::mutex> lock(generation_mutex);
            if (callback_count.load() == 0) {
                first_generation = generation;
            }
            latest_generation = generation;
            callback_count.fetch_add(1);
        },
        pipe_name));

    {
        cxxime::ControlServer server;
        ASSERT_TRUE(server.start(R"({"theme":"azure"})", {}, pipe_name));
        ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 1; }));
        server.stop();
    }
    {
        cxxime::ControlServer server;
        ASSERT_TRUE(server.start(R"({"theme":"dark"})", {}, pipe_name));
        ASSERT_TRUE(wait_for([&]() { return callback_count.load() >= 2; }, 5000));
        server.stop();
    }

    client.stop();
    std::lock_guard<std::mutex> lock(generation_mutex);
    ASSERT_TRUE(first_generation.server_epoch != latest_generation.server_epoch);
    ASSERT_EQ(latest_generation.revision, 1ULL);
}

TEST(ControlChannel, lexicon_codec_rejects_invalid_requests) {
    cxxime::LexiconControlRequest request;
    ASSERT_TRUE(!cxxime::decode_lexicon_request("not-json", &request));
    ASSERT_TRUE(!cxxime::decode_lexicon_request(
        R"({"operation":"query","kind":"pinyin","query":"ni","offset":0,"limit":0})",
        &request));
    ASSERT_TRUE(!cxxime::decode_lexicon_request(
        R"({"operation":"query","kind":"unknown","query":"ni","offset":0,"limit":32})",
        &request));
    ASSERT_TRUE(!cxxime::decode_lexicon_request(
        R"({"operation":"delete","kind":"pinyin","resource":"user_lexicon",)"
        R"("text":"entry","code":"code"})",
        &request));

    request = {};
    request.operation = cxxime::LexiconOperation::kDelete;
    std::string payload;
    ASSERT_TRUE(!cxxime::encode_lexicon_request(request, &payload));
    request.entries.assign(cxxime::LEXICON_CONTROL_MAX_LIMIT + 1, {"entry", "entry"});
    ASSERT_TRUE(!cxxime::encode_lexicon_request(request, &payload));
    request.entries.assign(cxxime::LEXICON_CONTROL_MAX_LIMIT, {"entry", "entry"});
    request.entries.front() = {"first-text", "first-code"};
    request.entries.back() = {"last-text", "last-code"};
    ASSERT_TRUE(cxxime::encode_lexicon_request(request, &payload));
    cxxime::LexiconControlRequest decoded;
    ASSERT_TRUE(cxxime::decode_lexicon_request(payload, &decoded));
    ASSERT_EQ(decoded.entries.size(), cxxime::LEXICON_CONTROL_MAX_LIMIT);
    ASSERT_EQ(decoded.entries.front().text, "first-text");
    ASSERT_EQ(decoded.entries.front().code, "first-code");
    ASSERT_EQ(decoded.entries.back().text, "last-text");
    ASSERT_EQ(decoded.entries.back().code, "last-code");

    ASSERT_TRUE(!cxxime::decode_lexicon_request(
        R"({"operation":"delete","kind":"pinyin","resource":"user_lexicon","entries":[]})",
        &decoded));
    request.entries.assign(cxxime::LEXICON_CONTROL_MAX_LIMIT + 1, {"entry", "entry"});
    nlohmann::json oversized = {{"operation", "delete"},
                                {"kind", "pinyin"},
                                {"resource", "user_lexicon"},
                                {"entries", nlohmann::json::array()}};
    for (const auto& entry : request.entries) {
        oversized["entries"].push_back({{"text", entry.text}, {"code", entry.code}});
    }
    ASSERT_TRUE(!cxxime::decode_lexicon_request(oversized.dump(), &decoded));

    request = {};
    request.operation = cxxime::LexiconOperation::kAdd;
    request.text.assign(cxxime::CONTROL_MAX_PAYLOAD, 'x');
    request.code = "x";
    ASSERT_TRUE(!cxxime::encode_lexicon_request(request, &payload));

    request.text = std::string("\xc3\x28", 2);
    request.code = "valid";
    payload = "stale";
    ASSERT_TRUE(!cxxime::encode_lexicon_request(request, &payload));
    ASSERT_TRUE(payload.empty());
}

TEST(ControlChannel, lexicon_codec_accepts_baseline_query_without_appended_fields) {
    cxxime::LexiconControlRequest request;
    ASSERT_TRUE(cxxime::decode_lexicon_request(
        R"({"operation":"query","kind":"pinyin","resource":"user_lexicon",)"
        R"("query":"ni","offset":0,"limit":16})",
        &request));
    ASSERT_TRUE(!request.exact_text);

    cxxime::LexiconControlResult result;
    ASSERT_TRUE(cxxime::decode_lexicon_result(
        R"({"operation":"query","succeeded":true,"error_code":0,)"
        R"("resource_total":1,"match_total":1,"offset":0,"has_more":false,)"
        R"("entries":[{"text":"baseline","code":"baseline","frequency":1,)"
        R"("sequence":0}]})",
        &result));
    ASSERT_EQ(result.query.entries.size(), static_cast<std::size_t>(1));
    ASSERT_TRUE(result.query.entries.front().syllables.empty());

    ASSERT_TRUE(!cxxime::decode_lexicon_request(
        R"({"operation":"query","kind":"pinyin","resource":"user_lexicon",)"
        R"("query":"ni","exact_text":"false","offset":0,"limit":16})",
        &request));
}

TEST(ControlChannel, candidate_order_codec_preserves_entries_and_version) {
    cxxime::LexiconControlRequest request;
    request.operation = cxxime::LexiconOperation::kSetCandidateOrder;
    request.kind = cxxime::UserDictKind::WUBI;
    request.resource = cxxime::LexiconResource::kManualCandidateOrder;
    request.code = "yiy";
    request.expected_version = 17;
    request.candidate_order = {{"应该", "yiyy", ""}, {"就让", "yiya", ""}};

    std::string payload;
    ASSERT_TRUE(cxxime::encode_lexicon_request(request, &payload));
    cxxime::LexiconControlRequest decoded;
    ASSERT_TRUE(cxxime::decode_lexicon_request(payload, &decoded));
    ASSERT_EQ(decoded.operation, cxxime::LexiconOperation::kSetCandidateOrder);
    ASSERT_EQ(decoded.resource, cxxime::LexiconResource::kManualCandidateOrder);
    ASSERT_EQ(decoded.code, "yiy");
    ASSERT_EQ(decoded.expected_version, 17ULL);
    ASSERT_EQ(decoded.candidate_order.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(decoded.candidate_order[1].text, "就让");

    cxxime::LexiconControlResult result;
    result.operation = cxxime::LexiconOperation::kQueryCandidateOrder;
    result.succeeded = true;
    result.candidate_order.input_code = "yiy";
    result.candidate_order.version = 18;
    result.candidate_order.has_more = true;
    result.candidate_order.entries.push_back(
        {"应该", "yiyy", "", cxxime::CandidateOrderReason::kManual});
    ASSERT_TRUE(cxxime::encode_lexicon_result(result, &payload));
    cxxime::LexiconControlResult decoded_result;
    ASSERT_TRUE(cxxime::decode_lexicon_result(payload, &decoded_result));
    ASSERT_EQ(decoded_result.candidate_order.version, 18ULL);
    ASSERT_TRUE(decoded_result.candidate_order.has_more);
    ASSERT_EQ(decoded_result.candidate_order.entries.front().reason,
              cxxime::CandidateOrderReason::kManual);
}

TEST(ControlChannel, user_backup_codec_and_client_preserve_summary) {
    cxxime::UserBackupControlRequest request;
    request.operation = cxxime::UserBackupOperation::kImport;
    request.path = "C:\\backup\\profile.zhiyi-backup";
    request.components = cxxime::kPortableUserBackupComponents;
    std::string payload;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    cxxime::UserBackupControlRequest decoded_request;
    ASSERT_TRUE(cxxime::decode_user_backup_request(payload, &decoded_request));
    ASSERT_EQ(decoded_request.path, request.path);
    ASSERT_EQ(decoded_request.components, request.components);
    ASSERT_EQ(decoded_request.operation, cxxime::UserBackupOperation::kImport);

    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})", {},
        [&](cxxime::ControlMessageType request_type, const std::string& request_payload,
            cxxime::ControlMessageType* response_type,
            std::string* response_payload) {
            ASSERT_EQ(request_type, cxxime::ControlMessageType::kUserBackupRequest);
            cxxime::UserBackupControlRequest received;
            ASSERT_TRUE(cxxime::decode_user_backup_request(request_payload, &received));
            cxxime::UserBackupControlResult result;
            result.operation = received.operation;
            result.succeeded = true;
            result.summary.format_version = cxxime::kUserBackupFormatVersion;
            result.summary.components = cxxime::kPortableUserBackupComponents;
            result.summary.app_version = "0.5.0";
            result.summary.created_at_utc = "2026-09-09T10:00:00Z";
            result.summary.entry_count = 10;
            result.summary.total_size = 4096;
            result.imported_count = 7;
            result.skipped_count = 2;
            *response_type = cxxime::ControlMessageType::kUserBackupResult;
            return cxxime::encode_user_backup_result(result, response_payload);
        },
        pipe_name));

    cxxime::UserBackupControlClient client(3000, pipe_name);
    cxxime::UserBackupControlResult result;
    ASSERT_TRUE(client.import_backup(request.path, request.components, &result));
    ASSERT_EQ(result.summary.entry_count, static_cast<std::size_t>(10));
    ASSERT_EQ(result.summary.total_size, 4096ULL);
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(7));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(2));
    server.stop();
}

TEST(ControlChannel, lexicon_client_supports_all_operations) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> request_count{0};

    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(
        R"({"theme":"azure"})", {},
        [&](cxxime::ControlMessageType request_type, const std::string& payload,
            cxxime::ControlMessageType* response_type,
            std::string* response_payload) {
            ASSERT_EQ(request_type, cxxime::ControlMessageType::kLexiconRequest);
            *response_type = cxxime::ControlMessageType::kLexiconResult;
            cxxime::LexiconControlRequest request;
            ASSERT_TRUE(cxxime::decode_lexicon_request(payload, &request));
            request_count.fetch_add(1);

            cxxime::LexiconControlResult result;
            result.operation = request.operation;
            result.succeeded = true;
            result.error_code = ERROR_SUCCESS;
            if (request.operation == cxxime::LexiconOperation::kQuery) {
                ASSERT_EQ(request.kind, cxxime::UserDictKind::WUBI);
                if (request.exact_text) {
                    ASSERT_TRUE(request.query == "你好");
                    ASSERT_EQ(request.offset, static_cast<std::size_t>(0));
                    ASSERT_EQ(request.limit, static_cast<std::size_t>(16));
                } else {
                    ASSERT_TRUE(request.query == "ni");
                    ASSERT_EQ(request.offset, static_cast<std::size_t>(2));
                    ASSERT_EQ(request.limit, static_cast<std::size_t>(3));
                }
                result.query.resource_total = 9;
                result.query.match_total = 4;
                result.query.offset = request.offset;
                result.query.has_more = true;
                result.query.entries.push_back({"你好", "wq", 7, 0});
            } else if (request.operation ==
                       cxxime::LexiconOperation::kQuerySystemEntryStatus) {
                ASSERT_EQ(request.texts.size(), static_cast<std::size_t>(2));
                result.query.resource_total = 1;
                result.query.match_total = 1;
                result.query.entries.push_back({request.texts[0], "", 1, 0});
            } else if (request.operation == cxxime::LexiconOperation::kImport) {
                ASSERT_EQ(request.source_path, "C:\\temp\\user_pinyin.tsv");
            } else if (request.operation == cxxime::LexiconOperation::kDelete) {
                ASSERT_EQ(request.entries.size(), static_cast<std::size_t>(2));
            } else if (request.operation == cxxime::LexiconOperation::kQueryCandidateOrder ||
                       request.operation == cxxime::LexiconOperation::kSetCandidateOrder ||
                       request.operation == cxxime::LexiconOperation::kClearCandidateOrder) {
                ASSERT_EQ(request.code, "yiy");
                result.candidate_order.input_code = request.code;
                result.candidate_order.version = request.expected_version + 1;
            }
            return cxxime::encode_lexicon_result(result, response_payload);
        },
        pipe_name));

    cxxime::LexiconControlClient client(3000, pipe_name);
    cxxime::LexiconControlResult result;
    ASSERT_TRUE(client.query(cxxime::UserDictKind::WUBI, "ni", 2, 3, &result));
    ASSERT_EQ(result.query.resource_total, static_cast<std::size_t>(9));
    ASSERT_EQ(result.query.match_total, static_cast<std::size_t>(4));
    ASSERT_TRUE(result.query.has_more);
    ASSERT_EQ(result.query.entries.size(), static_cast<std::size_t>(1));
    ASSERT_TRUE(result.query.entries[0].text == "你好");

    ASSERT_TRUE(client.add_entry(cxxime::UserDictKind::PINYIN, "你好", "nihao", &result));
    ASSERT_TRUE(client.replace_entry(cxxime::UserDictKind::PINYIN, "你好", "nihao", "您好",
                                     "ninhao", &result));
    ASSERT_TRUE(client.delete_entries(cxxime::UserDictKind::PINYIN,
                                      {{"您好", "ninhao"}, {"你们好", "nimenhao"}}, &result));
    ASSERT_TRUE(client.import_entries(cxxime::UserDictKind::PINYIN,
                                      "C:\\temp\\user_pinyin.tsv", &result));
    ASSERT_TRUE(client.save(cxxime::UserDictKind::PINYIN, &result));
    ASSERT_TRUE(client.query(cxxime::LexiconResource::kCandidatePreference,
                             cxxime::UserDictKind::WUBI, "ni", 2, 3, &result));
    ASSERT_TRUE(client.query(cxxime::LexiconResource::kDisabledSystemLexicon,
                             cxxime::UserDictKind::WUBI, "ni", 2, 3, &result));
    ASSERT_TRUE(client.query_exact_user_entries(cxxime::UserDictKind::WUBI, "你好", 16,
                                                &result));
    ASSERT_TRUE(client.delete_preferences(cxxime::UserDictKind::PINYIN,
                                          {{"你好", "nihao"}, {"您好", "ninhao"}}, &result));
    ASSERT_TRUE(client.clear_preferences(cxxime::UserDictKind::PINYIN, &result));
    ASSERT_TRUE(client.save_preferences(cxxime::UserDictKind::PINYIN, &result));
    ASSERT_TRUE(client.query_system_entry_status(cxxime::UserDictKind::PINYIN,
                                                 {"你好", "世界"}, &result));
    ASSERT_EQ(result.query.entries.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(result.query.entries[0].text, "你好");
    ASSERT_TRUE(client.disable_system_entry(cxxime::UserDictKind::PINYIN, "你好", &result));
    ASSERT_TRUE(client.restore_system_entry(cxxime::UserDictKind::PINYIN, "你好", &result));
    ASSERT_TRUE(client.query_candidate_order(cxxime::UserDictKind::WUBI, "yiy", 64, &result));
    ASSERT_TRUE(client.set_candidate_order(cxxime::UserDictKind::WUBI, "yiy",
                                           {{"应该", "yiyy", ""}}, 3, &result));
    ASSERT_TRUE(client.clear_candidate_order(cxxime::UserDictKind::WUBI, "yiy", 4, &result));
    ASSERT_EQ(request_count.load(), 18);
    server.stop();
}

TEST(ControlChannel, input_protocol_size_matches_native_layout) {
    cxxime::IPCRequest request = {};
    cxxime::IPCResponse response = {};
    ASSERT_EQ(cxxime::test::make_main_request_baseline(request).size(),
              static_cast<size_t>(cxxime::IPC_REQUEST_BASELINE_SIZE));
    ASSERT_EQ(cxxime::test::make_main_response_baseline(response).size(),
              static_cast<size_t>(cxxime::IPC_RESPONSE_BASELINE_SIZE));
}

RUN_ALL_TESTS()
