// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "continuation_worker.h"

#include <algorithm>
#include <utility>

ContinuationWorker::ContinuationWorker(Compute compute, Done done)
    : compute_(std::move(compute)), done_(std::move(done)) {
    thread_ = std::thread([this] { run(); });
}

ContinuationWorker::~ContinuationWorker() { stop(); }

void ContinuationWorker::submit(std::uint32_t session_id, std::string key,
                                std::shared_ptr<const cxxime::ContinuationWork> work,
                                std::shared_ptr<const cxxime::EngineRuntimeState> runtime,
                                std::chrono::milliseconds delay) {
    if (!work || !runtime) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_[session_id] = Job{std::chrono::steady_clock::now() + (std::max)(delay, std::chrono::milliseconds(0)),
                                std::move(key), std::move(work), std::move(runtime)};
    }
    wake_.notify_one();
}

void ContinuationWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void ContinuationWorker::run() {
    for (;;) {
        std::uint32_t session_id = 0;
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_) return;
                auto due = jobs_.begin();
                for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                    if (it->second.due < due->second.due) due = it;
                }
                if (due->second.due <= std::chrono::steady_clock::now()) {
                    session_id = due->first;
                    job = std::move(due->second);
                    jobs_.erase(due);
                    break;
                }
                // Until it is due, or a submit or stop changes what waits.
                wake_.wait_until(lock, due->second.due);
            }
        }
        if (!compute_(*job.work, *job.runtime)) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
        }
        done_(session_id, job.key);
    }
}
