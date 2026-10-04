// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Synchronous named pipe IPC client.
// Design reference: weasel PipeChannel (WeaselIPC).

#include <cxxime/ipc_client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include <windows.h>

namespace cxxime {

namespace {

bool overlapped_io(HANDLE pipe, bool write, void* buffer, DWORD size,
                   DWORD timeout_ms, DWORD& bytes_transferred) {
    bytes_transferred = 0;

    OVERLAPPED ol = {};
    ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent)
        return false;

    BOOL ok = write
        ? WriteFile(pipe, buffer, size, nullptr, &ol)
        : ReadFile(pipe, buffer, size, nullptr, &ol);

    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ol.hEvent);
            return false;
        }

        DWORD wait = WaitForSingleObject(ol.hEvent, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ol);
            GetOverlappedResult(pipe, &ol, &bytes_transferred, TRUE);
            CloseHandle(ol.hEvent);
            return false;
        }
    }

    ok = GetOverlappedResult(pipe, &ol, &bytes_transferred, FALSE);
    CloseHandle(ol.hEvent);
    return ok != FALSE;
}

} // namespace

// ============================================================
// Lifecycle
// ============================================================
IpcClient::~IpcClient() {
    disconnect();
}

bool IpcClient::connect(const std::wstring& pipe_name, int timeout_ms) {
    disconnect();
    pipe_name_ = make_user_pipe_name(pipe_name);
    timeout_ms_ = timeout_ms;

    // Retry loop: WaitNamedPipeW returns TRUE for ALL waiting threads when
    // ONE pipe instance becomes available, but CreateFileW only succeeds for
    // one caller. On CreateFileW failure, re-enter the wait for the next instance.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            break;

        if (!WaitNamedPipeW(pipe_name_.c_str(), (DWORD)remaining))
            break;  // pipe not available within timeout

        pipe_handle_ = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe_handle_ != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            DWORD pipe_timeout = static_cast<DWORD>((std::max)(timeout_ms, 1));
            SetNamedPipeHandleState((HANDLE)pipe_handle_, &mode, nullptr, &pipe_timeout);
            return true;
        }
        // Instance consumed by another thread; retry WaitNamedPipeW for next instance.
    }

    pipe_handle_ = nullptr;
    return false;
}

bool IpcClient::try_reconnect() {
    disconnect();
    if (pipe_name_.empty())
        return false;
    return connect(pipe_name_, timeout_ms_);
}

void IpcClient::disconnect() {
    if (pipe_handle_) {
        CloseHandle((HANDLE)pipe_handle_);
        pipe_handle_ = nullptr;
    }
}

bool IpcClient::is_connected() const {
    return pipe_handle_ != nullptr;
}

bool IpcClient::ensure_connected() {
    return is_connected() || try_reconnect();
}

