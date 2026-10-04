// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <cxxime/ipc_client.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/ipc_server.h>
#include <cxxime/pipe_names.h>

#include "support/ipc_baseline_fixture.h"
#include "support/testutil.h"

// ============================================================
// Helper
// ============================================================
static const std::wstring& test_pipe_name() {
    static const std::wstring pipe_name =
        L"\\\\.\\pipe\\ZhiyiIME-Test-" + std::to_wstring(GetCurrentProcessId());
    return pipe_name;
}

struct TestServer {
    cxxime::IpcServer server;
    bool start(cxxime::IpcServer::RequestHandler h) {
        server.set_handler(std::move(h));
        bool ok = server.start(test_pipe_name());
        if (ok) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return ok;
    }
    ~TestServer() { server.stop(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
};

static cxxime::IPCResponse make_response(cxxime::IPCStatus status) {
    cxxime::IPCResponse resp = {};
    resp.status = status;
    return resp;
}

static bool raw_round_trip(const std::vector<uint8_t>& request,
                           std::vector<uint8_t>* response) {
    const std::wstring pipe_name = cxxime::make_user_pipe_name(test_pipe_name());
    if (!WaitNamedPipeW(pipe_name.c_str(), 2000)) {
        return false;
    }
    HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD transferred = 0;
    bool ok = WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &transferred,
                        nullptr) != FALSE &&
              transferred == static_cast<DWORD>(request.size());
    if (ok) {
        response->resize(65536);
        ok = ReadFile(pipe, response->data(), static_cast<DWORD>(response->size()), &transferred,
                      nullptr) != FALSE;
        response->resize(ok ? transferred : 0);
    }
    CloseHandle(pipe);
    return ok;
}

// ============================================================
// Protocol Tests
// ============================================================

TEST(Protocol, pipe_names_are_scoped_once_per_user) {
    ASSERT_TRUE(wcscmp(cxxime::IPC_PIPE_BASE_NAME, L"\\\\.\\pipe\\ZhiyiIME") == 0);
    ASSERT_TRUE(wcscmp(cxxime::CONTROL_PIPE_BASE_NAME, L"\\\\.\\pipe\\ZhiyiIME-Control") == 0);
    const std::wstring input = cxxime::make_user_pipe_name(cxxime::IPC_PIPE_BASE_NAME);
    const std::wstring control = cxxime::make_user_pipe_name(cxxime::CONTROL_PIPE_BASE_NAME);

    ASSERT_TRUE(input.size() >= 8 && input.substr(input.size() - 8) == L"ZhiyiIME");
    ASSERT_TRUE(control.size() >= 16 && control.substr(control.size() - 16) == L"ZhiyiIME-Control");
    ASSERT_TRUE(input != control);
    ASSERT_TRUE(cxxime::make_user_pipe_name(input) == input);
    ASSERT_TRUE(cxxime::make_user_pipe_name(L"relative-name") == L"relative-name");
}

TEST(Protocol, wire_layout_preserves_the_0_4_prefix) {
    ASSERT_EQ(cxxime::IPC_REQUEST_BASELINE_SIZE,
              static_cast<uint32_t>(cxxime::test::MainRequestBaseline{}.size()));
    ASSERT_EQ(cxxime::IPC_RESPONSE_BASELINE_SIZE,
              static_cast<uint32_t>(cxxime::test::MainResponseBaseline{}.size()));
    cxxime::IPCWireHeader header;
    ASSERT_EQ(sizeof(header), static_cast<size_t>(12));
    ASSERT_EQ(header.magic, cxxime::IPC_WIRE_MAGIC);
    ASSERT_EQ(cxxime::IPC_WIRE_MIN_COMPATIBLE_VERSION, static_cast<uint16_t>(1));
    ASSERT_TRUE(cxxime::IPC_WIRE_VERSION >= cxxime::IPC_WIRE_MIN_COMPATIBLE_VERSION);
    ASSERT_EQ(header.version, cxxime::IPC_WIRE_VERSION);
    ASSERT_EQ(header.header_size, static_cast<uint16_t>(sizeof(header)));
}

TEST(Protocol, server_accepts_future_append_only_request) {
    TestServer server;
    ASSERT_TRUE(server.start([](const cxxime::IPCRequest& request) {
        return make_response(request.command == cxxime::IPCCommand::PING
                                 ? cxxime::IPCStatus::OK
                                 : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND);
    }));

    cxxime::IPCRequest request = {};
    request.command = cxxime::IPCCommand::PING;
    cxxime::IPCWireHeader header;
    header.version = cxxime::IPC_WIRE_VERSION + 1;
    header.payload_size = sizeof(request) + 16;
    std::vector<uint8_t> wire(sizeof(header) + header.payload_size, 0);
    memcpy(wire.data(), &header, sizeof(header));
    memcpy(wire.data() + sizeof(header), &request, sizeof(request));

    std::vector<uint8_t> response_wire;
    ASSERT_TRUE(raw_round_trip(wire, &response_wire));
    ASSERT_EQ(response_wire.size(), sizeof(cxxime::IPCWireHeader) + sizeof(cxxime::IPCResponse));
    cxxime::IPCWireHeader response_header = {};
    memcpy(&response_header, response_wire.data(), sizeof(response_header));
    ASSERT_EQ(response_header.magic, cxxime::IPC_WIRE_MAGIC);
    cxxime::IPCResponse response = {};
    memcpy(&response, response_wire.data() + response_header.header_size, sizeof(response));
    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
}

TEST(Protocol, server_accepts_baseline_request_prefix) {
    uint64_t client_capabilities = 1;
    uint64_t candidate_revision = 1;
    TestServer server;
    ASSERT_TRUE(server.start([&](const cxxime::IPCRequest& request) {
        client_capabilities = request.client_capabilities;
        candidate_revision = request.candidate_revision;
        return make_response(request.command == cxxime::IPCCommand::PING
                                 ? cxxime::IPCStatus::OK
                                 : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND);
    }));

    cxxime::IPCRequest request = {};
    request.command = cxxime::IPCCommand::PING;
    const auto payload = cxxime::test::make_main_request_baseline(request);
    cxxime::IPCWireHeader header;
    header.version = cxxime::IPC_WIRE_MIN_COMPATIBLE_VERSION;
    header.payload_size = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> wire(sizeof(header) + header.payload_size, 0);
    memcpy(wire.data(), &header, sizeof(header));
    memcpy(wire.data() + sizeof(header), payload.data(), payload.size());

    std::vector<uint8_t> response_wire;
    ASSERT_TRUE(raw_round_trip(wire, &response_wire));
    ASSERT_EQ(client_capabilities, 0u);
    ASSERT_EQ(candidate_revision, 0u);
}

TEST(Protocol, unknown_command_keeps_the_connection_usable) {
    TestServer server;
    ASSERT_TRUE(server.start([](const cxxime::IPCRequest& request) {
        return make_response(request.command == cxxime::IPCCommand::PING
                                 ? cxxime::IPCStatus::OK
                                 : cxxime::IPCStatus::ERR_UNKNOWN_COMMAND);
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCRequest request = {};
    request.command = static_cast<cxxime::IPCCommand>(999);
    cxxime::IPCResponse response = {};
    ASSERT_TRUE(client.send_request(request, response));
    ASSERT_EQ(response.status, cxxime::IPCStatus::ERR_UNKNOWN_COMMAND);

    request = {};
    request.command = cxxime::IPCCommand::PING;
    response = {};
    ASSERT_TRUE(client.send_request(request, response));
    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
}

TEST(Protocol, disconnect_reports_only_sessions_without_end_session) {
    HANDLE disconnected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_TRUE(disconnected != nullptr);
    uint32_t disconnected_session = 0;
    uint32_t next_session = 40;
    TestServer server;
    server.server.set_disconnect_handler([&](uint32_t session_id) {
        disconnected_session = session_id;
        SetEvent(disconnected);
    });
    ASSERT_TRUE(server.start([&](const cxxime::IPCRequest& request) {
        cxxime::IPCResponse response = {};
        if (request.command == cxxime::IPCCommand::START_SESSION) {
            response.highlighted = ++next_session;
        }
        return response;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    uint32_t ended_session = 0;
    uint32_t abandoned_session = 0;
    ASSERT_TRUE(client.start_session(ended_session));
    ASSERT_TRUE(client.start_session(abandoned_session));
    ASSERT_TRUE(client.end_session(ended_session));
    client.disconnect();

    ASSERT_EQ(WaitForSingleObject(disconnected, 2000), WAIT_OBJECT_0);
    ASSERT_EQ(disconnected_session, abandoned_session);
    CloseHandle(disconnected);
}

TEST(Protocol, server_rejects_unframed_request) {
    TestServer server;
    ASSERT_TRUE(server.start(
        [](const cxxime::IPCRequest&) { return make_response(cxxime::IPCStatus::OK); }));
    cxxime::IPCRequest request = {};
    request.command = cxxime::IPCCommand::PING;
    std::vector<uint8_t> wire(sizeof(request));
    memcpy(wire.data(), &request, sizeof(request));
    std::vector<uint8_t> response_wire;
    ASSERT_TRUE(!raw_round_trip(wire, &response_wire));
}

TEST(Protocol, candidate_ui_visible_count_distinguishes_unknown_server_layout) {
    cxxime::CandidateUiContext context;
    context.presenter = cxxime::CandidateUiContext::Presenter::SERVER;
    ASSERT_EQ(cxxime::candidate_ui_visible_count(context, 0), 1u);
    ASSERT_EQ(cxxime::candidate_ui_visible_count(context, 3), 3u);

    context.presenter = cxxime::CandidateUiContext::Presenter::LOCAL;
    context.local_visible_candidate_count = 2;
    ASSERT_EQ(cxxime::candidate_ui_visible_count(context, 0), 2u);

    context.presenter = cxxime::CandidateUiContext::Presenter::HOST;
    ASSERT_EQ(cxxime::candidate_ui_visible_count(context, 0), 0u);
}

TEST(Protocol, large_candidate_text_round_trips) {
    const std::string candidate(200, 'x');
    TestServer server;
    ASSERT_TRUE(server.start([&](const cxxime::IPCRequest&) {
        cxxime::IPCResponse response = {};
        response.candidate_count = 1;
        response.candidate_known_count = 1;
        strcpy_s(response.candidates[0], candidate.c_str());
        return response;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCRequest request = {};
    request.command = cxxime::IPCCommand::PING;
    cxxime::IPCResponse response = {};
    ASSERT_TRUE(client.send_request(request, response));
    ASSERT_TRUE(std::string(response.candidates[0]) == candidate);
}

TEST(Protocol, ime_status_flags_are_independent) {
    cxxime::ImeStatus status;
    ASSERT_TRUE(status.chinese_mode());
    ASSERT_TRUE(!status.caps_lock());
    ASSERT_TRUE(!status.full_shape());
    ASSERT_TRUE(status.chinese_punct());
    ASSERT_TRUE(status.english_words());

    status.set_chinese_mode(false);
    status.set_caps_lock(true);
    status.set_full_shape(true);
    status.set_chinese_punct(false);
    status.set_english_words(false);

    ASSERT_TRUE(!status.chinese_mode());
    ASSERT_TRUE(status.caps_lock());
    ASSERT_TRUE(status.full_shape());
    ASSERT_TRUE(!status.chinese_punct());
    ASSERT_TRUE(!status.english_words());
    ASSERT_EQ(status.flags,
              cxxime::ime_status_flag(cxxime::ImeStatusFlag::CAPS_LOCK) |
              cxxime::ime_status_flag(cxxime::ImeStatusFlag::FULL_SHAPE));
}

// ============================================================
// Server Lifecycle Tests
// ============================================================

TEST(Server, start_and_repeated_stop_complete_cleanly) {
    cxxime::IpcServer server;
    server.set_handler([](const cxxime::IPCRequest&) -> cxxime::IPCResponse { return {}; });
    ASSERT_TRUE(server.start(test_pipe_name()));
    server.stop();
    server.stop();
}

// ============================================================
// Client Connection Tests
// ============================================================

TEST(Client, connect_no_server) {
    cxxime::IpcClient client;
    ASSERT_TRUE(!client.connect(test_pipe_name(), 300));
    ASSERT_TRUE(!client.is_connected());
}

TEST(Client, connect_and_repeated_disconnect_update_connection_state) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest&) -> cxxime::IPCResponse { return {}; }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    ASSERT_TRUE(client.is_connected());
    client.disconnect();
    ASSERT_TRUE(!client.is_connected());
    client.disconnect();
}

// ============================================================
// IPC Command Tests
// ============================================================

TEST(IPC, session_lifecycle_and_ping_round_trip) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::START_SESSION) {
            resp.highlighted = 42;
        } else if (req.command != cxxime::IPCCommand::END_SESSION &&
                   req.command != cxxime::IPCCommand::PING) {
            resp.status = cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    uint32_t sid = 0;
    ASSERT_TRUE(client.start_session(sid));
    ASSERT_EQ(sid, (uint32_t)42);
    ASSERT_TRUE(client.end_session(sid));
    ASSERT_TRUE(client.ping());
}

TEST(IPC, process_key_preedit) {
    TestServer ts;
    cxxime::CandidateUiContext candidate_ui;
    ASSERT_TRUE(ts.start([&](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::PROCESS_KEY) {
            candidate_ui = req.candidate_ui;
            resp.status = cxxime::IPCStatus::OK;
            strncpy_s(resp.preedit, "ni", sizeof(resp.preedit) - 1);
            resp.preedit_cursor = 1;
            resp.candidate_count = 2;
            resp.candidate_offset = 4;
            resp.candidate_total = 12;
            resp.candidate_known_count = 12;
            resp.candidate_extent_state = cxxime::CandidateExtentState::kHasMore;
            strncpy_s(resp.candidates[0], "你", sizeof(resp.candidates[0]) - 1);
            strncpy_s(resp.candidates[1], "尼", sizeof(resp.candidates[1]) - 1);
            strncpy_s(resp.candidate_hints[1], "a", sizeof(resp.candidate_hints[1]) - 1);
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    cxxime::CandidateUiContext expected_ui;
    expected_ui.session_generation = 3;
    expected_ui.target_generation = 4;
    expected_ui.composition_generation = 5;
    expected_ui.presentation_generation = 6;
    expected_ui.local_visible_candidate_count = 2;
    expected_ui.presenter = cxxime::CandidateUiContext::Presenter::LOCAL;
    ASSERT_TRUE(client.process_key(1, 'N', 0, resp, false, expected_ui));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(candidate_ui.session_generation, expected_ui.session_generation);
    ASSERT_EQ(candidate_ui.target_generation, expected_ui.target_generation);
    ASSERT_EQ(candidate_ui.composition_generation, expected_ui.composition_generation);
    ASSERT_EQ(candidate_ui.presentation_generation, expected_ui.presentation_generation);
    ASSERT_EQ(candidate_ui.local_visible_candidate_count, 2u);
    ASSERT_EQ(candidate_ui.presenter, cxxime::CandidateUiContext::Presenter::LOCAL);
    ASSERT_EQ(strcmp(resp.preedit, "ni"), 0);
    ASSERT_EQ(resp.preedit_cursor, 1u);
    ASSERT_EQ(resp.candidate_count, (uint32_t)2);
    ASSERT_EQ(resp.candidate_offset, 4u);
    ASSERT_EQ(resp.candidate_total, 12u);
    ASSERT_EQ(strcmp(resp.candidate_hints[0], ""), 0);
    ASSERT_EQ(strcmp(resp.candidate_hints[1], "a"), 0);
}

TEST(IPC, request_timeout_disconnects_client) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest&) -> cxxime::IPCResponse {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return make_response(cxxime::IPCStatus::OK);
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 100));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(!client.ping(&resp));
    ASSERT_TRUE(!client.is_connected());
}

TEST(IPC, process_key_commit) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::PROCESS_KEY) {
            resp.status = cxxime::IPCStatus::OK;
            strncpy_s(resp.commit_text, "你", sizeof(resp.commit_text) - 1);
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.process_key(1, '1', 0, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(strcmp(resp.commit_text, "你"), 0);
}

TEST(IPC, process_key_rejected) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::PROCESS_KEY) resp.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.process_key(1, VK_RETURN, 0, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
}

TEST(IPC, select_candidate) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::SELECT_CANDIDATE) {
            resp.status = cxxime::IPCStatus::OK;
            strncpy_s(resp.commit_text, "你好", sizeof(resp.commit_text) - 1);
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.select_candidate(1, 0, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
}

TEST(IPC, commit_composition) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::COMMIT_COMPOSITION) {
            resp.status = cxxime::IPCStatus::OK;
            strncpy_s(resp.commit_text, "测试", sizeof(resp.commit_text) - 1);
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.commit_composition(1, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
}

TEST(IPC, set_chinese_mode_uses_explicit_target) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        ASSERT_EQ(req.command, cxxime::IPCCommand::SET_CHINESE_MODE);
        ASSERT_EQ(req.session_id, static_cast<uint32_t>(7));
        ASSERT_EQ(req.candidate_index, static_cast<uint32_t>(0));
        resp.status = cxxime::IPCStatus::OK;
        resp.ime_status.set_chinese_mode(false);
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.set_chinese_mode(7, false, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(resp.ime_status.chinese_mode(), false);
}

TEST(IPC, switch_input_mode_carries_target) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        ASSERT_EQ(req.command, cxxime::IPCCommand::SWITCH_INPUT_MODE);
        ASSERT_EQ(req.session_id, static_cast<uint32_t>(7));
        ASSERT_EQ(req.candidate_index, static_cast<uint32_t>(cxxime::InputMode::WUBI));
        resp.status = cxxime::IPCStatus::OK;
        resp.ime_status.input_mode = cxxime::InputMode::WUBI;
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.switch_input_mode(7, cxxime::InputMode::WUBI, resp));
    ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(resp.ime_status.input_mode, cxxime::InputMode::WUBI);
}

TEST(IPC, open_settings_carries_session_and_panel) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        ASSERT_EQ(req.command, cxxime::IPCCommand::OPEN_SETTINGS);
        ASSERT_EQ(req.session_id, static_cast<uint32_t>(7));
        ASSERT_EQ(req.candidate_index,
                  static_cast<uint32_t>(cxxime::SettingsPanel::kDictionary));
        resp.status = cxxime::IPCStatus::OK;
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
    ASSERT_TRUE(client.open_settings(7, cxxime::SettingsPanel::kDictionary));
}

TEST(IPC, send_request) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::CLEAR_COMPOSITION) resp.status = static_cast<cxxime::IPCStatus>(99);
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));

    cxxime::IPCRequest req = {};
    req.command = cxxime::IPCCommand::CLEAR_COMPOSITION;
    req.session_id = 1;
    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.send_request(req, resp));
    ASSERT_EQ(resp.status, static_cast<cxxime::IPCStatus>(99));
}

