// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/control_server.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include <cxxime/logging.h>
#include <cxxime/pipe_names.h>

#include "security_attributes.h"

namespace cxxime {
namespace {

constexpr DWORD kAcceptWaitMs = 100;
constexpr DWORD kWriteTimeoutMs = 2000;

std::uint64_t make_server_epoch() {
    std::uint64_t value = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 &&
        value != 0) {
        return value;
    }

    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    value = static_cast<std::uint64_t>(counter.QuadPart) ^
            (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    return value == 0 ? 1 : value;
}

bool connect_pipe_instance(HANDLE pipe, const std::atomic<bool>& running) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }

    bool connected = false;
    bool connect_pending = false;
    BOOL started = ConnectNamedPipe(pipe, &overlapped);
    if (started) {
        connected = true;
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (error == ERROR_IO_PENDING) {
            connect_pending = true;
            while (running.load(std::memory_order_acquire)) {
                DWORD wait = WaitForSingleObject(overlapped.hEvent, kAcceptWaitMs);
                if (wait == WAIT_OBJECT_0) {
                    DWORD transferred = 0;
                    connected =
                        GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
                    connect_pending = false;
                    break;
                }
                if (wait != WAIT_TIMEOUT) {
                    break;
                }
            }
        }
    }

    if (connect_pending) {
        CancelIoEx(pipe, &overlapped);
        DWORD transferred = 0;
        GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    return connected;
}

bool write_packet(HANDLE pipe, HANDLE stop_event, const std::vector<std::uint8_t>& packet) {
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
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kWriteTimeoutMs);
        if (wait == WAIT_OBJECT_0 + 1) {
            succeeded = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        } else {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
        }
    }

    CloseHandle(overlapped.hEvent);
    return succeeded && transferred == packet.size();
}

std::vector<std::uint8_t> make_packet(ControlMessageType type, ConfigGeneration generation,
                                      const void* payload = nullptr, std::size_t payload_size = 0) {
    std::vector<std::uint8_t> packet;
    build_control_packet(type, generation, payload, payload_size, &packet);
    return packet;
}

} // namespace

class ControlServer::Impl {
public:
    struct ClientContext {
        ~ClientContext() {
            if (pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(pipe);
            }
            if (publish_event) {
                CloseHandle(publish_event);
            }
            if (stop_event) {
                CloseHandle(stop_event);
            }
        }

        void queue_snapshot(const std::vector<std::uint8_t>& packet) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                pending_snapshot = packet;
            }
            SetEvent(publish_event);
        }

        bool take_snapshot(std::vector<std::uint8_t>* packet) {
            std::lock_guard<std::mutex> lock(pending_mutex);
            if (pending_snapshot.empty()) {
                return false;
            }
            packet->swap(pending_snapshot);
            return true;
        }

        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE stop_event = nullptr;
        HANDLE publish_event = nullptr;
        std::thread worker;
        std::atomic<bool> subscribed{false};
        std::atomic<bool> finished{false};
        std::mutex pending_mutex;
        std::vector<std::uint8_t> pending_snapshot;
    };

    ~Impl() { stop(); }

    bool start(const std::string& initial_config_json, MutationHandler mutation_handler,
               RequestHandler request_handler, const std::wstring& pipe_name) {
        if (initial_config_json.empty() || running_.exchange(true)) {
            return false;
        }

        mutation_handler_ = std::move(mutation_handler);
        request_handler_ = std::move(request_handler);
        pipe_name_ = make_user_pipe_name(pipe_name.empty() ? CONTROL_PIPE_BASE_NAME : pipe_name);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            generation_ = {make_server_epoch(), 1};
            snapshot_json_ = initial_config_json;
            if (!build_control_packet(ControlMessageType::kConfigSnapshot, generation_,
                                      initial_config_json.data(), initial_config_json.size(),
                                      &snapshot_packet_)) {
                running_.store(false, std::memory_order_release);
                return false;
            }
        }

        ConfigGeneration initial_generation = generation();
        accept_thread_ = std::thread([this]() { accept_loop(); });
        CXXIME_LOG(L"control event=start epoch=%llu revision=1 payload_bytes=%zu",
                   static_cast<unsigned long long>(initial_generation.server_epoch),
                   initial_config_json.size());
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        unblock_accept();
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        std::vector<std::shared_ptr<ClientContext>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients = clients_;
        }
        for (const auto& client : clients) {
            SetEvent(client->stop_event);
        }
        for (const auto& client : clients) {
            if (client->worker.joinable()) {
                client->worker.join();
            }
        }
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.clear();
        }
    }

    ConfigGeneration generation() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return generation_;
    }

    std::size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return static_cast<std::size_t>(std::count_if(
            clients_.begin(), clients_.end(), [](const std::shared_ptr<ClientContext>& client) {
                return client->subscribed.load(std::memory_order_acquire) &&
                       !client->finished.load(std::memory_order_acquire);
            }));
    }

