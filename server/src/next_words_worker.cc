// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "next_words_worker.h"

#include <utility>

NextWordsWorker::NextWordsWorker(Compute compute, Done done)
    : compute_(std::move(compute)), done_(std::move(done)) {
    thread_ = std::thread([this] { run(); });
}

NextWordsWorker::~NextWordsWorker() { stop(); }

void NextWordsWorker::submit(std::uint32_t session_id, Request request) {
    if (!request.runtime || request.count == 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_[session_id] = std::move(request);
    }
    wake_.notify_one();
}

void NextWordsWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void NextWordsWorker::run() {
    for (;;) {
        std::uint32_t session_id = 0;
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            auto it = jobs_.begin();
            session_id = it->first;
            request = std::move(it->second);
            jobs_.erase(it);
        }
        std::vector<std::string> words = compute_(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
        }
        done_(session_id, request.generation, std::move(words));
    }
}
