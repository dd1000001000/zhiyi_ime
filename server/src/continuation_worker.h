// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Long pinyin in the background (laya_rerank.h, LayaRerank::set_continuation_in_background):
// a key translates without the continuation model and leaves the syllable paths it lacked;
// this thread computes them, the latest composition of each session only, and reports the
// session and composition (Engine::continuation_key) back, which then translates again.
#ifndef CXXIME_SERVER_CONTINUATION_WORKER_H_
#define CXXIME_SERVER_CONTINUATION_WORKER_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cxxime {
struct ContinuationWork;
class EngineRuntimeState;
}  // namespace cxxime

class ContinuationWorker {
public:
    // Computes `work` (the model calls); false when it could not (no model yet).
    using Compute = std::function<bool(const cxxime::ContinuationWork& work,
                                       const cxxime::EngineRuntimeState& runtime)>;
    // The work for `key` of session `session_id` is done.
    using Done = std::function<void(std::uint32_t session_id, const std::string& key)>;

    ContinuationWorker(Compute compute, Done done);
    ~ContinuationWorker();

    // Replaces what is waiting for the session (an earlier composition is not computed).
    void submit(std::uint32_t session_id, std::string key,
                std::shared_ptr<const cxxime::ContinuationWork> work,
                std::shared_ptr<const cxxime::EngineRuntimeState> runtime);
    // Waits for the computation running, if any; nothing else is computed or reported.
    void stop();

private:
    struct Job {
        std::string key;
        std::shared_ptr<const cxxime::ContinuationWork> work;
        std::shared_ptr<const cxxime::EngineRuntimeState> runtime;
    };

    void run();

    Compute compute_;
    Done done_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::uint32_t, Job> jobs_;  // per session, the latest
    bool stopping_ = false;
    std::thread thread_;
};

#endif  // CXXIME_SERVER_CONTINUATION_WORKER_H_