// ============================================================
// Multi-Client Tests
// ============================================================

TEST(MultiClient, two_clients_simultaneous) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        resp.status = static_cast<cxxime::IPCStatus>(req.session_id * 10);
        return resp;
    }));

    cxxime::IpcClient client1;
    cxxime::IpcClient client2;
    ASSERT_TRUE(client1.connect(test_pipe_name(), 2000));
    ASSERT_TRUE(client2.connect(test_pipe_name(), 2000));

    cxxime::IPCResponse resp1 = {};
    cxxime::IPCResponse resp2 = {};
    ASSERT_TRUE(client1.process_key(1, 'A', 0, resp1));
    ASSERT_TRUE(client2.process_key(2, 'B', 0, resp2));
    ASSERT_EQ(resp1.status, static_cast<cxxime::IPCStatus>(10));
    ASSERT_EQ(resp2.status, static_cast<cxxime::IPCStatus>(20));

    client1.disconnect();
    client2.disconnect();
}

TEST(MultiClient, sequential_sessions) {
    // Ensure previous test's server is fully cleaned up
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    cxxime::IpcServer server;
    int call_count = 0;
    server.set_handler([&](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::START_SESSION) {
            ++call_count;
            resp.status = cxxime::IPCStatus::OK;
            resp.highlighted = static_cast<uint32_t>(call_count);
        } else {
            resp.status = cxxime::IPCStatus::OK;
        }
        return resp;
    });
    ASSERT_TRUE(server.start(test_pipe_name()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));

    uint32_t sid1 = 0;
    ASSERT_TRUE(client.start_session(sid1));
    ASSERT_EQ(sid1, (uint32_t)1);
    ASSERT_TRUE(client.end_session(sid1));

    uint32_t sid2 = 0;
    ASSERT_TRUE(client.start_session(sid2));
    ASSERT_EQ(sid2, (uint32_t)2);
    ASSERT_TRUE(client.end_session(sid2));

    client.disconnect();
    server.stop();
}

