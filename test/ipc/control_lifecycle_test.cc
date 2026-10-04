// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

#include <cxxime/control_client.h>
#include <cxxime/control_server.h>

#include "support/testutil.h"

namespace {

constexpr auto kStopLimit = std::chrono::milliseconds(2000);

bool wait_for(const std::function<bool()>& condition, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
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
    return L"\\\\.\\pipe\\ZhiyiIME-Control-Lifecycle-" + std::to_wstring(GetCurrentProcessId()) +
           L"-" + std::to_wstring(sequence.fetch_add(1));
}

template <typename Function>
std::chrono::milliseconds elapsed_time(Function&& function) {
    const auto start = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start);
}

} // namespace

TEST(ControlLifecycle, client_stops_with_idle_server) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(R"({"theme":"azure"})", {}, pipe_name));

    std::atomic<int> snapshot_count{0};
    cxxime::ControlClient client;
    ASSERT_TRUE(client.start(
        [&](cxxime::ConfigGeneration, const std::string&) { snapshot_count.fetch_add(1); },
        pipe_name));
    ASSERT_TRUE(wait_for([&]() { return snapshot_count.load() == 1; }));

    const auto elapsed = elapsed_time([&]() { client.stop(); });
    ASSERT_TRUE(elapsed < kStopLimit);
    ASSERT_TRUE(wait_for([&]() { return server.subscriber_count() == 0; }));
    server.stop();
}

TEST(ControlLifecycle, server_stops_with_idle_client) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(R"({"theme":"azure"})", {}, pipe_name));

    cxxime::ControlClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.subscriber_count() == 1; }));

    const auto server_elapsed = elapsed_time([&]() { server.stop(); });
    ASSERT_TRUE(server_elapsed < kStopLimit);
    const auto client_elapsed = elapsed_time([&]() { client.stop(); });
    ASSERT_TRUE(client_elapsed < kStopLimit);
}

TEST(ControlLifecycle, server_stops_with_two_idle_clients) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(R"({"theme":"azure"})", {}, pipe_name));

    cxxime::ControlClient first;
    cxxime::ControlClient second;
    ASSERT_TRUE(first.start({}, pipe_name));
    ASSERT_TRUE(second.start({}, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.subscriber_count() == 2; }));

    const auto server_elapsed = elapsed_time([&]() { server.stop(); });
    ASSERT_TRUE(server_elapsed < kStopLimit);
    ASSERT_TRUE(elapsed_time([&]() { first.stop(); }) < kStopLimit);
    ASSERT_TRUE(elapsed_time([&]() { second.stop(); }) < kStopLimit);
}

TEST(ControlLifecycle, server_stops_after_one_shot_request_disconnects) {
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

    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(
        !cxxime::replace_user_config(R"({"theme":"dark"})", nullptr, &error_code, 3000, pipe_name));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_INVALID_DATA));

    const auto elapsed = elapsed_time([&]() { server.stop(); });
    ASSERT_TRUE(elapsed < kStopLimit);
}

TEST(ControlLifecycle, client_and_server_stop_concurrently) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::ControlServer server;
    ASSERT_TRUE(server.start(R"({"theme":"azure"})", {}, pipe_name));

    cxxime::ControlClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.subscriber_count() == 1; }));

    HANDLE start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_TRUE(start_event != nullptr);
    const auto start = std::chrono::steady_clock::now();
    std::thread client_thread([&]() {
        WaitForSingleObject(start_event, INFINITE);
        client.stop();
    });
    std::thread server_thread([&]() {
        WaitForSingleObject(start_event, INFINITE);
        server.stop();
    });
    SetEvent(start_event);
    client_thread.join();
    server_thread.join();
    CloseHandle(start_event);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_TRUE(elapsed < kStopLimit);
}

RUN_ALL_TESTS()
