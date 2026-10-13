// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "translate_service.h"

#include <algorithm>

namespace {

// Whether the text is mostly Chinese (CJK): its translation goes to English, else to Chinese.
bool looks_chinese(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        uint32_t code_point = 0;
        size_t extra = 0;
        if ((c & 0xE0) == 0xC0) {
            code_point = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            code_point = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            code_point = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= text.size()) break;
        for (size_t k = 0; k < extra; ++k) {
            code_point = (code_point << 6) | (static_cast<unsigned char>(text[i + 1 + k]) & 0x3F);
        }
        if (code_point >= 0x3000 && code_point <= 0x9FFF) return true;  // CJK punctuation / Han
        i += extra + 1;
    }
    return false;
}

constexpr int kTranslateTimeoutMs = 30000;

}  // namespace

TranslateService::TranslateService() = default;

TranslateService::~TranslateService() {
    stop();
}

bool TranslateService::start(const std::shared_ptr<const cxxime::Config>& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }
    worker_ = std::thread([this] { run(); });
    return true;
}

void TranslateService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

void TranslateService::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void TranslateService::request(uint32_t id, const std::string& text) {
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text.empty()) {
            phase_ = Phase::kIdle;
            owner_ = id;
            ++generation_;
            text_.clear();
            return;
        }
        phase_ = Phase::kTranslating;
        owner_ = id;
        generation = ++generation_;
        text_.clear();
    }
    post([this, id, generation, text] { do_translate(id, generation, text); });
}

void TranslateService::session_ended(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ == id) {
        phase_ = Phase::kIdle;
        owner_ = 0;
        text_.clear();
    }
}

uint32_t TranslateService::take_status(uint32_t id, std::string* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return cxxime::kTranslateIdle;
    if (phase_ == Phase::kDone) {
        if (text) *text = text_;
        phase_ = Phase::kIdle;
        owner_ = 0;
        text_.clear();
        return cxxime::kTranslateDone;
    }
    return phase_ == Phase::kTranslating ? cxxime::kTranslateTranslating : cxxime::kTranslateIdle;
}

void TranslateService::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void TranslateService::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(5),
                           [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) {
            task();
            continue;
        }
        // Idle: unload the model after translate.idle_seconds without a request.
        std::shared_ptr<const cxxime::Config> config;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = config_;
        }
        if (server_ && config && config->translate_idle_seconds > 0 &&
            std::chrono::steady_clock::now() - last_used_ >
                std::chrono::seconds(config->translate_idle_seconds)) {
            server_->stop();
            server_.reset();
            server_device_.clear();
        }
    }
}

bool TranslateService::ensure_server(std::string* error) {
    if (server_ && server_->running()) return true;

    cxxime::mt::InstalledModel installed;
    if (!cxxime::mt::find_installed(&installed)) {
        if (error) *error = "the translation model is not installed";
        return false;
    }
    std::string device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_) device = config_->translate_device;
    }
    if (device.empty()) device = cxxime::mt::kCpuDevice;  // "" means the CPU, as elsewhere
    auto server = std::make_unique<cxxime::mt::LlamaServer>();
    if (!server->start(installed, device, error)) return false;
    server_ = std::move(server);
    server_device_ = device;
    return true;
}

void TranslateService::do_translate(uint32_t id, uint64_t generation, std::string text) {
    std::string result;
    std::string error;
    if (ensure_server(&error)) {
        last_used_ = std::chrono::steady_clock::now();
        const bool chinese = looks_chinese(text);
        cxxime::mt::Item item;
        item.source = chinese ? "zh" : "en";
        item.target = chinese ? "en" : "zh";
        item.text = text;
        const int max_tokens = std::clamp(static_cast<int>(text.size()) * 2, 64, 1024);
        const std::vector<std::string> out =
            server_->translate_document({item}, {}, max_tokens, kTranslateTimeoutMs);
        if (!out.empty()) result = out[0];
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id || generation_ != generation || phase_ != Phase::kTranslating) return;
    if (result.empty()) {
        phase_ = Phase::kIdle;  // nothing to show: the client stops polling
    } else {
        text_ = std::move(result);
        phase_ = Phase::kDone;
    }
}