// ============================================================
// Core send/recv
// Reference: weasel PipeChannel::_Send / _ReceiveResponse
// ============================================================
bool IpcClient::send_request(const IPCRequest& request, IPCResponse& response) {
    auto start = std::chrono::steady_clock::now();

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!is_connected()) {
            if (!try_reconnect())
                return false;
        }

        HANDLE pipe = (HANDLE)pipe_handle_;

        IPCWireHeader request_header = {};
        request_header.payload_size = sizeof(IPCRequest);
        std::vector<uint8_t> request_wire(sizeof(request_header) + sizeof(request));
        memcpy(request_wire.data(), &request_header, sizeof(request_header));
        memcpy(request_wire.data() + sizeof(request_header), &request, sizeof(request));

        // No FlushFileBuffers needed for message-mode pipe.
        DWORD bytes_written = 0;
        if (!overlapped_io(pipe, true, request_wire.data(),
                           static_cast<DWORD>(request_wire.size()),
                           static_cast<DWORD>(timeout_ms_), bytes_written) ||
            bytes_written != static_cast<DWORD>(request_wire.size())) {
            disconnect();
            continue;
        }

        constexpr size_t kMaxResponseWire = 65536;
        std::vector<uint8_t> response_wire(kMaxResponseWire);
        DWORD bytes_read = 0;
        if (!overlapped_io(pipe, false, response_wire.data(),
                           static_cast<DWORD>(response_wire.size()),
                           static_cast<DWORD>(timeout_ms_), bytes_read)) {
            disconnect();
            continue;
        }

        response = {};
        if (bytes_read >= sizeof(IPCWireHeader) + IPC_RESPONSE_BASELINE_SIZE) {
            IPCWireHeader response_header = {};
            memcpy(&response_header, response_wire.data(), sizeof(response_header));
            if (response_header.magic != IPC_WIRE_MAGIC ||
                response_header.version < IPC_WIRE_MIN_COMPATIBLE_VERSION ||
                response_header.header_size < sizeof(IPCWireHeader) ||
                response_header.header_size > bytes_read ||
                response_header.payload_size < IPC_RESPONSE_BASELINE_SIZE ||
                response_header.payload_size > bytes_read - response_header.header_size ||
                bytes_read != response_header.header_size + response_header.payload_size) {
                disconnect();
                continue;
            }
            const size_t response_size =
                (std::min)(static_cast<size_t>(response_header.payload_size), sizeof(response));
            memcpy(&response, response_wire.data() + response_header.header_size, response_size);
            constexpr size_t kCandidateExtentPayloadSize =
                offsetof(IPCResponse, candidate_extent_complete) + sizeof(uint32_t);
            const uint64_t returned_end = static_cast<uint64_t>(response.candidate_offset) +
                                          response.candidate_count;
            if (returned_end > (std::numeric_limits<uint32_t>::max)()) {
                disconnect();
                continue;
            }
            if (response_header.payload_size < kCandidateExtentPayloadSize) {
                response.candidate_known_count = (std::max)(
                    response.candidate_total, static_cast<uint32_t>(returned_end));
                response.candidate_extent_state = response.candidate_total > returned_end
                        ? CandidateExtentState::kHasMore
                        : CandidateExtentState::kExhausted;
                response.candidate_extent_complete = 1;
            } else if ((response.candidate_extent_state != CandidateExtentState::kExhausted &&
                        response.candidate_extent_state != CandidateExtentState::kHasMore &&
                        response.candidate_extent_state != CandidateExtentState::kIndeterminate) ||
                       response.candidate_extent_complete > 1 ||
                       response.candidate_known_count < returned_end) {
                disconnect();
                continue;
            }
        } else {
            disconnect();
            continue;
        }

        // Record IPC round-trip time
        auto end = std::chrono::steady_clock::now();
        last_ipc_us_ = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        return true;
    }
    return false;
}

// ============================================================
// High-level commands
// ============================================================
bool IpcClient::start_session(uint32_t& session_id, uint64_t client_capabilities) {
    IPCRequest req = {};
    req.command = IPCCommand::START_SESSION;
    req.client_capabilities = client_capabilities;

    IPCResponse resp = {};
    if (!send_request(req, resp))
        return false;

    if (resp.status != IPCStatus::OK)
        return false;

    session_id = resp.highlighted;
    return true;
}

bool IpcClient::end_session(uint32_t session_id) {
    IPCRequest req = {};
    req.command = IPCCommand::END_SESSION;
    req.session_id = session_id;

    IPCResponse resp = {};
    return send_request(req, resp) && resp.status == IPCStatus::OK;
}

bool IpcClient::process_key(uint32_t session_id, uint32_t key_code, uint32_t modifiers,
                            IPCResponse& response, bool is_key_up,
                            CandidateUiContext candidate_ui) {
    IPCRequest req = {};
    req.command = IPCCommand::PROCESS_KEY;
    req.session_id = session_id;
    req.key_code = key_code;
    req.modifiers = modifiers;
    req.is_key_up = is_key_up;
    req.candidate_ui = candidate_ui;
    return send_request(req, response);
}

bool IpcClient::select_candidate(uint32_t session_id, int index, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::SELECT_CANDIDATE;
    req.session_id = session_id;
    req.candidate_index = static_cast<uint32_t>(index);
    return send_request(req, response) && response.status == IPCStatus::OK;
}

CandidateSelectionCallResult IpcClient::select_candidate_with_revision(
    uint32_t session_id, int index, uint64_t candidate_revision) {
    IPCRequest request = {};
    request.command = IPCCommand::SELECT_CANDIDATE;
    request.session_id = session_id;
    request.candidate_index = static_cast<uint32_t>(index);
    request.candidate_revision = candidate_revision;

    CandidateSelectionCallResult result;
    result.transport_success = send_request(request, result.response);
    if (result.transport_success) {
        result.status = result.response.status;
    }
    return result;
}

bool IpcClient::commit_composition(uint32_t session_id, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::COMMIT_COMPOSITION;
    req.session_id = session_id;
    return send_request(req, response) && response.status == IPCStatus::OK;
}

