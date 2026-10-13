// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Selection translation: the client sends the text selected in the input box
// (IPCCommand::TRANSLATE_SELECTION) and polls TRANSLATE_STATUS for the translation. The offline
// model (machine_translation.h, the same one the learning mode uses) is loaded on first use and
// unloaded after a while unused; the translation runs on a thread of its own, never on the IPC
// thread (loading the model can take seconds).
//
// One request at a time, for the session that asked; another request replaces it.
#ifndef CXXIME_SERVER_TRANSLATE_SERVICE_H_
#define CXXIME_SERVER_TRANSLATE_SERVICE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <cxxime/config.h>
#include <cxxime/machine_translation.h>

class TranslateService {
public:
    TranslateService();
    ~TranslateService();

    bool start(const std::shared_ptr<const cxxime::Config>& config);
    void stop();
    void update_config(const std::shared_ptr<const cxxime::Config>& config);

    // Session `id` selected `text` (UTF-8): translate it.
    void request(uint32_t id, const std::string& text);
    // The session ended or lost the focus: drop its request.
    void session_ended(uint32_t id);

    // kTranslate* (ipc_protocol.h) for session `id`; when done the text, once.
    uint32_t take_status(uint32_t id, std::string* text);

private:
    enum class Phase { kIdle, kTranslating, kDone };

    void post(std::function<void()> task);
    void run();
    void do_translate(uint32_t id, uint64_t generation, std::string text);
    bool ensure_server(std::string* error);

    std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    uint32_t owner_ = 0;
    uint64_t generation_ = 0;
    std::string text_;
    std::shared_ptr<const cxxime::Config> config_;

    // The worker's own.
    std::thread worker_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::unique_ptr<cxxime::mt::LlamaServer> server_;
    std::string server_device_;
    std::chrono::steady_clock::time_point last_used_{};
};

#endif  // CXXIME_SERVER_TRANSLATE_SERVICE_H_
