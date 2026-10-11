// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/control_client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <cxxime/logging.h>
#include <cxxime/pipe_names.h>

namespace cxxime {
namespace {

constexpr DWORD kConnectSliceMs = 50;
constexpr DWORD kIoTimeoutMs = 2000;

HANDLE connect_pipe(const std::wstring& pipe_name, int timeout_ms, HANDLE stop_event = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    DWORD last_error = ERROR_SEM_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        DWORD wait_ms = static_cast<DWORD>(
            (std::min)(remaining, static_cast<decltype(remaining)>(kConnectSliceMs)));
        if (!WaitNamedPipeW(pipe_name.c_str(), wait_ms)) {
            last_error = GetLastError();
            if (last_error == ERROR_FILE_NOT_FOUND) {
                DWORD retry_ms = (std::max)(1UL, (std::min)(wait_ms, 10UL));
                if (stop_event) {
                    if (WaitForSingleObject(stop_event, retry_ms) == WAIT_OBJECT_0) {
                        SetLastError(ERROR_OPERATION_ABORTED);
                        return INVALID_HANDLE_VALUE;
                    }
                } else {
                    Sleep(retry_ms);
                }
                continue;
            }
            if (last_error == ERROR_SEM_TIMEOUT) {
                continue;
            }
            return INVALID_HANDLE_VALUE;
        }

        HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                return pipe;
            }
            CloseHandle(pipe);
            return INVALID_HANDLE_VALUE;
        }
        last_error = GetLastError();
    }
    SetLastError(last_error);
    return INVALID_HANDLE_VALUE;
}

bool write_packet(HANDLE pipe, HANDLE stop_event, const std::vector<std::uint8_t>& packet,
                  DWORD timeout_ms = kIoTimeoutMs) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }

    DWORD transferred = 0;
    BOOL started =
        WriteFile(pipe, packet.data(), static_cast<DWORD>(packet.size()), nullptr, &overlapped);
    bool succeeded = false;
    if (started) {
        succeeded = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        HANDLE handles[] = {stop_event, overlapped.hEvent};
        DWORD count = stop_event ? 2 : 1;
        HANDLE* first = stop_event ? handles : &handles[1];
        DWORD wait = WaitForMultipleObjects(count, first, FALSE, timeout_ms);
        if (wait == WAIT_OBJECT_0 + (stop_event ? 1 : 0)) {
            succeeded = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        } else {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            // The cancelled I/O reports "aborted": say it was the wait that ran out.
            if (wait == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
        }
    }

    CloseHandle(overlapped.hEvent);
    return succeeded && transferred == packet.size();
}

bool read_packet(HANDLE pipe, HANDLE stop_event, std::vector<std::uint8_t>* packet,
                 DWORD timeout_ms) {
    if (!packet) {
        return false;
    }

    packet->resize(sizeof(ControlHeader) + CONTROL_MAX_PAYLOAD);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }

    DWORD transferred = 0;
    BOOL started =
        ReadFile(pipe, packet->data(), static_cast<DWORD>(packet->size()), nullptr, &overlapped);
    bool succeeded = false;
    if (started) {
        succeeded = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        HANDLE handles[] = {stop_event, overlapped.hEvent};
        DWORD count = stop_event ? 2 : 1;
        HANDLE* first = stop_event ? handles : &handles[1];
        DWORD wait = WaitForMultipleObjects(count, first, FALSE, timeout_ms);
        if (wait == WAIT_OBJECT_0 + (stop_event ? 1 : 0)) {
            succeeded = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        } else {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            // The cancelled I/O reports "aborted": say it was the wait that ran out.
            if (wait == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
        }
    }

    CloseHandle(overlapped.hEvent);
    if (!succeeded) {
        packet->clear();
        return false;
    }
    packet->resize(transferred);
    return true;
}

} // namespace