bool IpcClient::clear_composition(uint32_t session_id) {
    IPCRequest req = {};
    req.command = IPCCommand::CLEAR_COMPOSITION;
    req.session_id = session_id;

    IPCResponse resp = {};
    return send_request(req, resp) && resp.status == IPCStatus::OK;
}

bool IpcClient::focus_in(uint32_t session_id) {
    IPCRequest req = {};
    req.command = IPCCommand::FOCUS_IN;
    req.session_id = session_id;

    IPCResponse resp = {};
    return send_request(req, resp);
}

bool IpcClient::set_input_target(uint32_t session_id, const std::string& app,
                                 const std::string& window_title) {
    IPCRequest req = {};
    req.command = IPCCommand::SET_INPUT_TARGET;
    req.session_id = session_id;
    // Truncated at a UTF-8 character boundary, NUL-terminated.
    auto copy = [](char* dst, size_t size, const std::string& src) {
        size_t n = (std::min)(src.size(), size - 1);
        while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
            --n;
        }
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };
    copy(req.search_query, sizeof(req.search_query), app);
    copy(req.search_result, sizeof(req.search_result), window_title);
    IPCResponse resp = {};
    return send_request(req, resp);
}

bool IpcClient::focus_out(uint32_t session_id) {
    IPCRequest req = {};
    req.command = IPCCommand::FOCUS_OUT;
    req.session_id = session_id;

    IPCResponse resp = {};
    return send_request(req, resp);
}

bool IpcClient::toggle_chinese(uint32_t session_id, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::TOGGLE_CHINESE;
    req.session_id = session_id;
    return send_request(req, response);
}

bool IpcClient::set_chinese_mode(uint32_t session_id, bool chinese_mode,
                                  IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::SET_CHINESE_MODE;
    req.session_id = session_id;
    req.candidate_index = chinese_mode ? 1u : 0u;
    return send_request(req, response);
}

bool IpcClient::toggle_shape(uint32_t session_id, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::TOGGLE_SHAPE;
    req.session_id = session_id;
    return send_request(req, response);
}

bool IpcClient::toggle_punct(uint32_t session_id, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::TOGGLE_PUNCT;
    req.session_id = session_id;
    return send_request(req, response);
}

bool IpcClient::switch_input_mode(uint32_t session_id, InputMode mode, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::SWITCH_INPUT_MODE;
    req.session_id = session_id;
    req.candidate_index = static_cast<uint32_t>(mode);
    return send_request(req, response);
}

bool IpcClient::get_status(uint32_t session_id, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::GET_STATUS;
    req.session_id = session_id;
    return send_request(req, response);
}

bool IpcClient::sync_caps_lock(uint32_t session_id, bool caps_lock, IPCResponse& response) {
    IPCRequest req = {};
    req.command = IPCCommand::SYNC_CAPS_LOCK;
    req.session_id = session_id;
    if (caps_lock)
        req.modifiers |= 0x08;
    return send_request(req, response);
}

bool IpcClient::ping(IPCResponse* response) {
    IPCRequest req = {};
    req.command = IPCCommand::PING;

    IPCResponse local = {};
    IPCResponse& resp = response ? *response : local;
    return send_request(req, resp) && resp.status == IPCStatus::OK;
}

bool IpcClient::search_candidates(const std::string& query, IPCResponse& response) {
    if (query.size() >= sizeof(IPCRequest::search_query)) {
        return false;
    }

    IPCRequest req = {};
    req.command = IPCCommand::SEARCH_CANDIDATES;
    memcpy(req.search_query, query.data(), query.size());
    req.search_query[query.size()] = '\0';
    return send_request(req, response) && response.status == IPCStatus::OK;
}

bool IpcClient::set_search_result(const std::string& query, const std::string& result) {
    if (query.size() >= sizeof(IPCRequest::search_query) ||
        result.size() >= sizeof(IPCRequest::search_result)) {
        return false;
    }

    IPCRequest req = {};
    req.command = IPCCommand::SEARCH_CANDIDATE_RESULT;
    memcpy(req.search_query, query.data(), query.size());
    memcpy(req.search_result, result.data(), result.size());
    IPCResponse response = {};
    return send_request(req, response) && response.status == IPCStatus::OK;
}

bool IpcClient::open_settings(uint32_t session_id, SettingsPanel panel) {
    IPCRequest request = {};
    request.command = IPCCommand::OPEN_SETTINGS;
    request.session_id = session_id;
    request.candidate_index = static_cast<uint32_t>(panel);

    IPCResponse response = {};
    return send_request(request, response) && response.status == IPCStatus::OK;
}

} // namespace cxxime
