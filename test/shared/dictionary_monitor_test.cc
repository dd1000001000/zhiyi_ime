// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "support/testutil.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <windows.h>
#include <cxxime/dictionary_monitor.h>

namespace {

std::string make_temp_dir(const char* name) {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + name + "_" + std::to_string(GetCurrentProcessId()) +
                      "_" + std::to_string(GetTickCount()) + "\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

void write_file(const std::string& path, const char* content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
}

bool wait_for(std::atomic<int>& value, int expected, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (value.load() < expected) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace

TEST(DictionaryMonitor, start_returns_false_for_empty_paths) {
    cxxime::DictionaryMonitor monitor;
    ASSERT_TRUE(!monitor.start({}, [] { return true; }));
    ASSERT_TRUE(!monitor.running());
}

TEST(DictionaryMonitor, callback_on_watched_file_change) {
    std::string dir = make_temp_dir("zhiyi_dict_monitor");
    std::string path = dir + "dictionary_manifest.json";
    write_file(path, "old");

    std::atomic<int> count{0};
    cxxime::DictionaryMonitorOptions options;
    options.debounce_ms = 20;
    options.poll_ms = 100;
    options.retry_ms = 50;

    cxxime::DictionaryMonitor monitor;
    ASSERT_TRUE(monitor.start({path}, [&] {
        count.fetch_add(1);
        return true;
    }, options));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_file(path, "new-content");

    ASSERT_TRUE(wait_for(count, 1, 3000));
    monitor.stop();

    DeleteFileA(path.c_str());
    RemoveDirectoryA(dir.c_str());
}

TEST(DictionaryMonitor, failed_callback_retries_after_change) {
    std::string dir = make_temp_dir("zhiyi_dict_monitor_retry");
    std::string path = dir + "dictionary_manifest.json";
    write_file(path, "old");

    std::atomic<int> count{0};
    cxxime::DictionaryMonitorOptions options;
    options.debounce_ms = 20;
    options.poll_ms = 100;
    options.retry_ms = 50;
    options.max_retries = 2;

    cxxime::DictionaryMonitor monitor;
    ASSERT_TRUE(monitor.start({path}, [&] {
        int attempt = count.fetch_add(1) + 1;
        return attempt >= 2;
    }, options));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    write_file(path, "new-content");

    ASSERT_TRUE(wait_for(count, 2, 3000));
    monitor.stop();

    DeleteFileA(path.c_str());
    RemoveDirectoryA(dir.c_str());
}

RUN_ALL_TESTS()
