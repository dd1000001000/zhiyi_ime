// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Next words (联想, docs/next-words.md) in the background: after a commit the session asks for the
// words that may follow its context (LayaRerank::next_words, ~15-35 ms on the CPU); this thread
// computes the latest request of each session and reports them back with the request's
// generation (the session drops a result for an older one: a key came in the meantime).
#ifndef CXXIME_SERVER_NEXT_WORDS_WORKER_H_
#define CXXIME_SERVER_NEXT_WORDS_WORKER_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cxxime {
class EngineRuntimeState;
}  // namespace cxxime

class NextWordsWorker {
public:
    struct Request {
        std::uint64_t generation = 0;
        std::string context;
        std::string lang;  // "zh", "en"
        std::size_t count = 0;
        std::shared_ptr<const cxxime::EngineRuntimeState> runtime;
    };
    // The words for `request` (blocks for the model).
    using Compute = std::function<std::vector<std::string>(const Request& request)>;
    using Done = std::function<void(std::uint32_t session_id, std::uint64_t generation,
                                    std::vector<std::string> words)>;

    NextWordsWorker(Compute compute, Done done);
    ~NextWordsWorker();

    // Replaces what is waiting for the session.
    void submit(std::uint32_t session_id, Request request);
    // Waits for the computation running, if any; nothing else is computed or reported.
    void stop();

private:
    void run();

    Compute compute_;
    Done done_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::uint32_t, Request> jobs_;  // per session, the latest
    bool stopping_ = false;
    std::thread thread_;
};

#endif  // CXXIME_SERVER_NEXT_WORDS_WORKER_H_