// ============================================================
// Reconnection Tests
// ============================================================

TEST(Reconnect, server_restart) {
    {
        TestServer ts;
        ASSERT_TRUE(ts.start([](const cxxime::IPCRequest&) -> cxxime::IPCResponse {
            return make_response(cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
        }));
        cxxime::IpcClient client;
        ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
        cxxime::IPCResponse resp = {};
        ASSERT_TRUE(client.process_key(1, 'A', 0, resp));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        TestServer ts;
        ASSERT_TRUE(ts.start([](const cxxime::IPCRequest&) -> cxxime::IPCResponse {
            return make_response(cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
        }));
        cxxime::IpcClient client;
        ASSERT_TRUE(client.connect(test_pipe_name(), 2000));
        cxxime::IPCResponse resp = {};
        ASSERT_TRUE(client.process_key(1, 'A', 0, resp));
    }
}

// ============================================================
// Error Handling Tests
// ============================================================

TEST(Error, invalid_session) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::PROCESS_KEY && req.session_id == 0) {
            resp.status = cxxime::IPCStatus::ERR_INVALID_SESSION;
        } else {
            resp.status = cxxime::IPCStatus::OK;
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));

    cxxime::IPCResponse resp = {};
    ASSERT_TRUE(client.process_key(0, 'A', 0, resp));  // session_id=0 is invalid
    ASSERT_EQ(resp.status, cxxime::IPCStatus::ERR_INVALID_SESSION);
    client.disconnect();
}

