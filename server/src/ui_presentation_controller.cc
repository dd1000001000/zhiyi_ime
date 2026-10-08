// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "ui_presentation_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>
#include <objbase.h>

#include <cxxime/candidate_presentation.h>
#include <cxxime/candidate_window.h>
#include <cxxime/ui_presentation_trace.h>
#include <cxxime/window_position.h>

#include "ui_candidate_page_conversion.h"

namespace {

constexpr DWORD kUiThreadStartTimeoutMs = 5000;

bool ui_timeline_enabled(const cxxime::Config& config) {
    return config.diagnostics.trace_mode == cxxime::DiagnosticTraceMode::kNormal ||
           config.diagnostics.trace_mode == cxxime::DiagnosticTraceMode::kVerbose;
}

bool has_flag(const cxxime::UiPresentationSnapshot& snapshot, cxxime::UiSnapshotFlag flag) {
    return (snapshot.flags & cxxime::ui_snapshot_flag(flag)) != 0;
}

HWND local_candidate_window(const cxxime::UiPresentationSnapshot& snapshot) {
    const HWND candidate = reinterpret_cast<HWND>(snapshot.local_candidate_window);
    if (!candidate || !IsWindowVisible(candidate) ||
        (GetWindowLongPtrW(candidate, GWL_STYLE) & WS_CHILD) != 0 ||
        (GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        return nullptr;
    }
    wchar_t class_name[64] = {};
    if (!GetClassNameW(candidate, class_name, 64) ||
        lstrcmpW(class_name, L"ZhiyiIMECandidateWindow") != 0) {
        return nullptr;
    }
    const HWND owner = GetWindow(candidate, GW_OWNER);
    DWORD candidate_process = 0;
    GetWindowThreadProcessId(candidate, &candidate_process);
    if (!candidate_process || !IsWindow(owner)) {
        return nullptr;
    }
    const HWND target = reinterpret_cast<HWND>(snapshot.target_window);
    if (target) {
        DWORD target_process = 0;
        GetWindowThreadProcessId(target, &target_process);
        if (target_process != candidate_process ||
            (owner != target && owner != GetAncestor(target, GA_ROOT))) {
            return nullptr;
        }
    } else {
        DWORD owner_process = 0;
        GetWindowThreadProcessId(owner, &owner_process);
        if (owner_process != candidate_process) {
            return nullptr;
        }
    }
    // With no TSF view HWND, the local presenter may have used GetFocus as owner.
    return candidate;
}

bool transform_caret_to_physical(std::uint64_t source_window, RECT* caret) {
    const HWND hwnd = reinterpret_cast<HWND>(source_window);
    RECT transformed = {};
    if (caret && cxxime::logical_screen_rect_to_physical(hwnd, *caret, &transformed)) {
        *caret = transformed;
        return true;
    }

    // UWP input sites can reject cross-process DPI conversion even while their
    // foreground root remains a valid screen-coordinate conversion target.
    const HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (root == hwnd || !caret ||
        !cxxime::logical_screen_rect_to_physical(root, *caret, &transformed)) {
        return false;
    }
    *caret = transformed;
    return true;
}

std::string packet_text(const char* text, std::uint32_t length, std::size_t capacity) {
    const std::size_t safe_length = (std::min)(static_cast<std::size_t>(length), capacity);
    return std::string(text, text + safe_length);
}

} // namespace

class UiPresentationController::Impl {
public:
    struct RoutedPresentation {
        cxxime::UiEndpointId endpoint = 0;
        cxxime::UiPresentationSnapshot snapshot;
        std::uint64_t received_time_100ns = 0;
    };

    struct AppliedPresentation {
        bool candidate_requested = false;
        bool candidate_visible = false;
        bool candidate_ownerless = false;
        RECT source_caret = {};
        RECT caret = {};
        bool caret_transformed = false;
    };

    ~Impl() { stop(); }

    bool start(const std::shared_ptr<const cxxime::Config>& config,
               CommandHandler command_handler) {
        if (!config) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (running_) {
            return false;
        }
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        update_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_ || !update_event_) {
            close_events();
            return false;
        }

        running_ = true;
        initialized_ = false;
        initialization_succeeded_ = false;
        pending_config_ = config;
        trace_enabled_.store(ui_timeline_enabled(*config), std::memory_order_relaxed);
        command_handler_ = std::move(command_handler);
        try {
            thread_ = std::thread(&Impl::run, this);
        } catch (...) {
            running_ = false;
            command_handler_ = {};
            close_events();
            return false;
        }

