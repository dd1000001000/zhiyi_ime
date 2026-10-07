// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "engine_response.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <windows.h>

namespace cxxime_tsf {
namespace {

template <std::size_t Capacity>
bool read_field(const char (&field)[Capacity], std::string* value) {
    const std::size_t length = strnlen_s(field, Capacity);
    if (length == Capacity) {
        return false;
    }
    value->assign(field, length);
    return true;
}

bool utf8_prefix_to_utf16(const std::string& text, std::size_t bytes, std::size_t* utf16_units) {
    if (!utf16_units || bytes > text.size() ||
        (bytes < text.size() && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80)) {
        return false;
    }
    if (bytes == 0) {
        *utf16_units = 0;
        return true;
    }
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(bytes), nullptr, 0);
    if (units <= 0) {
        return false;
    }
    *utf16_units = static_cast<std::size_t>(units);
    return true;
}

bool is_utf8_offset(const std::string& text, std::size_t bytes) {
    std::size_t ignored = 0;
    return utf8_prefix_to_utf16(text, bytes, &ignored);
}

bool utf8_to_utf16(const std::string& text, std::wstring* result) {
    if (text.empty()) {
        result->clear();
        return true;
    }
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (units <= 0) {
        return false;
    }
    result->assign(static_cast<std::size_t>(units), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), &(*result)[0], units) == units;
}

} // namespace

bool decode_engine_presentation(const cxxime::IPCResponse& response,
                                DecodedEnginePresentation* presentation) {
    if (!presentation || response.candidate_count > cxxime::kCandidateCapacity) {
        return false;
    }

    std::string preedit;
    if (!read_field(response.preedit, &preedit)) {
        return false;
    }
    constexpr std::uint32_t kSyllableBoundaryFlag = cxxime::preedit_presentation_flag(
        cxxime::PreeditPresentationFlag::SyllableBoundaries);
    if ((response.preedit_presentation_flags & ~kSyllableBoundaryFlag) != 0) {
        return false;
    }
    presentation->has_syllable_boundaries =
        (response.preedit_presentation_flags & kSyllableBoundaryFlag) != 0;
    std::size_t focused_start = response.focused_preedit_start_bytes;
    std::size_t focused_end = response.focused_preedit_end_bytes;
    if (focused_start == 0 && focused_end == 0) {
        if (response.ime_status.input_mode == cxxime::InputMode::PINYIN) {
            focused_start = response.converted_prefix_bytes;
            focused_end = preedit.size();
        } else {
            focused_start = preedit.size();
            focused_end = preedit.size();
        }
    }
    if (response.converted_prefix_bytes > response.preedit_cursor ||
        focused_start < response.converted_prefix_bytes || focused_start > focused_end ||
        focused_end > preedit.size() ||
        !utf8_to_utf16(preedit, &presentation->display_preedit.text)) {
        return false;
    }
    std::string logical_preedit;
    logical_preedit.reserve(preedit.size());
    std::vector<std::size_t> logical_offsets(preedit.size() + 1, 0);
    for (std::size_t index = 0; index < preedit.size(); ++index) {
        logical_offsets[index] = logical_preedit.size();
        if (!presentation->has_syllable_boundaries || index < response.converted_prefix_bytes ||
            preedit[index] != '\'') {
            logical_preedit.push_back(preedit[index]);
        }
    }
    logical_offsets[preedit.size()] = logical_preedit.size();
    if (!is_utf8_offset(preedit, response.preedit_cursor) ||
        !is_utf8_offset(preedit, response.converted_prefix_bytes) ||
        !is_utf8_offset(preedit, focused_start) || !is_utf8_offset(preedit, focused_end) ||
        !utf8_to_utf16(logical_preedit, &presentation->logical_preedit.text) ||
        !utf8_prefix_to_utf16(logical_preedit, logical_offsets[response.preedit_cursor],
                              &presentation->logical_preedit.cursor) ||
        !utf8_prefix_to_utf16(logical_preedit, logical_offsets[response.converted_prefix_bytes],
                              &presentation->logical_preedit.converted_prefix) ||
        !utf8_prefix_to_utf16(logical_preedit, logical_offsets[focused_start],
                              &presentation->logical_preedit.focused_start) ||
        !utf8_prefix_to_utf16(logical_preedit, logical_offsets[focused_end],
                              &presentation->logical_preedit.focused_end) ||
        !utf8_prefix_to_utf16(preedit, response.preedit_cursor,
                              &presentation->display_preedit.cursor) ||
        !utf8_prefix_to_utf16(preedit, response.converted_prefix_bytes,
                              &presentation->display_preedit.converted_prefix) ||
        !utf8_prefix_to_utf16(preedit, focused_start,
                              &presentation->display_preedit.focused_start) ||
        !utf8_prefix_to_utf16(preedit, focused_end,
                              &presentation->display_preedit.focused_end)) {
        return false;
    }
    presentation->display_focused_preedit_start_bytes = focused_start;
    presentation->display_focused_preedit_end_bytes = focused_end;

    cxxime::CandidatePresentationPage page;
    page.page_index = response.page_current > 0 ? static_cast<int>(response.page_current - 1) : 0;
    page.page_offset = static_cast<int>(response.candidate_offset);
    page.page_size = response.candidate_count > 0 ? static_cast<int>(response.candidate_count) : 9;
    page.extent.known_count = static_cast<int>(response.candidate_known_count);
    page.extent.state = response.candidate_extent_state;
    page.extent.complete = response.candidate_extent_complete != 0;
    page.extent.known_count = (std::max)(
        page.extent.known_count,
        page.page_offset + static_cast<int>(response.candidate_count));
    page.highlighted = response.candidate_count > 0 ? static_cast<int>(response.highlighted) : -1;
    page.items.reserve(response.candidate_count);
    for (std::uint32_t index = 0; index < response.candidate_count; ++index) {
        cxxime::CandidatePresentationItem item;
        if (!read_field(response.candidates[index], &item.text) || item.text.empty() ||
            !read_field(response.candidate_comments[index], &item.hint)) {
            return false;
        }
        // An older server sends short hints only.
        if (item.hint.empty() && !read_field(response.candidate_hints[index], &item.hint)) {
            return false;
        }
        std::wstring ignored;
        if (!utf8_to_utf16(item.text, &ignored) || !utf8_to_utf16(item.hint, &ignored)) {
            return false;
        }
        item.recommended = (response.candidate_recommended_mask >> index & 1u) != 0;
        item.learned = (response.candidate_learned_mask >> index & 1u) != 0;
        // A translation that does not read as UTF-8 is left out; the candidate stays.
        std::wstring gloss_check;
        if (!read_field(response.candidate_glosses[index], &item.gloss) ||
            !utf8_to_utf16(item.gloss, &gloss_check)) {
            item.gloss.clear();
        }
        page.items.push_back(std::move(item));
    }
    if (page.highlighted >= static_cast<int>(page.items.size())) {
        return false;
    }
    presentation->candidates = std::move(page);
    return true;
}

bool decode_engine_commit_text(const cxxime::IPCResponse& response, std::wstring* commit_text) {
    if (!commit_text) {
        return false;
    }
    std::string utf8;
    return read_field(response.commit_text, &utf8) && utf8_to_utf16(utf8, commit_text);
}

} // namespace cxxime_tsf
