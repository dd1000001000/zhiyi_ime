// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "machine_translator.h"

#include <chrono>

#include <windows.h>

#include <cxxime/logging.h>

namespace {

constexpr size_t kMaxCached = 20000;
constexpr long long kRetryAfterFailureMs = 5 * 60 * 1000;

std::string cache_key(const std::string& target, const std::string& text) {
    return target + '\x1F' + text;
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

MachineTranslator& MachineTranslator::instance() {
    static MachineTranslator translator;
    return translator;
}

MachineTranslator::~MachineTranslator() { stop(); }

void MachineTranslator::set_callback(Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

bool MachineTranslator::cached(const std::string& target, const std::string& text,
                               std::string* translation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = cache_.find(cache_key(target, text));
    if (found == cache_.end()) return false;
    *translation = found->second;
    return true;
}

void MachineTranslator::request(std::vector<cxxime::mt::Item> items, std::string context,
                                std::string device, int idle_seconds) {
    if (items.empty() || device.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (device == failed_device_ && now_ms() - failed_at_ms_ < kRetryAfterFailureMs) return;
    page_ = Page{std::move(items), std::move(context), std::move(device), idle_seconds};
    have_page_ = true;
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    wake_.notify_one();
}

void MachineTranslator::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void MachineTranslator::run() {
    cxxime::mt::LlamaServer server;
    int idle_seconds = 60;
    for (;;) {
        Page page;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool woken = wake_.wait_for(lock, std::chrono::seconds(idle_seconds),
                                              [this] { return stopping_ || have_page_; });
            if (stopping_) break;
            if (!woken) {  // nothing to translate for a while: free the graphics card
                if (server.running()) {
                    CXXIME_LOG(L"%s", L"MachineTranslator: idle, model unloaded");
                    server.stop();
                }
                continue;
            }
            page = std::move(page_);
            have_page_ = false;
            // Translated meanwhile (an earlier page with the same words).
            std::vector<cxxime::mt::Item> left;
            for (auto& item : page.items) {
                if (cache_.find(cache_key(item.target, item.text)) == cache_.end()) {
                    left.push_back(std::move(item));
                }
            }
            page.items = std::move(left);
        }
        idle_seconds = page.idle_seconds;
        if (page.items.empty()) continue;
        if (!server.running() || server.device() != page.device) {
            cxxime::mt::InstalledModel installed;
            std::string error = "the model is not installed";
            const auto started = now_ms();
            if (!cxxime::mt::find_installed(&installed) ||
                !server.start(installed, page.device, &error)) {
                CXXIME_LOG(L"MachineTranslator: cannot start on %S: %S", page.device.c_str(), error.c_str());
                std::lock_guard<std::mutex> lock(mutex_);
                failed_device_ = page.device;
                failed_at_ms_ = now_ms();
                continue;
            }
            CXXIME_LOG(L"MachineTranslator: model loaded on %S in %lld ms", page.device.c_str(),
                       now_ms() - started);
        }
        const std::vector<std::string> translations = server.translate(page.items, page.context);
        if (!server.running()) {  // it died: nothing is remembered, started again on the next page
            server.stop();
            continue;
        }
        std::vector<std::pair<std::string, std::string>> done;
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cache_.size() > kMaxCached) cache_.clear();
            for (size_t i = 0; i < page.items.size(); ++i) {
                cache_[cache_key(page.items[i].target, page.items[i].text)] = translations[i];
                if (!translations[i].empty()) done.emplace_back(page.items[i].text, translations[i]);
            }
            callback = callback_;
        }
        if (callback && !done.empty()) callback(done);
    }
    server.stop();
}