        if (!initialized_cv_.wait_for(lock, std::chrono::milliseconds(kUiThreadStartTimeoutMs),
                                      [this]() { return initialized_; })) {
            lock.unlock();
            stop();
            return false;
        }
        const bool succeeded = initialization_succeeded_;
        lock.unlock();
        if (!succeeded) {
            stop();
        }
        return succeeded;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        SetEvent(stop_event_);
        if (thread_.joinable()) {
            thread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pending_snapshot_.reset();
        rendered_presentation_.reset();
        pending_candidate_placement_cycle_ = 0;
        applied_candidate_placement_cycle_ = 0;
        clear_visible_candidate_count();
        pending_config_.reset();
        command_handler_ = {};
        close_events();
    }

    void present(cxxime::UiEndpointId endpoint, const cxxime::UiPresentationSnapshot* snapshot,
                 std::uint64_t candidate_placement_cycle,
                 std::uint64_t router_revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || router_revision <= pending_router_revision_) {
            return;
        }
        pending_router_revision_ = router_revision;
        if (snapshot) {
            pending_snapshot_ = RoutedPresentation{
                endpoint, *snapshot,
                trace_enabled_.load(std::memory_order_relaxed)
                    ? cxxime::ui_presentation_timestamp_100ns()
                    : 0};
        } else {
            pending_snapshot_.reset();
        }
        pending_candidate_placement_cycle_ = candidate_placement_cycle;
        ++presentation_revision_;
        SetEvent(update_event_);
    }

    void add_glosses(const std::vector<std::pair<std::string, std::string>>& glosses) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !pending_snapshot_) {
            return;
        }
        // The latest snapshot stays in pending_snapshot_ after it is shown: filling it and
        // counting a new revision shows it again.
        cxxime::UiPresentationSnapshot& snapshot = pending_snapshot_->snapshot;
        bool changed = false;
        const std::uint32_t count = (std::min)(snapshot.candidate_page.count,
                                               static_cast<std::uint32_t>(cxxime::kCandidateCapacity));
        for (std::uint32_t i = 0; i < count; ++i) {
            cxxime::UiCandidateGloss& gloss = snapshot.candidate_glosses[i];
            if (gloss.length != 0) continue;
            const cxxime::UiCandidate& candidate = snapshot.candidate_page.candidates[i];
            const std::string text = packet_text(candidate.text, candidate.text_length,
                                                 sizeof(candidate.text));
            for (const auto& [source, translation] : glosses) {
                if (source != text) continue;
                const std::string fitted = cxxime::fit_candidate_gloss(translation, sizeof(gloss.text));
                std::memcpy(gloss.text, fitted.data(), fitted.size());
                gloss.text[fitted.size()] = '\0';
                gloss.length = static_cast<std::uint32_t>(fitted.size());
                changed = true;
                break;
            }
        }
        if (changed) {
            ++presentation_revision_;
            SetEvent(update_event_);
        }
    }

    void update_config(const std::shared_ptr<const cxxime::Config>& config) {
        if (!config) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        pending_config_ = config;
        trace_enabled_.store(ui_timeline_enabled(*config), std::memory_order_relaxed);
        ++config_revision_;
        SetEvent(update_event_);
    }

    std::uint32_t visible_candidate_count(
        std::uint32_t session_id, const cxxime::CandidateUiContext& context) const {
        std::lock_guard<std::mutex> lock(visible_candidate_mutex_);
        if (visible_candidate_session_id_ != session_id ||
            visible_candidate_session_generation_ != context.session_generation ||
            visible_candidate_target_generation_ != context.target_generation ||
            visible_candidate_composition_generation_ != context.composition_generation ||
            visible_candidate_presentation_generation_ != context.presentation_generation) {
            return 0;
        }
        return visible_candidate_count_;
    }