private:
    void unblock_accept() {
        for (int attempt = 0; attempt < 20; ++attempt) {
            HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(pipe);
                return;
            }
            Sleep(10);
        }
    }

    void reap_finished_clients() {
        std::vector<std::shared_ptr<ClientContext>> finished;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = clients_.begin();
            while (it != clients_.end()) {
                if ((*it)->finished.load(std::memory_order_acquire)) {
                    finished.push_back(*it);
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& client : finished) {
            if (client->worker.joinable()) {
                client->worker.join();
            }
        }
    }

    void accept_loop() {
        SecurityAttributes security;
        while (running_.load(std::memory_order_acquire)) {
            reap_finished_clients();
            HANDLE pipe = CreateNamedPipeW(
                pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                static_cast<DWORD>(sizeof(ControlHeader) + CONTROL_MAX_PAYLOAD),
                static_cast<DWORD>(sizeof(ControlHeader) + CONTROL_MAX_PAYLOAD), 0, security.get());
            if (pipe == INVALID_HANDLE_VALUE) {
                if (running_.load(std::memory_order_acquire)) {
                    Sleep(100);
                }
                continue;
            }

            if (!connect_pipe_instance(pipe, running_)) {
                CloseHandle(pipe);
                continue;
            }
            if (!running_.load(std::memory_order_acquire)) {
                CloseHandle(pipe);
                break;
            }

            auto client = std::make_shared<ClientContext>();
            client->pipe = pipe;
            client->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            client->publish_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!client->stop_event || !client->publish_event) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.push_back(client);
            }
            client->worker = std::thread([this, client]() { client_loop(client); });
        }
        reap_finished_clients();
    }

    void queue_current_snapshot(const std::shared_ptr<ClientContext>& client) {
        std::vector<std::uint8_t> packet;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            packet = snapshot_packet_;
        }
        client->queue_snapshot(packet);
    }

public:
    bool publish_snapshot(const std::string& config_json, ConfigGeneration* published_generation) {
        std::vector<std::uint8_t> packet;
        ConfigGeneration generation;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            generation = generation_;
            if (config_json != snapshot_json_) {
                ++generation.revision;
                if (!build_control_packet(ControlMessageType::kConfigSnapshot, generation,
                                          config_json.data(), config_json.size(), &packet)) {
                    return false;
                }
                generation_ = generation;
                snapshot_json_ = config_json;
                snapshot_packet_ = packet;
                changed = true;
            }
        }

        if (changed) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& client : clients_) {
                if (client->subscribed.load(std::memory_order_acquire) &&
                    !client->finished.load(std::memory_order_acquire)) {
                    client->queue_snapshot(packet);
                }
            }
        }
        if (published_generation) {
            *published_generation = generation;
        }
        if (changed) {
            CXXIME_LOG(L"control event=publish epoch=%llu revision=%llu payload_bytes=%zu",
                       static_cast<unsigned long long>(generation.server_epoch),
                       static_cast<unsigned long long>(generation.revision), config_json.size());
        }
        return true;
    }

