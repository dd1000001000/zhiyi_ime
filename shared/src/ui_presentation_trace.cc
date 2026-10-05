// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/ui_presentation_trace.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <cxxime/query_trace.h>

namespace cxxime {
namespace {

using GetSystemTimePreciseAsFileTimeFn = VOID(WINAPI*)(LPFILETIME);

class JsonLineBuilder {
public:
    JsonLineBuilder(char* buffer, std::size_t capacity)
        : buffer_(buffer)
        , capacity_(capacity) {
        append_raw("{");
    }

    void uint64_field(const char* name, std::uint64_t value) {
        char number[32] = {};
        const int length =
            std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(value));
        field_prefix(name);
        append_formatted(number, length);
    }

    void uint_field(const char* name, UINT value) {
        char number[16] = {};
        const int length = std::snprintf(number, sizeof(number), "%u", value);
        field_prefix(name);
        append_formatted(number, length);
    }

    void bool_field(const char* name, bool value) {
        field_prefix(name);
        append_raw(value ? "true" : "false");
    }

    void text_field(const char* name, const char* value) {
        field_prefix(name);
        append_raw("\"");
        append_raw(value);
        append_raw("\"");
    }

    void rect_field(const char* name, const RECT& rect) {
        char value[96] = {};
        const int length = std::snprintf(value, sizeof(value), "[%ld,%ld,%ld,%ld]", rect.left,
                                         rect.top, rect.right, rect.bottom);
        field_prefix(name);
        append_formatted(value, length);
    }

    int finish() {
        append_raw("}");
        if (!valid_ || length_ >= capacity_) {
            return 0;
        }
        buffer_[length_] = '\0';
        return static_cast<int>(length_);
    }

private:
    void field_prefix(const char* name) {
        if (!first_field_) {
            append_raw(",");
        }
        first_field_ = false;
        append_raw("\"");
        append_raw(name);
        append_raw("\":");
    }

    void append_raw(const char* value) { append_bytes(value, value ? std::strlen(value) : 0); }

    void append_formatted(const char* value, int length) {
        if (length <= 0) {
            valid_ = false;
            return;
        }
        append_bytes(value, static_cast<std::size_t>(length));
    }

    void append_bytes(const char* value, std::size_t length) {
        if (!value || length_ + length >= capacity_) {
            valid_ = false;
            return;
        }
        std::memcpy(buffer_ + length_, value, length);
        length_ += length;
    }

    char* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t length_ = 0;
    bool valid_ = true;
    bool first_field_ = true;
};

} // namespace

std::uint64_t ui_presentation_timestamp_100ns() {
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

void enqueue_ui_presentation_trace(const UiPresentationTrace& trace) {
    char json[1024] = {};
    JsonLineBuilder builder(json, sizeof(json));
    builder.text_field("event", "ui.presentation_applied");
    builder.uint64_field("timestamp_100ns", trace.timestamp_100ns);
    builder.uint64_field("server_received_100ns", trace.server_received_100ns);
    builder.uint64_field("server_queue_us", trace.server_queue_us);
    builder.uint64_field("session", trace.session);
    builder.uint64_field("session_generation", trace.session_generation);
    builder.uint64_field("target_generation", trace.target_generation);
    builder.uint64_field("composition_generation", trace.composition_generation);
    builder.bool_field("immersive_mode", trace.immersive_mode);
    builder.bool_field("tsf_local_candidate", trace.tsf_local_candidate);
    builder.bool_field("candidate_ownerless", trace.candidate_ownerless);
    builder.bool_field("candidate_requested", trace.candidate_requested);
    builder.bool_field("candidate_visible", trace.candidate_visible);
    builder.rect_field("source_caret", trace.source_caret);
    builder.rect_field("caret", trace.caret);
    builder.bool_field("caret_transformed", trace.caret_transformed);
    builder.rect_field("candidate_rect", trace.candidate_rect);
    builder.bool_field("candidate_rect_valid", trace.candidate_rect_valid);
    builder.uint_field("candidate_dpi", trace.candidate_dpi);
    enqueue_server_trace_json(json, builder.finish());
}

} // namespace cxxime