class ControlClient::Impl {
public:
    Impl() {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        mutation_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~Impl() {
        stop();
        if (mutation_event_) {
            CloseHandle(mutation_event_);
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
        }
    }

    bool start(SnapshotHandler handler, const std::wstring& pipe_name) {
        if (!stop_event_ || !mutation_event_ || running_.exchange(true)) {
            return false;
        }
        handler_ = std::move(handler);
        pipe_name_ = make_user_pipe_name(pipe_name.empty() ? CONTROL_PIPE_BASE_NAME : pipe_name);
        ResetEvent(stop_event_);
        ResetEvent(mutation_event_);
        worker_ = std::thread([this]() { run(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        SetEvent(stop_event_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void patch_user_config(const std::string& merge_patch_json) {
        if (merge_patch_json.empty() || merge_patch_json.size() > CONTROL_MAX_PAYLOAD) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutation_mutex_);
            pending_patches_.push_back(merge_patch_json);
        }
        if (running_.load(std::memory_order_acquire)) {
            SetEvent(mutation_event_);
        }
    }

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    bool send_subscribe(HANDLE pipe) {
        ControlSubscribe subscribe;
        subscribe.process_id = GetCurrentProcessId();
        subscribe.pointer_size = static_cast<std::uint16_t>(sizeof(void*));

        std::vector<std::uint8_t> packet;
        if (!build_control_packet(ControlMessageType::kSubscribe, {}, &subscribe, sizeof(subscribe),
                                  &packet)) {
            return false;
        }
        return write_packet(pipe, stop_event_, packet);
    }

    bool process_message(const std::vector<std::uint8_t>& packet) {
        ControlMessage message;
        if (!parse_control_packet(packet.data(), packet.size(), &message)) {
            return false;
        }
        if (message.type == ControlMessageType::kConfigSnapshot) {
            if (message.generation.server_epoch == 0 || message.generation.revision == 0 ||
                message.payload.empty()) {
                return false;
            }
            if (handler_) {
                handler_(message.generation, message.payload);
            }
        } else if (message.type == ControlMessageType::kMutationResult) {
            ControlMutationResult result = {};
            if (!decode_control_mutation_result(message.payload, &result)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutation_mutex_);
                if (!patch_in_flight_ || pending_patches_.empty()) {
                    return false;
                }
                pending_patches_.pop_front();
                patch_in_flight_ = false;
                if (!pending_patches_.empty()) {
                    SetEvent(mutation_event_);
                }
            }
            if (!result.succeeded) {
                CXXIME_LOG(L"control event=patch result=0 error=%lu",
                           static_cast<unsigned long>(result.error_code));
            }
        }
        return true;
    }

    bool send_next_patch(HANDLE pipe) {
        std::string patch;
        {
            std::lock_guard<std::mutex> lock(mutation_mutex_);
            if (patch_in_flight_ || pending_patches_.empty()) {
                return true;
            }
            patch = pending_patches_.front();
            patch_in_flight_ = true;
        }

        std::vector<std::uint8_t> packet;
        if (!build_control_packet(ControlMessageType::kPatchUserConfig, {}, patch.data(),
                                  patch.size(), &packet) ||
            !write_packet(pipe, stop_event_, packet)) {
            std::lock_guard<std::mutex> lock(mutation_mutex_);
            patch_in_flight_ = false;
            return false;
        }
        return true;
    }

    bool connected_loop(HANDLE pipe) {
        if (!send_subscribe(pipe)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutation_mutex_);
            if (!pending_patches_.empty()) {
                SetEvent(mutation_event_);
            }
        }

        std::vector<std::uint8_t> read_buffer(sizeof(ControlHeader) + CONTROL_MAX_PAYLOAD);
        OVERLAPPED read = {};
        read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!read.hEvent) {
            return false;
        }

        bool connected = true;
        bool read_pending = false;
        while (running_.load(std::memory_order_acquire) && connected) {
            if (!read_pending) {
                ResetEvent(read.hEvent);
                DWORD transferred = 0;
                BOOL started = ReadFile(pipe, read_buffer.data(),
                                        static_cast<DWORD>(read_buffer.size()), nullptr, &read);
                if (started) {
                    if (!GetOverlappedResult(pipe, &read, &transferred, FALSE)) {
                        connected = false;
                        continue;
                    }
                    std::vector<std::uint8_t> packet(read_buffer.begin(),
                                                     read_buffer.begin() + transferred);
                    connected = process_message(packet);
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    connected = false;
                    continue;
                }
                read_pending = true;
            }

            HANDLE handles[] = {stop_event_, mutation_event_, read.hEvent};
            DWORD wait = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1) {
                if (!send_next_patch(pipe)) {
                    connected = false;
                }
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 2) {
                connected = false;
                continue;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe, &read, &transferred, FALSE)) {
                connected = false;
                continue;
            }
            read_pending = false;
            std::vector<std::uint8_t> packet(read_buffer.begin(),
                                             read_buffer.begin() + transferred);
            connected = process_message(packet);
        }

