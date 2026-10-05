// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "text_service.h"

#include <cstdio>

#include <cxxime/diagnostics_config.h>

#include "edit_target.h"
#include "tsf_log_writer.h"
#include "tsf_trace.h"

namespace {

const char* bool_json(bool value) {
    return value ? "true" : "false";
}

using GetSystemTimePreciseAsFileTimeFn = VOID(WINAPI*)(LPFILETIME);

std::uint64_t precise_system_time_100ns() {
    static const GetSystemTimePreciseAsFileTimeFn get_precise_time = []() {
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        return kernel32 ? reinterpret_cast<GetSystemTimePreciseAsFileTimeFn>(
                              GetProcAddress(kernel32, "GetSystemTimePreciseAsFileTime"))
                        : nullptr;
    }();
    FILETIME file_time = {};
    if (get_precise_time) {
        get_precise_time(&file_time);
    } else {
        GetSystemTimeAsFileTime(&file_time);
    }
    return (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32) | file_time.dwLowDateTime;
}

bool tsf_should_log_event(bool important) {
    cxxime::DiagnosticsConfig config = cxxime::diagnostics_config();
    if (config.trace_mode == cxxime::DiagnosticTraceMode::kOff)
        return false;
    if (config.trace_mode == cxxime::DiagnosticTraceMode::kError)
        return important;
    return true;
}

void foreground_class_utf8(char* out, int out_size) {
    if (!out || out_size <= 0)
        return;
    out[0] = '\0';

    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return;

    wchar_t class_name[64] = {};
    if (!GetClassNameW(foreground, class_name, ARRAYSIZE(class_name)))
        return;

    WideCharToMultiByte(CP_UTF8, 0, class_name, -1, out, out_size, nullptr, nullptr);
    out[out_size - 1] = '\0';
}

void current_process_utf8(char* out, int out_size) {
    if (!out || out_size <= 0)
        return;
    out[0] = '\0';

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)))
        return;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    WideCharToMultiByte(CP_UTF8, 0, base, -1, out, out_size, nullptr, nullptr);
    out[out_size - 1] = '\0';
}

}  // namespace

const char* TextService::TsfTrace::result_string() const {
    switch (result) {
    case TsfResult::IPC_FAILED: return "ipc_failed";
    case TsfResult::COMMITTED:  return "committed";
    case TsfResult::PREEDIT:    return "preedit";
    case TsfResult::CLEARED:    return "cleared";
    case TsfResult::HANDLED:    return "handled";
    case TsfResult::REJECTED:   return "rejected";
    default: return "unknown";
    }
}

int TextService::TsfTrace::to_json(char* buf, int size) const {
    return snprintf(buf, size,
        "{\"vk\":%u,\"mod\":%u,\"result\":\"%s\",\"cands\":%u,\"preedit_len\":%u,"
        "\"preedit_cursor\":%u,"
        "\"total_us\":%lld,\"ipc_us\":%lld,\"window_us\":%lld,\"slow\":%s}",
        vk, modifiers, result_string(),
        candidate_count, preedit_len, preedit_cursor,
        (long long)total_us, (long long)ipc_us, (long long)window_us,
        slow ? "true" : "false");
}

bool TextService::TsfTrace::should_log() const {
    cxxime::DiagnosticsConfig config = cxxime::diagnostics_config();
    if (config.trace_mode == cxxime::DiagnosticTraceMode::kOff)
        return false;
    if (result == TsfResult::IPC_FAILED) return true;
    if (config.trace_mode == cxxime::DiagnosticTraceMode::kError)
        return false;
    if (config.trace_mode == cxxime::DiagnosticTraceMode::kVerbose)
        return true;
    if (slow) return true;
    return false;
}

void TextService::_enqueue_trace(const TsfTrace& trace) {
    if (!trace.should_log()) return;

    char json[512] = {};
    const int length = trace.to_json(json, sizeof(json));
    cxxime_tsf::enqueue_tsf_log_line(json, length);
}

void TextService::_enqueue_event_trace(const char* event, const char* detail, bool important) {
    if (!tsf_should_log_event(important))
        return;

    char foreground_class[96] = {};
    foreground_class_utf8(foreground_class, sizeof(foreground_class));
    char process_name[MAX_PATH] = {};
    current_process_utf8(process_name, sizeof(process_name));

    char json[512] = {};
    const int length = snprintf(json, sizeof(json),
                                "{\"event\":\"%s\",\"detail\":\"%s\",\"session\":%u,"
                                "\"focused\":%s,\"chinese\":%s,\"caps\":%s,"
                                "\"proc\":\"%s\",\"fg\":\"%s\"}",
                                event ? event : "", detail ? detail : "", _sessionId,
                                bool_json(_inputFocused), bool_json(_chinese_mode),
                                bool_json(_caps_lock), process_name, foreground_class);
    if (length <= 0 || length >= static_cast<int>(sizeof(json)))
        return;
    cxxime_tsf::enqueue_tsf_log_line(json, length);
}