private:
    void close_events() {
        if (update_event_) {
            CloseHandle(update_event_);
            update_event_ = nullptr;
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
    }

    void dispatch_command(cxxime::UiCommandType type, std::uint32_t candidate_index = 0,
                          std::uint32_t value = 0) {
        if (!rendered_presentation_) {
            return;
        }
        CommandHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = command_handler_;
        }
        if (handler) {
            const cxxime::UiPresentationSnapshot& snapshot = rendered_presentation_->snapshot;
            cxxime::UiCommand command;
            command.session_id = snapshot.session_id;
            command.session_generation = snapshot.session_generation;
            command.target_generation = snapshot.target_generation;
            command.composition_generation = snapshot.composition_generation;
            command.presentation_generation = snapshot.presentation_generation;
            command.type = type;
            command.candidate_index = candidate_index;
            command.value = value;
            command.candidate_revision = snapshot.candidate_revision;
            handler(rendered_presentation_->endpoint, command);
        }
    }

    void configure_window_callbacks() {
        candidate_window_.set_candidate_selection_callback([this](std::size_t index) {
            dispatch_command(cxxime::UiCommandType::kSelectCandidate,
                             static_cast<std::uint32_t>(index));
        });
        candidate_window_.set_page_callback([this](cxxime::CandidatePageDirection direction) {
            dispatch_command(direction == cxxime::CandidatePageDirection::Previous
                                 ? cxxime::UiCommandType::kPagePrevious
                                 : cxxime::UiCommandType::kPageNext);
        });
        candidate_window_.set_layout_changed_callback([this]() {
            if (applying_presentation_ || !rendered_presentation_ ||
                !candidate_window_.is_visible()) {
                return;
            }
            store_visible_candidate_count(rendered_presentation_->snapshot);
        });
    }

    void apply_config(const std::shared_ptr<const cxxime::Config>& config) {
        if (!config) {
            return;
        }
        current_config_ = config;
        candidate_window_.set_config(*current_config_);
        candidate_window_.set_layout(current_config_->layout);
    }

    void apply_presentation(const std::optional<RoutedPresentation>& presentation,
                            std::uint64_t candidate_placement_cycle) {
        if (candidate_placement_cycle != applied_candidate_placement_cycle_) {
            candidate_window_.reset_placement();
            applied_candidate_placement_cycle_ = candidate_placement_cycle;
        }
        if (!presentation) {
            candidate_window_.hide();
            clear_visible_candidate_count();
            rendered_presentation_.reset();
            return;
        }

        const cxxime::UiPresentationSnapshot& current = presentation->snapshot;
        AppliedPresentation applied;
        applied.source_caret = current.caret;
        applied.caret = current.caret;

        applied.candidate_requested =
            current.ownership == cxxime::UiOwnership::kExternal &&
            has_flag(current, cxxime::UiSnapshotFlag::kCandidateVisible) &&
            has_flag(current, cxxime::UiSnapshotFlag::kHasCaret) &&
            !has_flag(current, cxxime::UiSnapshotFlag::kTsfLocalCandidate);
        applied.candidate_visible = applied.candidate_requested;
        if (applied.candidate_visible) {
            applied.caret_transformed =
                transform_caret_to_physical(current.target_window, &applied.caret);
            applied.candidate_visible = applied.caret_transformed;
        }
        if (!applied.candidate_visible) {
            candidate_window_.hide();
            clear_visible_candidate_count();
            rendered_presentation_.reset();
            trace_presentation(*presentation, applied);
            return;
        }

        // Bind the popup to the active TSF view before showing it so ordinary
        // desktop hosts keep the candidate window in their owner hierarchy.
        const HWND candidate_owner = reinterpret_cast<HWND>(current.target_window);
        bool owner_binding_fallback = false;
        if (!candidate_window_.ensure_created_with_ownerless_fallback(
                candidate_owner, &owner_binding_fallback)) {
            applied.candidate_visible = false;
            candidate_window_.hide();
            clear_visible_candidate_count();
            rendered_presentation_.reset();
            trace_presentation(*presentation, applied);
            return;
        }
        applied.candidate_ownerless = candidate_owner == nullptr || owner_binding_fallback;
        candidate_window_.set_page_info(static_cast<int>(current.candidate_page.page_current),
                                        static_cast<int>(current.candidate_page.page_total));
        if (has_flag(current, cxxime::UiSnapshotFlag::kHasPreedit)) {
            std::size_t focused_start = current.focused_preedit_start_bytes;
            std::size_t focused_end = current.focused_preedit_end_bytes;
            if (focused_start == 0 && focused_end == 0) {
                if (current.ime_status.input_mode == cxxime::InputMode::PINYIN) {
                    focused_start = current.converted_prefix_bytes;
                    focused_end = current.preedit_length;
                } else {
                    focused_start = current.preedit_length;
                    focused_end = current.preedit_length;
                }
            }
            candidate_window_.set_preedit(
                packet_text(current.preedit, current.preedit_length, sizeof(current.preedit)),
                static_cast<std::size_t>(current.preedit_cursor),
                static_cast<std::size_t>(current.converted_prefix_bytes),
                focused_start, focused_end,
                (current.preedit_presentation_flags &
                 cxxime::preedit_presentation_flag(
                     cxxime::PreeditPresentationFlag::SyllableBoundaries)) != 0);
        } else {
            candidate_window_.set_preedit({});
        }
        candidate_window_.move_to_caret(applied.caret);
        candidate_window_.update(cxxime::candidate_page_from_snapshot(current));
        candidate_window_.show();
        applied.candidate_visible = candidate_window_.is_visible();
        if (!applied.candidate_visible) {
            candidate_window_.hide();
            clear_visible_candidate_count();
            rendered_presentation_.reset();
            trace_presentation(*presentation, applied);
            return;
        }
        rendered_presentation_ = presentation;
        store_visible_candidate_count(current);
        trace_presentation(*presentation, applied);
    }

    void trace_presentation(const RoutedPresentation& presentation,
                            const AppliedPresentation& applied) {
        const cxxime::UiPresentationSnapshot& snapshot = presentation.snapshot;
        if (!trace_enabled_.load(std::memory_order_relaxed) ||
            presentation.received_time_100ns == 0) {
            return;
        }

        RECT candidate_rect = {};
        const bool candidate_rect_valid =
            applied.candidate_visible && candidate_window_.get_window_rect(&candidate_rect);
        cxxime::UiPresentationTrace trace;
        const std::uint64_t applied_time_100ns = cxxime::ui_presentation_timestamp_100ns();
        trace.timestamp_100ns = applied_time_100ns;
        trace.server_received_100ns = presentation.received_time_100ns;
        trace.server_queue_us =
            applied_time_100ns >= trace.server_received_100ns
                ? (applied_time_100ns - trace.server_received_100ns) / 10
                : 0;
        trace.session = snapshot.session_id;
        trace.session_generation = snapshot.session_generation;
        trace.target_generation = snapshot.target_generation;
        trace.composition_generation = snapshot.composition_generation;
        trace.immersive_mode =
            has_flag(snapshot, cxxime::UiSnapshotFlag::kImmersiveMode);
        trace.tsf_local_candidate =
            has_flag(snapshot, cxxime::UiSnapshotFlag::kTsfLocalCandidate);
        trace.candidate_ownerless =
            applied.candidate_ownerless && applied.candidate_visible;
        trace.candidate_requested = applied.candidate_requested;
        trace.candidate_visible = applied.candidate_visible;
        trace.source_caret = applied.source_caret;
        trace.caret = applied.caret;
        trace.caret_transformed = applied.caret_transformed;
        trace.candidate_rect = candidate_rect;
        trace.candidate_rect_valid = candidate_rect_valid;
        trace.candidate_dpi = candidate_window_.dpi();
        cxxime::enqueue_ui_presentation_trace(trace);
    }

    void clear_visible_candidate_count() {
        std::lock_guard<std::mutex> lock(visible_candidate_mutex_);
        visible_candidate_session_id_ = 0;
        visible_candidate_session_generation_ = 0;
        visible_candidate_target_generation_ = 0;
        visible_candidate_composition_generation_ = 0;
        visible_candidate_presentation_generation_ = 0;
        visible_candidate_count_ = 0;
    }

    void store_visible_candidate_count(const cxxime::UiPresentationSnapshot& snapshot) {
        const std::uint32_t visible_count =
            static_cast<std::uint32_t>(candidate_window_.visible_candidate_count());
        if (visible_count == 0) {
            clear_visible_candidate_count();
            return;
        }

        std::lock_guard<std::mutex> lock(visible_candidate_mutex_);
        visible_candidate_session_id_ = snapshot.session_id;
        visible_candidate_session_generation_ = snapshot.session_generation;
        visible_candidate_target_generation_ = snapshot.target_generation;
        visible_candidate_composition_generation_ = snapshot.composition_generation;
        visible_candidate_presentation_generation_ = snapshot.presentation_generation;
        visible_candidate_count_ = visible_count;
    }

    void consume_pending_updates() {
        std::shared_ptr<const cxxime::Config> config;
        std::optional<RoutedPresentation> snapshot;
        std::uint64_t config_revision = 0;
        std::uint64_t presentation_revision = 0;
        std::uint64_t candidate_placement_cycle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ResetEvent(update_event_);
            config = pending_config_;
            snapshot = pending_snapshot_;
            config_revision = config_revision_;
            presentation_revision = presentation_revision_;
            candidate_placement_cycle = pending_candidate_placement_cycle_;
        }
        const bool config_changed = config_revision != applied_config_revision_;
        // Window geometry callbacks must not reconcile against the previous target
        // while configuration or presentation for a new target is being applied.
        applying_presentation_ = true;
        if (config_changed) {
            apply_config(config);
            applied_config_revision_ = config_revision;
        }
        if (config_changed || presentation_revision != applied_presentation_revision_) {
            apply_presentation(snapshot, candidate_placement_cycle);
            applied_presentation_revision_ = presentation_revision;
        }
        applying_presentation_ = false;
    }

    void run() {
        const DPI_AWARENESS_CONTEXT previous_dpi_context =
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::shared_ptr<const cxxime::Config> initial_config;
        std::uint64_t initial_config_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initial_config = pending_config_;
            initial_config_revision = config_revision_;
        }

        const bool candidate_created =
            initial_config && candidate_window_.create(nullptr, *initial_config);
        if (candidate_created) {
            configure_window_callbacks();
            candidate_window_.hide();
            apply_config(initial_config);
            applied_config_revision_ = initial_config_revision;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialization_succeeded_ = candidate_created;
            initialized_ = true;
        }
        initialized_cv_.notify_all();

        if (candidate_created) {
            HANDLE handles[] = {stop_event_, update_event_};
            bool stopping = false;
            while (!stopping) {
                const DWORD result =
                    MsgWaitForMultipleObjects(2, handles, FALSE, INFINITE, QS_ALLINPUT);
                if (result == WAIT_OBJECT_0) {
                    stopping = true;
                } else if (result == WAIT_OBJECT_0 + 1) {
                    consume_pending_updates();
                } else if (result == WAIT_OBJECT_0 + 2) {
                    MSG message;
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) {
                            stopping = true;
                            break;
                        }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                } else {
                    stopping = true;
                }
            }
        }

        candidate_window_.destroy();
        current_config_.reset();
        if (SUCCEEDED(com_result)) {
            CoUninitialize();
        }
        if (previous_dpi_context) {
            SetThreadDpiAwarenessContext(previous_dpi_context);
        }
    }

    std::mutex mutex_;
    std::condition_variable initialized_cv_;
    bool running_ = false;
    bool initialized_ = false;
    bool initialization_succeeded_ = false;
    std::atomic<bool> trace_enabled_{false};
    HANDLE stop_event_ = nullptr;
    HANDLE update_event_ = nullptr;
    std::thread thread_;
    CommandHandler command_handler_;
    std::shared_ptr<const cxxime::Config> pending_config_;
    std::shared_ptr<const cxxime::Config> current_config_;
    std::optional<RoutedPresentation> pending_snapshot_;
    std::optional<RoutedPresentation> rendered_presentation_;
    bool applying_presentation_ = false;
    std::uint64_t config_revision_ = 1;
    std::uint64_t presentation_revision_ = 0;
    std::uint64_t applied_config_revision_ = 0;
    std::uint64_t applied_presentation_revision_ = 0;
    std::uint64_t pending_router_revision_ = 0;
    std::uint64_t pending_candidate_placement_cycle_ = 0;
    std::uint64_t applied_candidate_placement_cycle_ = 0;
    mutable std::mutex visible_candidate_mutex_;
    std::uint64_t visible_candidate_session_id_ = 0;
    std::uint64_t visible_candidate_session_generation_ = 0;
    std::uint64_t visible_candidate_target_generation_ = 0;
    std::uint64_t visible_candidate_composition_generation_ = 0;
    std::uint64_t visible_candidate_presentation_generation_ = 0;
    std::uint32_t visible_candidate_count_ = 0;
    cxxime::CandidateWindow candidate_window_;

};

UiPresentationController::UiPresentationController()
    : impl_(new Impl()) {}

UiPresentationController::~UiPresentationController() = default;

bool UiPresentationController::start(const std::shared_ptr<const cxxime::Config>& config,
                                     CommandHandler command_handler) {
    return impl_->start(config, std::move(command_handler));
}

void UiPresentationController::stop() { impl_->stop(); }

void UiPresentationController::present(cxxime::UiEndpointId endpoint,
                                       const cxxime::UiPresentationSnapshot* snapshot,
                                       std::uint64_t candidate_placement_cycle,
                                       std::uint64_t router_revision) {
    impl_->present(endpoint, snapshot, candidate_placement_cycle, router_revision);
}

void UiPresentationController::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    impl_->update_config(config);
}

void UiPresentationController::add_glosses(
    const std::vector<std::pair<std::string, std::string>>& glosses) {
    impl_->add_glosses(glosses);
}

std::uint32_t UiPresentationController::visible_candidate_count(
    std::uint32_t session_id, const cxxime::CandidateUiContext& context) const {
    return impl_->visible_candidate_count(session_id, context);
}
