// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "ui_presentation_router.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

struct SessionKey {
    cxxime::UiEndpointId endpoint = 0;
    std::uint64_t session_id = 0;

    bool operator==(const SessionKey& other) const noexcept {
        return endpoint == other.endpoint && session_id == other.session_id;
    }
};

struct SessionKeyHash {
    std::size_t operator()(const SessionKey& key) const noexcept {
        const auto endpoint = static_cast<std::size_t>(key.endpoint);
        const auto session = static_cast<std::size_t>(key.session_id);
        return endpoint ^ (session + 0x9e3779b9u + (endpoint << 6) + (endpoint >> 2));
    }
};

struct RoutedSnapshot {
    cxxime::UiPresentationSnapshot snapshot;
    std::uint64_t received_revision = 0;
    std::uint64_t candidate_placement_cycle = 0;
};

struct ActivePresentation {
    SessionKey key;
    cxxime::UiPresentationSnapshot snapshot;
    std::uint64_t candidate_placement_cycle = 0;
};

bool has_flag(const cxxime::UiPresentationSnapshot& snapshot, cxxime::UiSnapshotFlag flag) {
    return (snapshot.flags & cxxime::ui_snapshot_flag(flag)) != 0;
}

bool has_candidate_content(const cxxime::UiPresentationSnapshot& snapshot) {
    return has_flag(snapshot, cxxime::UiSnapshotFlag::kHasCandidates) ||
           has_flag(snapshot, cxxime::UiSnapshotFlag::kHasPreedit);
}

bool is_newer_or_equal(const cxxime::UiPresentationSnapshot& next,
                       const cxxime::UiPresentationSnapshot& current) {
    if (next.session_generation != current.session_generation) {
        return next.session_generation > current.session_generation;
    }
    if (next.target_generation != current.target_generation) {
        return next.target_generation > current.target_generation;
    }
    if (next.composition_generation != current.composition_generation) {
        return next.composition_generation > current.composition_generation;
    }
    return next.presentation_generation >= current.presentation_generation;
}

bool requests_visible_ui(const cxxime::UiPresentationSnapshot& snapshot) {
    // Host ownership is an active presentation state even though CxxIME draws
    // neither window; the controller must receive it to hide stale UI.
    if (snapshot.ownership == cxxime::UiOwnership::kHost) {
        return true;
    }
    return has_flag(snapshot, cxxime::UiSnapshotFlag::kCandidateVisible);
}

bool is_candidate_command(cxxime::UiCommandType type) {
    return type == cxxime::UiCommandType::kSelectCandidate ||
           type == cxxime::UiCommandType::kPagePrevious ||
           type == cxxime::UiCommandType::kPageNext;
}

bool command_is_allowed(const cxxime::UiPresentationSnapshot& snapshot,
                        cxxime::UiCommandType type) {
    if (!is_candidate_command(type)) {
        return requests_visible_ui(snapshot);
    }
    return snapshot.ownership == cxxime::UiOwnership::kExternal &&
           has_flag(snapshot, cxxime::UiSnapshotFlag::kCandidateVisible) &&
           has_flag(snapshot, cxxime::UiSnapshotFlag::kHasCandidates) &&
           !has_flag(snapshot, cxxime::UiSnapshotFlag::kTsfLocalCandidate);
}

bool belongs_to_foreground(const cxxime::UiPresentationSnapshot& snapshot) {
    if (snapshot.target_window == 0) {
        // Some TSF views expose screen coordinates without an HWND. Their focused
        // target has already been validated by the TSF producer.
        return true;
    }
    const HWND target = reinterpret_cast<HWND>(snapshot.target_window);
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindow(target)) {
        return false;
    }
    if (target == foreground || IsChild(foreground, target) || IsChild(target, foreground)) {
        return true;
    }
    const HWND target_root = GetAncestor(target, GA_ROOT);
    const HWND foreground_root = GetAncestor(foreground, GA_ROOT);
    return target_root && target_root == foreground_root;
}

} // namespace