void TextService::_enqueue_ui_presentation_trace(const cxxime::UiPresentationSnapshot& snapshot) {
    const std::uint64_t timestamp_100ns = precise_system_time_100ns();
    char json[512] = {};
    const int length = std::snprintf(
        json, sizeof(json),
        "{\"event\":\"ui.presentation_publish\",\"timestamp_100ns\":%llu,\"session\":%llu,"
        "\"session_generation\":%llu,\"target_generation\":%llu,"
        "\"composition_generation\":%llu,\"candidate_visible\":%s,"
        "\"tsf_local_candidate\":%s}",
        static_cast<unsigned long long>(timestamp_100ns),
        static_cast<unsigned long long>(snapshot.session_id),
        static_cast<unsigned long long>(snapshot.session_generation),
        static_cast<unsigned long long>(snapshot.target_generation),
        static_cast<unsigned long long>(snapshot.composition_generation),
        (snapshot.flags & cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible)) != 0
            ? "true"
            : "false",
        (snapshot.flags & cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate)) != 0
            ? "true"
            : "false");
    if (length > 0 && length < static_cast<int>(sizeof(json))) {
        cxxime_tsf::enqueue_tsf_log_line(json, length);
    }
}

void TextService::_trace_input_decision(const char* block_reason) {
    if (!block_reason) {
        if (!_lastInputBlockReason.empty()) {
            _lastInputBlockReason.clear();
            _enqueue_event_trace("input_context", "allowed");
        }
        return;
    }

    if (_lastInputBlockReason == block_reason)
        return;
    _lastInputBlockReason = block_reason;
    _enqueue_event_trace("input_context", block_reason);
}

void TextService::_reset_trace_composition(const char* reason) {
    cxxime_tsf::trace_composition_end(trace_input_id(), trace_composition_id(), reason);
    _hostTraceSession.reset_composition();
}

void TextService::trace_ui_element_method(const char* element, const char* method, bool important) {
    char detail[96] = {};
    snprintf(detail, sizeof(detail), "%s.%s",
             element ? element : "unknown", method ? method : "unknown");
    _enqueue_event_trace("ui_element_call", detail, important);
}

uint64_t TextService::ensure_trace_composition_id() {
    return _hostTraceSession.ensure_composition();
}

void TextService::trace_caret_event(const char* action,
                                    const char* source,
                                    bool resolved,
                                    const RECT* rect,
                                    HRESULT hr,
                                    bool important) {
    char detail[256] = {};
    if (rect) {
        snprintf(detail, sizeof(detail),
                 "action=%s source=%s resolved=%d rc=%ld,%ld,%ld,%ld hr=0x%08lx "
                 "composing=%d external=%d waiting=%d",
                 action ? action : "unknown", source ? source : "unknown",
                 resolved ? 1 : 0, rect->left, rect->top, rect->right, rect->bottom,
                 static_cast<unsigned long>(hr), _composing ? 1 : 0,
                 _candidatePresentation.external_window_expected() ? 1 : 0,
                 _candidatePresentation.waiting_for_caret() ? 1 : 0);
    } else {
        snprintf(detail, sizeof(detail),
                 "action=%s source=%s resolved=%d hr=0x%08lx composing=%d external=%d waiting=%d",
                 action ? action : "unknown", source ? source : "unknown",
                 resolved ? 1 : 0, static_cast<unsigned long>(hr),
                 _composing ? 1 : 0,
                 _candidatePresentation.external_window_expected() ? 1 : 0,
                 _candidatePresentation.waiting_for_caret() ? 1 : 0);
    }
    _enqueue_event_trace("caret_position", detail, important);
}

void TextService::trace_text_ext_rect(const cxxime_tsf::TextExtRectTrace& trace) {
    char detail[320] = {};
    const int length = snprintf(
        detail, sizeof(detail),
        "branch=%s hr=0x%08lx raw=%ld,%ld,%ld,%ld rc=%ld,%ld,%ld,%ld "
        "resolved=%d clipped=%d view=%p fg=%p owner=%p focus=%p "
        "gui=%d getpos=%d/%d pos=%ld,%ld map=%d",
        trace.branch, static_cast<unsigned long>(trace.text_ext_hr), trace.raw.left, trace.raw.top,
        trace.raw.right, trace.raw.bottom, trace.result.left, trace.result.top, trace.result.right,
        trace.result.bottom, trace.resolved ? 1 : 0, trace.clipped ? 1 : 0,
        static_cast<const void*>(trace.view_hwnd), static_cast<const void*>(trace.foreground_hwnd),
        static_cast<const void*>(trace.caret_hwnd), static_cast<const void*>(trace.focus_hwnd),
        trace.gui_info_ok ? 1 : 0, trace.caret_pos_queried ? 1 : 0, trace.caret_pos_ok ? 1 : 0,
        trace.caret_pos.x, trace.caret_pos.y, trace.caret_map_ok ? 1 : 0);
    if (length > 0 && length < static_cast<int>(sizeof(detail))) {
        _enqueue_event_trace("caret_normalize", detail);
    }
}
