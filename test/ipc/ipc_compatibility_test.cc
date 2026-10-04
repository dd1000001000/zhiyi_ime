// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <chrono>
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

namespace {

const std::wstring& compatibility_pipe_name() {
    static const std::wstring pipe_name =
        L"\\\\.\\pipe\\ZhiyiIME-Compatibility-" + std::to_wstring(GetCurrentProcessId());
    return pipe_name;
}

class ScopedIpcServer {
public:
    bool start(cxxime::IpcServer::RequestHandler handler) {
        server_.set_handler(std::move(handler));
        if (!server_.start(compatibility_pipe_name())) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return true;
    }

    ~ScopedIpcServer() {
        server_.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

private:
    cxxime::IpcServer server_;
};

class BaselineResponseServer {
public:
    bool start() {
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready_) {
            return false;
        }
        worker_ = std::thread([this] { run(); });
        return WaitForSingleObject(ready_, 2000) == WAIT_OBJECT_0;
    }

    ~BaselineResponseServer() {
        if (worker_.joinable()) {
            worker_.join();
        }
        if (ready_) {
            CloseHandle(ready_);
        }
    }

private:
    void run() {
        const std::wstring pipe_name = cxxime::make_user_pipe_name(compatibility_pipe_name());
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                       65536, 65536, 0, nullptr);
        SetEvent(ready_);
        if (pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        const BOOL connected =
            ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            std::vector<unsigned char> request_wire(65536);
            DWORD request_size = 0;
            if (ReadFile(pipe, request_wire.data(), static_cast<DWORD>(request_wire.size()),
                         &request_size, nullptr)) {
                cxxime::IPCResponse response = {};
                response.status = cxxime::IPCStatus::OK;
                response.highlighted = 7;
                response.candidate_count = 1;
                response.candidate_total = 2;
                response.candidate_revision = 99;
                const auto payload = cxxime::test::make_main_response_baseline(response);
                cxxime::IPCWireHeader header;
                header.payload_size = static_cast<uint32_t>(payload.size());
                std::vector<unsigned char> response_wire(sizeof(header) + payload.size());
                memcpy(response_wire.data(), &header, sizeof(header));
                memcpy(response_wire.data() + sizeof(header), payload.data(), payload.size());
                DWORD written = 0;
                WriteFile(pipe, response_wire.data(), static_cast<DWORD>(response_wire.size()),
                          &written, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    HANDLE ready_ = nullptr;
    std::thread worker_;
};

} // namespace

TEST(IpcCompatibility, start_session_sends_segmented_selection_capability) {
    uint64_t received_capabilities = 0;
    ScopedIpcServer server;
    ASSERT_TRUE(server.start([&](const cxxime::IPCRequest& request) {
        cxxime::IPCResponse response = {};
        received_capabilities = request.client_capabilities;
        response.highlighted = 42;
        return response;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(compatibility_pipe_name(), 2000));
    uint32_t session_id = 0;
    ASSERT_TRUE(client.start_session(session_id, cxxime::kClientCapabilitySegmentedSelection));
    ASSERT_EQ(session_id, 42u);
    ASSERT_EQ(received_capabilities, cxxime::kClientCapabilitySegmentedSelection);
}

TEST(IpcCompatibility, candidate_selection_result_separates_transport_and_status) {
    uint64_t received_revision = 0;
    ScopedIpcServer server;
    ASSERT_TRUE(server.start([&](const cxxime::IPCRequest& request) {
        cxxime::IPCResponse response = {};
        received_revision = request.candidate_revision;
        response.status = cxxime::IPCStatus::ERR_STALE_CANDIDATE;
        response.candidate_revision = 12;
        response.composing = 1;
        strcpy_s(response.preedit, "huaruijishu");
        return response;
    }));

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(compatibility_pipe_name(), 2000));
    const cxxime::CandidateSelectionCallResult result =
        client.select_candidate_with_revision(3, 1, 11);
    ASSERT_TRUE(result.transport_success);
    ASSERT_EQ(result.status, cxxime::IPCStatus::ERR_STALE_CANDIDATE);
    ASSERT_EQ(result.response.candidate_revision, 12u);
    ASSERT_EQ(std::string(result.response.preedit), "huaruijishu");
    ASSERT_EQ(received_revision, 11u);
}

TEST(IpcCompatibility, candidate_selection_reports_transport_failure_separately) {
    cxxime::IpcClient client;
    const cxxime::CandidateSelectionCallResult result =
        client.select_candidate_with_revision(3, 1, 11);
    ASSERT_TRUE(!result.transport_success);
    ASSERT_EQ(result.status, cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
}

TEST(IpcCompatibility, current_client_accepts_0_4_response_prefix) {
    BaselineResponseServer server;
    ASSERT_TRUE(server.start());

    cxxime::IpcClient client;
    ASSERT_TRUE(client.connect(compatibility_pipe_name(), 2000));
    cxxime::IPCRequest request = {};
    request.command = cxxime::IPCCommand::START_SESSION;
    request.client_capabilities = cxxime::kClientCapabilitySegmentedSelection;
    cxxime::IPCResponse response = {};
    ASSERT_TRUE(client.send_request(request, response));
    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(response.highlighted, 7u);
    ASSERT_EQ(response.candidate_revision, 0u);
    ASSERT_EQ(response.converted_prefix_bytes, 0u);
    ASSERT_EQ(response.focused_preedit_start_bytes, 0u);
    ASSERT_EQ(response.focused_preedit_end_bytes, 0u);
    ASSERT_EQ(response.preedit_presentation_flags, 0u);
    ASSERT_EQ(response.candidate_known_count, response.candidate_total);
    ASSERT_EQ(response.candidate_extent_state, cxxime::CandidateExtentState::kHasMore);
    ASSERT_EQ(response.candidate_extent_complete, 1u);
}