class UiPresentationRouter::Impl {
public:
    bool start(PresentationHandler presentation_handler, const std::wstring& pipe_name) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) {
                return false;
            }
            running_ = true;
            presentation_handler_ = std::move(presentation_handler);
        }
        if (!channel_.start(
                [this](cxxime::UiEndpointId endpoint,
                       const cxxime::UiPresentationSnapshot& snapshot) {
                    on_snapshot(endpoint, snapshot);
                },
                [this](cxxime::UiEndpointId endpoint) { on_disconnect(endpoint); }, pipe_name)) {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            presentation_handler_ = {};
            return false;
        }
        foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                           nullptr, foreground_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!foreground_hook_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                presentation_handler_ = {};
                sessions_.clear();
                active_.reset();
            }
            channel_.stop();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(foreground_listener_mutex_);
            foreground_listener_ = this;
        }
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
        if (foreground_hook_) {
            UnhookWinEvent(foreground_hook_);
            foreground_hook_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(foreground_listener_mutex_);
            if (foreground_listener_ == this) {
                foreground_listener_ = nullptr;
            }
        }
        channel_.stop();

        PresentationHandler handler;
        std::uint64_t router_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.clear();
            active_.reset();
            handler = presentation_handler_;
            presentation_handler_ = {};
            router_revision = ++router_revision_;
        }
        if (handler) {
            handler(0, nullptr, 0, router_revision);
        }
    }

    bool send_command(cxxime::UiEndpointId endpoint, const cxxime::UiCommand& command) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || endpoint == 0 || command.session_id == 0) {
                return false;
            }
            const auto found = sessions_.find({endpoint, command.session_id});
            if (found == sessions_.end()) {
                return false;
            }

            const cxxime::UiPresentationSnapshot& snapshot = found->second.snapshot;
            if (command.session_generation != snapshot.session_generation ||
                command.target_generation != snapshot.target_generation ||
                command.composition_generation != snapshot.composition_generation ||
                command.presentation_generation != snapshot.presentation_generation ||
                !command_is_allowed(snapshot, command.type) ||
                !belongs_to_foreground(snapshot)) {
                return false;
            }
        }
        return channel_.send_command(endpoint, command);
    }

    void reconcile_system_ui(bool clear_presentation) {
        std::vector<std::pair<cxxime::UiEndpointId, cxxime::UiCommand>> commands;
        PresentationHandler handler;
        std::uint64_t router_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            commands.reserve(sessions_.size());
            for (const auto& entry : sessions_) {
                cxxime::UiCommand command;
                command.session_id = entry.second.snapshot.session_id;
                command.session_generation = entry.second.snapshot.session_generation;
                command.type = cxxime::UiCommandType::kRefreshInputIndicator;
                commands.emplace_back(entry.first.endpoint, command);
            }
            if (clear_presentation) {
                sessions_.clear();
                active_.reset();
                handler = presentation_handler_;
                router_revision = ++router_revision_;
            }
        }
        if (handler) {
            handler(0, nullptr, 0, router_revision);
        }
        for (const auto& entry : commands) {
            channel_.send_command(entry.first, entry.second);
        }
    }