TEST(Error, engine_not_initialized) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        if (req.command == cxxime::IPCCommand::START_SESSION) {
            resp.status = cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
            resp.highlighted = 0;
        }
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));

    uint32_t sid = 0;
    ASSERT_TRUE(!client.start_session(sid));
    ASSERT_EQ(sid, (uint32_t)0);
    client.disconnect();
}

// ============================================================
// Stress Tests
// ============================================================

TEST(Stress, rapid_requests) {
    TestServer ts;
    ASSERT_TRUE(ts.start([](const cxxime::IPCRequest& req) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        resp.status = cxxime::IPCStatus::OK;
        return resp;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(test_pipe_name(), 2000));

    // Send 200 rapid requests
    for (int i = 0; i < 200; ++i) {
        cxxime::IPCResponse resp = {};
        ASSERT_TRUE(client.process_key(1, 'A', 0, resp));
        ASSERT_EQ(resp.status, cxxime::IPCStatus::OK);
    }

    client.disconnect();
}

TEST(Stress, concurrent_clients) {
    constexpr int kClientCount = 3;
    constexpr int kRequestsPerClient = 50;
    constexpr int kExpectedRequests = kClientCount * kRequestsPerClient;
    TestServer ts;
    std::atomic<int> total_handled{0};
    ASSERT_TRUE(ts.start([&](const cxxime::IPCRequest&) -> cxxime::IPCResponse {
        cxxime::IPCResponse resp = {};
        resp.status = cxxime::IPCStatus::OK;
        total_handled.fetch_add(1);
        return resp;
    }));

    std::atomic<int> total_sent{0};
    std::atomic<int> total_failed{0};
    auto client_func = [&](int id) {
        cxxime::IpcClient client;
        if (!client.connect(test_pipe_name(), 5000)) {
            total_failed.fetch_add(kRequestsPerClient);
            return;
        }
        for (int i = 0; i < kRequestsPerClient; ++i) {
            cxxime::IPCResponse resp = {};
            if (client.process_key(static_cast<uint32_t>(id), 'A', 0, resp) &&
                resp.status == cxxime::IPCStatus::OK) {
                total_sent.fetch_add(1);
            } else {
                total_failed.fetch_add(1);
            }
        }
        client.disconnect();
    };

    std::thread t1(client_func, 1);
    std::thread t2(client_func, 2);
    std::thread t3(client_func, 3);
    t1.join();
    t2.join();
    t3.join();

    ASSERT_EQ(total_failed.load(), 0);
    ASSERT_EQ(total_sent.load(), kExpectedRequests);
    ASSERT_EQ(total_handled.load(), kExpectedRequests);
}

RUN_ALL_TESTS()
