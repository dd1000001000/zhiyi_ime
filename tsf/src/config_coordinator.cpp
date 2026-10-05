// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "config_coordinator.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <json.hpp>

#include <cxxime/control_client.h>
#include <cxxime/diagnostics_config.h>
#include <cxxime/logging.h>

#include "tsf_log_writer.h"

namespace cxxime_tsf {
namespace {

struct Subscriber {
    HWND window = nullptr;
    std::uint32_t id = 0;
};

class ConfigCoordinator {
public:
    ConfigCoordinator() {
        auto config = std::make_shared<cxxime::Config>();
        snapshot_.config = std::move(config);
    }

    ~ConfigCoordinator() { client_.stop(); }

    ConfigSnapshot subscribe(HWND window, std::uint32_t id) {
        bool start_client = false;
        ConfigSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            subscribers_.push_back({window, id});
            start_client = subscribers_.size() == 1;
            snapshot = snapshot_;
        }
        if (start_client) {
            client_.start(
                [this](cxxime::ConfigGeneration generation, const std::string& config_json) {
                    apply_snapshot(generation, config_json);
                });
        }
        return snapshot;
    }

    bool unsubscribe(HWND window, std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                           [window, id](const Subscriber& subscriber) {
                                               return subscriber.window == window &&
                                                      subscriber.id == id;
                                           }),
                            subscribers_.end());
        return subscribers_.empty();
    }

    ConfigSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void patch(const std::string& merge_patch_json) { client_.patch_user_config(merge_patch_json); }

private:
    void apply_snapshot(cxxime::ConfigGeneration generation, const std::string& config_json) {
        auto config = std::make_shared<cxxime::Config>();
        if (!config->load_runtime_json(config_json)) {
            CXXIME_LOG(L"ControlChannel: rejected invalid config snapshot");
            return;
        }

        std::vector<Subscriber> subscribers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation.server_epoch == snapshot_.generation.server_epoch &&
                generation.revision <= snapshot_.generation.revision) {
                return;
            }
            snapshot_.generation = generation;
            snapshot_.config = config;
            subscribers = subscribers_;
        }

        cxxime::set_diagnostics_config(config->diagnostics);
        set_tsf_log_writer_enabled(config->diagnostics.trace_mode !=
                                   cxxime::DiagnosticTraceMode::kOff);
        CXXIME_LOG(L"control event=apply epoch=%llu revision=%llu subscribers=%zu",
                   static_cast<unsigned long long>(generation.server_epoch),
                   static_cast<unsigned long long>(generation.revision), subscribers.size());
        for (const auto& subscriber : subscribers) {
            PostMessageW(subscriber.window, WM_CXXIME_CONFIG_CHANGED,
                         static_cast<WPARAM>(subscriber.id), 0);
        }
    }

    mutable std::mutex mutex_;
    ConfigSnapshot snapshot_;
    std::vector<Subscriber> subscribers_;
    cxxime::ControlClient client_;
};

std::mutex g_coordinator_mutex;
// Normal unsubscribe deletes the coordinator outside the lock. Avoid a static owner that could
// join the client thread from CRT teardown while the Windows loader lock is held.
ConfigCoordinator* g_coordinator = nullptr;
std::atomic<std::uint32_t> g_next_subscription_id{1};

} // namespace

std::uint32_t allocate_config_subscription_id() {
    std::uint32_t id = g_next_subscription_id.fetch_add(1, std::memory_order_relaxed);
    return id == 0 ? g_next_subscription_id.fetch_add(1, std::memory_order_relaxed) : id;
}

ConfigSnapshot subscribe_config_updates(HWND window, std::uint32_t subscription_id) {
    std::lock_guard<std::mutex> lock(g_coordinator_mutex);
    if (!g_coordinator) {
        g_coordinator = new ConfigCoordinator();
    }
    return g_coordinator->subscribe(window, subscription_id);
}

void unsubscribe_config_updates(HWND window, std::uint32_t subscription_id) {
    std::unique_ptr<ConfigCoordinator> retired;
    {
        std::lock_guard<std::mutex> lock(g_coordinator_mutex);
        if (g_coordinator && g_coordinator->unsubscribe(window, subscription_id)) {
            retired.reset(g_coordinator);
            g_coordinator = nullptr;
        }
    }
    if (retired) {
        retired.reset();
        shutdown_tsf_log_writer_if_no_config_subscribers();
    }
}

void shutdown_tsf_log_writer_if_no_config_subscribers() {
    std::lock_guard<std::mutex> lock(g_coordinator_mutex);
    if (!g_coordinator) {
        shutdown_tsf_log_writer();
    }
}

ConfigSnapshot current_config_snapshot() {
    std::lock_guard<std::mutex> lock(g_coordinator_mutex);
    return g_coordinator ? g_coordinator->snapshot() : ConfigSnapshot{};
}

} // namespace cxxime_tsf
