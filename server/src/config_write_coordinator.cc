// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "config_write_coordinator.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <cxxime/logging.h>
#include <cxxime/problem_log.h>

#include "config_store.h"

namespace {

constexpr auto kPatchBatchWindow = std::chrono::milliseconds(16);

struct WriteRequest {
    cxxime::UserConfigMutationKind kind = cxxime::UserConfigMutationKind::kMergePatch;
    std::string payload;
    std::chrono::steady_clock::time_point enqueued_at;
    std::mutex mutex;
    std::condition_variable completed_event;
    bool wait_for_result = false;
    bool completed = false;
    bool succeeded = false;
    unsigned long error_code = ERROR_OPERATION_ABORTED;
    std::string config_json;
    bool snapshot = false;
};

void complete_request(const std::shared_ptr<WriteRequest>& request, bool succeeded,
                      unsigned long error_code, const std::string& config_json) {
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->succeeded = succeeded;
        request->error_code = error_code;
        request->config_json = config_json;
        request->completed = true;
    }
    if (request->wait_for_result) {
        request->completed_event.notify_one();
    }
}

} // namespace

class ConfigWriteCoordinator::Impl {
public:
    ~Impl() { stop(); }

    bool start(ConfigStore* store, ApplyHandler apply_handler, PrepareHandler prepare_handler,
               CancelHandler cancel_handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || !store || !apply_handler ||
            (static_cast<bool>(prepare_handler) != static_cast<bool>(cancel_handler))) {
            return false;
        }
        store_ = store;
        apply_handler_ = std::move(apply_handler);
        prepare_handler_ = std::move(prepare_handler);
        cancel_handler_ = std::move(cancel_handler);
        running_ = true;
        worker_ = std::thread([this]() { run(); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        queue_event_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool submit(cxxime::UserConfigMutationKind kind, const std::string& payload,
                std::string* config_json, unsigned long* error_code) {
        auto request = make_request(kind, payload, true);
        if (!request) {
            if (error_code) {
                *error_code = ERROR_INVALID_PARAMETER;
            }
            return false;
        }
        if (!enqueue(request)) {
            if (error_code) {
                *error_code = ERROR_OPERATION_ABORTED;
            }
            return false;
        }

        std::unique_lock<std::mutex> lock(request->mutex);
        request->completed_event.wait(lock, [&request]() { return request->completed; });
        if (config_json) {
            *config_json = request->config_json;
        }
        if (error_code) {
            *error_code = request->error_code;
        }
        return request->succeeded;
    }

    bool enqueue_patch(const std::string& merge_patch_json) {
        auto request =
            make_request(cxxime::UserConfigMutationKind::kMergePatch, merge_patch_json, false);
        return request && enqueue(request);
    }

    bool snapshot_user_config(std::string* user_config_json, unsigned long* error_code) {
        if (!user_config_json) {
            if (error_code) {
                *error_code = ERROR_INVALID_PARAMETER;
            }
            return false;
        }
        auto request = std::make_shared<WriteRequest>();
        request->snapshot = true;
        request->wait_for_result = true;
        request->enqueued_at = std::chrono::steady_clock::now();
        if (!enqueue(request)) {
            if (error_code) {
                *error_code = ERROR_OPERATION_ABORTED;
            }
            return false;
        }
        std::unique_lock<std::mutex> lock(request->mutex);
        request->completed_event.wait(lock, [&request]() { return request->completed; });
        *user_config_json = request->config_json;
        if (error_code) {
            *error_code = request->error_code;
        }
        return request->succeeded;
    }

private:
    std::shared_ptr<WriteRequest> make_request(cxxime::UserConfigMutationKind kind,
                                               const std::string& payload, bool wait_for_result) {
        if (payload.empty() || payload.size() > cxxime::CONTROL_MAX_PAYLOAD ||
            (kind != cxxime::UserConfigMutationKind::kReplace &&
             kind != cxxime::UserConfigMutationKind::kMergePatch)) {
            return {};
        }
        auto request = std::make_shared<WriteRequest>();
        request->kind = kind;
        request->payload = payload;
        request->enqueued_at = std::chrono::steady_clock::now();
        request->wait_for_result = wait_for_result;
        return request;
    }

    bool enqueue(const std::shared_ptr<WriteRequest>& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return false;
            }
            queue_.push_back(request);
        }
        queue_event_.notify_one();
        return true;
    }