private:
    bool handle_message(const std::shared_ptr<ClientContext>& client,
                        const ControlMessage& message) {
        switch (message.type) {
            case ControlMessageType::kSubscribe: {
                ControlSubscribe subscribe = {};
                if (message.generation != ConfigGeneration{} ||
                    !decode_control_subscribe(message.payload, &subscribe) ||
                    subscribe.process_id == 0 ||
                    (subscribe.pointer_size != 4 && subscribe.pointer_size != 8) ||
                    subscribe.reserved != 0) {
                    return false;
                }
                client->subscribed.store(true, std::memory_order_release);
                queue_current_snapshot(client);
                return true;
            }

            case ControlMessageType::kReplaceUserConfig:
            case ControlMessageType::kPatchUserConfig: {
                if (message.generation == ConfigGeneration{} && !message.payload.empty()) {
                    UserConfigMutationKind kind = message.type == ControlMessageType::kReplaceUserConfig
                                                  ? UserConfigMutationKind::kReplace
                                                  : UserConfigMutationKind::kMergePatch;
                    std::string config_json;
                    unsigned long error_code = ERROR_INVALID_DATA;
                    bool succeeded = mutation_handler_ && mutation_handler_(kind, message.payload,
                                                                            &config_json, &error_code);
                    ConfigGeneration current = generation();
                    if (succeeded && config_json.empty()) {
                        succeeded = false;
                        error_code = ERROR_INVALID_DATA;
                    } else if (succeeded && !publish_snapshot(config_json, &current)) {
                        succeeded = false;
                        error_code = ERROR_BUFFER_OVERFLOW;
                    }
                    CXXIME_LOG(L"control event=mutation kind=%u result=%u error=%lu epoch=%llu "
                               L"revision=%llu",
                            static_cast<unsigned int>(kind), succeeded ? 1U : 0U,
                            succeeded ? ERROR_SUCCESS : error_code,
                            static_cast<unsigned long long>(current.server_epoch),
                            static_cast<unsigned long long>(current.revision));

                    ControlMutationResult result;
                    result.succeeded = succeeded ? 1U : 0U;
                    result.error_code = succeeded ? ERROR_SUCCESS : error_code;
                    auto packet = make_packet(ControlMessageType::kMutationResult, current, &result,
                                              sizeof(result));
                    return write_packet(client->pipe, client->stop_event, packet);
                }
                return false;
            }

            case ControlMessageType::kPing: {
                if (message.generation != ConfigGeneration{} || !message.payload.empty()) {
                    return false;
                }
                auto packet = make_packet(ControlMessageType::kPong, generation());
                return write_packet(client->pipe, client->stop_event, packet);
            }

            case ControlMessageType::kLexiconRequest:
            case ControlMessageType::kUserBackupRequest:
            case ControlMessageType::kWordListRequest: {
                const ControlMessageType expected_response =
                    message.type == ControlMessageType::kLexiconRequest
                        ? ControlMessageType::kLexiconResult
                    : message.type == ControlMessageType::kUserBackupRequest
                        ? ControlMessageType::kUserBackupResult
                        : ControlMessageType::kWordListResult;
                ControlMessageType response_type = expected_response;
                std::string response_payload;
                if (message.generation != ConfigGeneration{} || !request_handler_ ||
                    !request_handler_(message.type, message.payload, &response_type,
                                      &response_payload) ||
                    response_type != expected_response || response_payload.empty() ||
                    response_payload.size() > CONTROL_MAX_PAYLOAD) {
                    return false;
                }
                auto packet = make_packet(response_type, {}, response_payload.data(),
                                          response_payload.size());
                return write_packet(client->pipe, client->stop_event, packet);
            }

            default:
                return true;
        }
    }

    void client_loop(const std::shared_ptr<ClientContext>& client) {
        std::vector<std::uint8_t> read_buffer(sizeof(ControlHeader) + CONTROL_MAX_PAYLOAD);
        OVERLAPPED read = {};
        read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool read_pending = false;
        bool connected = read.hEvent != nullptr;

        while (connected && running_.load(std::memory_order_acquire)) {
            if (!read_pending) {
                ResetEvent(read.hEvent);
                DWORD transferred = 0;
                BOOL started = ReadFile(client->pipe, read_buffer.data(),
                                        static_cast<DWORD>(read_buffer.size()), nullptr, &read);
                if (started) {
                    if (!GetOverlappedResult(client->pipe, &read, &transferred, FALSE)) {
                        break;
                    }
                    ControlMessage message;
                    connected = parse_control_packet(read_buffer.data(), transferred, &message) &&
                                handle_message(client, message);
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    break;
                }
                read_pending = true;
            }

            HANDLE handles[] = {client->stop_event, client->publish_event, read.hEvent};
            DWORD wait = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1) {
                std::vector<std::uint8_t> packet;
                if (client->take_snapshot(&packet) &&
                    !write_packet(client->pipe, client->stop_event, packet)) {
                    connected = false;
                }
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 2) {
                break;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(client->pipe, &read, &transferred, FALSE)) {
                break;
            }
            read_pending = false;
            ControlMessage message;
            connected = parse_control_packet(read_buffer.data(), transferred, &message) &&
                        handle_message(client, message);
        }

        if (read_pending) {
            CancelIoEx(client->pipe, &read);
            DWORD transferred = 0;
            GetOverlappedResult(client->pipe, &read, &transferred, TRUE);
        }
        if (read.hEvent) {
            CloseHandle(read.hEvent);
        }
        client->finished.store(true, std::memory_order_release);
    }

    std::wstring pipe_name_;
    MutationHandler mutation_handler_;
    RequestHandler request_handler_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    mutable std::mutex state_mutex_;
    ConfigGeneration generation_;
    std::string snapshot_json_;
    std::vector<std::uint8_t> snapshot_packet_;

    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<ClientContext>> clients_;
};

ControlServer::ControlServer()
    : impl_(new Impl()) {}

ControlServer::~ControlServer() = default;

bool ControlServer::start(const std::string& initial_config_json, MutationHandler mutation_handler,
                          const std::wstring& pipe_name) {
    return impl_->start(initial_config_json, std::move(mutation_handler), {}, pipe_name);
}

bool ControlServer::start(const std::string& initial_config_json, MutationHandler mutation_handler,
                          RequestHandler request_handler, const std::wstring& pipe_name) {
    return impl_->start(initial_config_json, std::move(mutation_handler),
                        std::move(request_handler), pipe_name);
}

void ControlServer::stop() { impl_->stop(); }

bool ControlServer::publish_snapshot(const std::string& config_json, ConfigGeneration* generation) {
    return impl_->publish_snapshot(config_json, generation);
}

ConfigGeneration ControlServer::generation() const { return impl_->generation(); }

std::size_t ControlServer::subscriber_count() const { return impl_->subscriber_count(); }

} // namespace cxxime
