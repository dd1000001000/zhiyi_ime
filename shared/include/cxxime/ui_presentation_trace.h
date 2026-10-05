// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_UI_PRESENTATION_TRACE_H_
#define CXXIME_UI_PRESENTATION_TRACE_H_

#include <cstdint>

#include <windows.h>

namespace cxxime {

struct UiPresentationTrace {
    std::uint64_t timestamp_100ns = 0;
    std::uint64_t server_received_100ns = 0;
    std::uint64_t server_queue_us = 0;
    std::uint64_t session = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t target_generation = 0;
    std::uint64_t composition_generation = 0;
    bool immersive_mode = false;
    bool tsf_local_candidate = false;
    bool candidate_ownerless = false;
    bool candidate_requested = false;
    bool candidate_visible = false;
    RECT source_caret = {};
    RECT caret = {};
    bool caret_transformed = false;
    RECT candidate_rect = {};
    bool candidate_rect_valid = false;
    UINT candidate_dpi = 0;
};

// Returns the current system time in 100-nanosecond units since the Windows epoch.
std::uint64_t ui_presentation_timestamp_100ns();

// Queues a UI presentation timeline record for server-trace.jsonl.
void enqueue_ui_presentation_trace(const UiPresentationTrace& trace);

} // namespace cxxime

#endif // CXXIME_UI_PRESENTATION_TRACE_H_