    bool ordering_barrier_is_waiting() const {
        for (const auto& request : queue_) {
            if (request->snapshot ||
                request->kind == cxxime::UserConfigMutationKind::kReplace) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::shared_ptr<WriteRequest>> take_batch(std::unique_lock<std::mutex>* lock) {
        std::vector<std::shared_ptr<WriteRequest>> batch;
        if (queue_.front()->snapshot ||
            queue_.front()->kind == cxxime::UserConfigMutationKind::kReplace) {
            batch.push_back(queue_.front());
            queue_.pop_front();
            return batch;
        }

        const auto deadline = queue_.front()->enqueued_at + kPatchBatchWindow;
        while (running_ && !ordering_barrier_is_waiting() &&
               std::chrono::steady_clock::now() < deadline) {
            queue_event_.wait_until(*lock, deadline);
        }
        while (!queue_.empty() && !queue_.front()->snapshot &&
               queue_.front()->kind == cxxime::UserConfigMutationKind::kMergePatch) {
            batch.push_back(queue_.front());
            queue_.pop_front();
        }
        return batch;
    }

    void run() {
        while (true) {
            std::vector<std::shared_ptr<WriteRequest>> requests;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                queue_event_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) {
                    return;
                }
                requests = take_batch(&lock);
            }

            if (requests.front()->snapshot) {
                complete_request(requests.front(), true, ERROR_SUCCESS,
                                 store_->user_config_json());
                continue;
            }

            std::vector<ConfigMutation> mutations;
            mutations.reserve(requests.size());
            for (const auto& request : requests) {
                mutations.push_back({request->kind, request->payload});
            }

            PreparedConfigUpdate update;
            unsigned long error_code = ERROR_INVALID_DATA;
            bool succeeded = store_->prepare_update(mutations, &update, &error_code);
            bool apply_prepared = false;
            if (succeeded && prepare_handler_) {
                succeeded = prepare_handler_(update.config, &error_code);
                apply_prepared = succeeded;
            }
            if (succeeded) {
                succeeded = store_->commit_update(update, &error_code);
            }
            if (!succeeded && apply_prepared && cancel_handler_) {
                cancel_handler_();
            }
            std::string config_json;
            if (succeeded) {
                apply_handler_(update.config);
                config_json = update.config->to_runtime_json();
                error_code = ERROR_SUCCESS;
            }

            if (!succeeded) {
                cxxime::log_problem("config", "saving the settings failed: " +
                                                  cxxime::windows_error_text(error_code));
            }
            CXXIME_LOG(L"config_write event=batch kind=%u requests=%zu result=%u error=%lu",
                       static_cast<unsigned int>(requests.front()->kind), requests.size(),
                       succeeded ? 1U : 0U, error_code);
            for (const auto& request : requests) {
                complete_request(request, succeeded, error_code, config_json);
            }
        }
    }

    ConfigStore* store_ = nullptr;
    ApplyHandler apply_handler_;
    PrepareHandler prepare_handler_;
    CancelHandler cancel_handler_;
    std::mutex mutex_;
    std::condition_variable queue_event_;
    std::deque<std::shared_ptr<WriteRequest>> queue_;
    bool running_ = false;
    std::thread worker_;
};

ConfigWriteCoordinator::ConfigWriteCoordinator()
    : impl_(new Impl()) {}

ConfigWriteCoordinator::~ConfigWriteCoordinator() = default;

bool ConfigWriteCoordinator::start(ConfigStore* store, ApplyHandler apply_handler,
                                   PrepareHandler prepare_handler, CancelHandler cancel_handler) {
    return impl_->start(store, std::move(apply_handler), std::move(prepare_handler),
                        std::move(cancel_handler));
}

void ConfigWriteCoordinator::stop() { impl_->stop(); }

bool ConfigWriteCoordinator::submit(cxxime::UserConfigMutationKind kind, const std::string& payload,
                                    std::string* config_json, unsigned long* error_code) {
    return impl_->submit(kind, payload, config_json, error_code);
}

bool ConfigWriteCoordinator::enqueue_patch(const std::string& merge_patch_json) {
    return impl_->enqueue_patch(merge_patch_json);
}

bool ConfigWriteCoordinator::snapshot_user_config(std::string* user_config_json,
                                                  unsigned long* error_code) {
    return impl_->snapshot_user_config(user_config_json, error_code);
}