        if (read_pending) {
            CancelIoEx(pipe, &read);
            DWORD transferred = 0;
            GetOverlappedResult(pipe, &read, &transferred, TRUE);
        }
        {
            std::lock_guard<std::mutex> lock(mutation_mutex_);
            patch_in_flight_ = false;
        }
        CloseHandle(read.hEvent);
        return connected;
    }

    void run() {
        DWORD reconnect_delay_ms = 100;
        while (running_.load(std::memory_order_acquire)) {
            HANDLE pipe = connect_pipe(pipe_name_, static_cast<int>(kConnectSliceMs), stop_event_);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(stop_event_, reconnect_delay_ms) == WAIT_OBJECT_0) {
                    break;
                }
                reconnect_delay_ms = (std::min)(reconnect_delay_ms * 2, 2000UL);
                continue;
            }

            reconnect_delay_ms = 100;
            CXXIME_LOG(L"control event=connect pid=%lu pointer_size=%zu", GetCurrentProcessId(),
                       sizeof(void*));
            connected_loop(pipe);
            CloseHandle(pipe);
            if (running_.load(std::memory_order_acquire)) {
                CXXIME_LOG(L"control event=disconnect pid=%lu reconnect=1", GetCurrentProcessId());
            }
        }
    }

    HANDLE stop_event_ = nullptr;
    HANDLE mutation_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread worker_;
    SnapshotHandler handler_;
    std::wstring pipe_name_;
    std::mutex mutation_mutex_;
    std::deque<std::string> pending_patches_;
    bool patch_in_flight_ = false;
};

ControlClient::ControlClient()
    : impl_(new Impl()) {}

ControlClient::~ControlClient() = default;

bool ControlClient::start(SnapshotHandler handler, const std::wstring& pipe_name) {
    return impl_->start(std::move(handler), pipe_name);
}

void ControlClient::stop() { impl_->stop(); }

void ControlClient::patch_user_config(const std::string& merge_patch_json) {
    impl_->patch_user_config(merge_patch_json);
}

bool ControlClient::is_running() const { return impl_->is_running(); }

bool send_control_request(ControlMessageType request_type, const std::string& request_payload,
                          ControlMessageType response_type, ControlMessage* response,
                          unsigned long* error_code, int timeout_ms,
                          const std::wstring& pipe_name, int response_timeout_ms) {
    if (!response || request_payload.size() > CONTROL_MAX_PAYLOAD) {
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }
    const std::wstring resolved_name =
        make_user_pipe_name(pipe_name.empty() ? CONTROL_PIPE_BASE_NAME : pipe_name);
    HANDLE pipe = connect_pipe(resolved_name, timeout_ms);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (error_code) {
            *error_code = GetLastError();
        }
        return false;
    }

    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<std::uint8_t> request;
    bool request_built = build_control_packet(request_type, {}, request_payload.data(),
                                              request_payload.size(), &request);
    std::vector<std::uint8_t> response_packet;
    const DWORD read_timeout =
        response_timeout_ms < 0
            ? INFINITE
            : static_cast<DWORD>(response_timeout_ms > 0 ? response_timeout_ms : timeout_ms);
    bool ok = stop_event && request_built && write_packet(pipe, stop_event, request, timeout_ms) &&
              read_packet(pipe, stop_event, &response_packet, read_timeout);

    if (ok) {
        ok = parse_control_packet(response_packet.data(), response_packet.size(), response) &&
             response->type == response_type;
        if (!ok) {
            SetLastError(ERROR_INVALID_DATA);
        }
    }

    if (!ok && error_code) {
        *error_code = GetLastError();
    }

    if (stop_event) {
        CloseHandle(stop_event);
    }
    CloseHandle(pipe);
    return ok;
}

bool replace_user_config(const std::string& config_json, ConfigGeneration* generation,
                         unsigned long* error_code, int timeout_ms, const std::wstring& pipe_name,
                         int response_timeout_ms) {
    if (config_json.empty()) {
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }

    ControlMessage message;
    bool transaction_ok = send_control_request(
        ControlMessageType::kReplaceUserConfig, config_json,
        ControlMessageType::kMutationResult, &message, error_code, timeout_ms, pipe_name,
        response_timeout_ms);
    ControlMutationResult result = {};
    bool ok = transaction_ok && decode_control_mutation_result(message.payload, &result);
    if (transaction_ok && !ok && error_code) {
        *error_code = ERROR_INVALID_DATA;
    }
    if (ok) {
        if (generation) {
            *generation = message.generation;
        }
        if (error_code) {
            *error_code = result.error_code;
        }
        ok = result.succeeded != 0;
    }
    return ok;
}

} // namespace cxxime
