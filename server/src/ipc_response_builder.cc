// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "ipc_response_builder.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <windows.h>

#include <cxxime/candidate.h>
#include <cxxime/candidate_presentation.h>

namespace {

bool response_copy_field(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0 || src.size() >= dst_size) {
        return false;
    }
    memcpy(dst, src.c_str(), src.size() + 1);
    return true;
}

// A candidate annotation cut to `capacity - 1` bytes at a space (pinyin syllables) or at
// least a UTF-8 boundary: an annotation never fails the response.
std::string fit_comment(const std::string& text, size_t capacity) {
    if (text.size() < capacity) {
        return text;
    }
    size_t end = capacity - 1;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    const size_t space = text.rfind(' ', end);
    if (space != std::string::npos && space > 0) {
        end = space;
    }
    return text.substr(0, end);
}

bool is_valid_utf8_field(const std::string& value) {
    if (value.find('\0') != std::string::npos) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool is_valid_utf8_offset(const std::string& value, size_t offset) {
    return offset <= value.size() && (offset == 0 || is_valid_utf8_field(value.substr(0, offset)));
}

} // namespace

void fill_process_response(const ProcessKeyResult& result, cxxime::IPCResponse* response) {
    if (!response) {
        return;
    }
    response->status = result.status;
    response->ascii_mode = !result.ime_status.chinese_mode();
    response->composing = result.composing;
    response->ime_status = result.ime_status;
    response->candidate_revision = result.candidate_revision;

    if (!result.commit_text.empty() &&
        (!is_valid_utf8_field(result.commit_text) ||
         !response_copy_field(response->commit_text, sizeof(response->commit_text),
                              result.commit_text))) {
        response->status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        return;
    }
    if (!result.composing) {
        return;
    }
    std::size_t focused_start = result.focused_preedit_start_bytes;
    std::size_t focused_end = result.focused_preedit_end_bytes;
    if (focused_start == 0 && focused_end == 0) {
        focused_start = result.converted_prefix_bytes;
        focused_end = result.preedit.size();
    }
    constexpr uint32_t kKnownPreeditPresentationFlags =
        cxxime::preedit_presentation_flag(
            cxxime::PreeditPresentationFlag::SyllableBoundaries);
    if (result.converted_prefix_bytes > result.preedit_cursor ||
        focused_start > focused_end || focused_start < result.converted_prefix_bytes ||
        focused_end > result.preedit.size() ||
        !is_valid_utf8_field(result.preedit) ||
        !is_valid_utf8_offset(result.preedit, result.preedit_cursor) ||
        !is_valid_utf8_offset(result.preedit, result.converted_prefix_bytes) ||
        !is_valid_utf8_offset(result.preedit, focused_start) ||
        !is_valid_utf8_offset(result.preedit, focused_end) ||
        (result.preedit_presentation_flags & ~kKnownPreeditPresentationFlags) != 0 ||
        !response_copy_field(response->preedit, sizeof(response->preedit), result.preedit)) {
        response->status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        return;
    }

    response->preedit_cursor = static_cast<uint32_t>(result.preedit_cursor);
    response->converted_prefix_bytes = static_cast<uint32_t>(result.converted_prefix_bytes);
    response->focused_preedit_start_bytes = static_cast<uint32_t>(focused_start);
    response->focused_preedit_end_bytes = static_cast<uint32_t>(focused_end);
    response->preedit_presentation_flags = result.preedit_presentation_flags;
    const cxxime::CandidatePresentationPage& page = result.presentation;
    response->candidate_count =
        static_cast<uint32_t>((std::min)(page.items.size(), cxxime::kCandidateCapacity));
    response->candidate_offset = static_cast<uint32_t>((std::max)(page.page_offset, 0));
    const int returned_end = page.page_offset + static_cast<int>(response->candidate_count);
    response->candidate_known_count = static_cast<uint32_t>(
        (std::max)(page.extent.known_count, returned_end));
    response->candidate_extent_state = page.extent.state;
    response->candidate_extent_complete = page.extent.complete ? 1u : 0u;
    response->candidate_total = static_cast<uint32_t>(
        (std::max)(cxxime::legacy_candidate_total(page.extent, returned_end), 0));
    response->highlighted = page.highlighted >= 0 ? static_cast<uint32_t>(page.highlighted) : 0;
    response->page_current = static_cast<uint32_t>((std::max)(page.page_index + 1, 1));
    const int page_size = page.page_size > 0 ? page.page_size : 9;
    const uint32_t page_total = response->candidate_total > 0
            ? static_cast<uint32_t>((response->candidate_total + page_size - 1) / page_size)
            : 1;
    const uint32_t continuation_page_total =
        cxxime::candidate_extent_may_continue(page.extent, returned_end)
            ? response->page_current + 1
            : response->page_current;
    response->page_total = (std::max)(continuation_page_total, page_total);

    for (uint32_t index = 0; index < response->candidate_count; ++index) {
        const cxxime::CandidatePresentationItem& item = page.items[index];
        if (!cxxime::candidate_text_fits(item.text) || !is_valid_utf8_field(item.text) ||
            !is_valid_utf8_field(item.hint) ||
            !response_copy_field(response->candidates[index], sizeof(response->candidates[index]),
                                 item.text)) {
            response->status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
            return;
        }
        if (!item.hint.empty()) {
            const std::string comment =
                fit_comment(item.hint, sizeof(response->candidate_comments[index]));
            response_copy_field(response->candidate_comments[index],
                                sizeof(response->candidate_comments[index]), comment);
            // Older clients read the short field only (Wubi codes fit).
            response_copy_field(response->candidate_hints[index],
                                sizeof(response->candidate_hints[index]), item.hint);
        }
        if (item.recommended) response->candidate_recommended_mask |= 1u << index;
    }
}