private:
    static void CALLBACK foreground_event(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD,
                                          DWORD) {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            std::lock_guard<std::mutex> lock(foreground_listener_mutex_);
            Impl* listener = foreground_listener_;
            if (listener) {
                listener->refresh_active_for_foreground(false);
            }
        }
    }

    bool select_active_for_foreground_locked(cxxime::UiPresentationSnapshot* presentation,
                                             std::uint64_t* candidate_placement_cycle) {
        const RoutedSnapshot* selected = nullptr;
        SessionKey selected_key;
        for (const auto& entry : sessions_) {
            const cxxime::UiPresentationSnapshot& snapshot = entry.second.snapshot;
            if (!requests_visible_ui(snapshot) || !belongs_to_foreground(snapshot)) {
                continue;
            }
            if (!selected || entry.second.received_revision > selected->received_revision) {
                selected = &entry.second;
                selected_key = entry.first;
            }
        }
        if (!selected) {
            return false;
        }
        *presentation = selected->snapshot;
        *candidate_placement_cycle = selected->candidate_placement_cycle;
        active_ = ActivePresentation{selected_key, *presentation, *candidate_placement_cycle};
        return true;
    }

    void refresh_active_for_foreground(bool force_publish) {
        PresentationHandler handler;
        cxxime::UiPresentationSnapshot presentation;
        cxxime::UiEndpointId endpoint = 0;
        bool publish = false;
        bool clear = false;
        std::uint64_t candidate_placement_cycle = 0;
        std::uint64_t router_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }

            const std::optional<ActivePresentation> previous = active_;
            if (select_active_for_foreground_locked(&presentation,
                                                    &candidate_placement_cycle)) {
                publish = force_publish || !previous || !(active_->key == previous->key);
                endpoint = active_->key.endpoint;
            } else {
                active_.reset();
                clear = previous.has_value();
                if (previous) {
                    const auto found = sessions_.find(previous->key);
                    if (found != sessions_.end()) {
                        candidate_placement_cycle = found->second.candidate_placement_cycle;
                    }
                }
            }
            if (publish || clear) {
                router_revision = ++router_revision_;
            }
            handler = presentation_handler_;
        }
        if (!handler) {
            return;
        }
        if (publish) {
            handler(endpoint, &presentation, candidate_placement_cycle, router_revision);
        } else if (clear) {
            handler(0, nullptr, candidate_placement_cycle, router_revision);
        }
    }

    void on_snapshot(cxxime::UiEndpointId endpoint,
                     const cxxime::UiPresentationSnapshot& snapshot) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }

            const SessionKey key{endpoint, snapshot.session_id};
            const auto found = sessions_.find(key);
            if (found != sessions_.end() && !is_newer_or_equal(snapshot, found->second.snapshot)) {
                return;
            }

            if (has_flag(snapshot, cxxime::UiSnapshotFlag::kSessionEnded)) {
                sessions_.erase(key);
            } else {
                std::uint64_t candidate_placement_cycle = 0;
                if (has_candidate_content(snapshot)) {
                    const bool starts_new_cycle =
                        found == sessions_.end() ||
                        found->second.candidate_placement_cycle == 0 ||
                        !has_candidate_content(found->second.snapshot) ||
                        found->second.snapshot.session_generation != snapshot.session_generation ||
                        found->second.snapshot.target_generation != snapshot.target_generation;
                    candidate_placement_cycle = starts_new_cycle
                        ? next_candidate_placement_cycle()
                        : found->second.candidate_placement_cycle;
                }
                if (found == sessions_.end()) {
                    sessions_[key] = {
                        snapshot, ++received_revision_, candidate_placement_cycle};
                } else {
                    found->second.snapshot = snapshot;
                    found->second.received_revision = ++received_revision_;
                    found->second.candidate_placement_cycle = candidate_placement_cycle;
                }
            }
        }
        refresh_active_for_foreground(true);
    }

    void on_disconnect(cxxime::UiEndpointId endpoint) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->first.endpoint == endpoint) {
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        refresh_active_for_foreground(false);
    }

    std::uint64_t next_candidate_placement_cycle() {
        if (++candidate_placement_cycle_revision_ == 0) {
            ++candidate_placement_cycle_revision_;
        }
        return candidate_placement_cycle_revision_;
    }

    std::mutex mutex_;
    bool running_ = false;
    cxxime::UiChannelServer channel_;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    PresentationHandler presentation_handler_;
    std::unordered_map<SessionKey, RoutedSnapshot, SessionKeyHash> sessions_;
    std::optional<ActivePresentation> active_;
    std::uint64_t router_revision_ = 0;
    std::uint64_t received_revision_ = 0;
    std::uint64_t candidate_placement_cycle_revision_ = 0;

    static std::mutex foreground_listener_mutex_;
    static Impl* foreground_listener_;
};

std::mutex UiPresentationRouter::Impl::foreground_listener_mutex_;
UiPresentationRouter::Impl* UiPresentationRouter::Impl::foreground_listener_ = nullptr;

UiPresentationRouter::UiPresentationRouter()
    : impl_(new Impl()) {}

UiPresentationRouter::~UiPresentationRouter() = default;

bool UiPresentationRouter::start(PresentationHandler presentation_handler,
                                 const std::wstring& pipe_name) {
    return impl_->start(std::move(presentation_handler), pipe_name);
}

void UiPresentationRouter::stop() { impl_->stop(); }

bool UiPresentationRouter::send_command(cxxime::UiEndpointId endpoint,
                                        const cxxime::UiCommand& command) {
    return impl_->send_command(endpoint, command);
}

void UiPresentationRouter::reconcile_system_ui(bool clear_presentation) {
    impl_->reconcile_system_ui(clear_presentation);
}
