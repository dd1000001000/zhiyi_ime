// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/ordinary_candidate.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include <cxxime/user_dict_validation.h>

#include "symbol_ranges.inc"

namespace cxxime {
namespace {

// The caller has already checked UTF-8 validity and the candidate byte limit.
std::uint32_t next_codepoint(const std::string& text, std::size_t& offset) {
    const auto lead = static_cast<unsigned char>(text[offset++]);
    if (lead < 0x80) {
        return lead;
    }
    const int count = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
    std::uint32_t point = lead & (0x7f >> count);
    for (int index = 0; index < count; ++index) {
        point = (point << 6) | (static_cast<unsigned char>(text[offset++]) & 0x3f);
    }
    return point;
}

template <std::size_t N>
bool in_ranges(const SymbolRange (&ranges)[N], std::uint32_t point) {
    const auto found = std::lower_bound(
        std::begin(ranges), std::end(ranges), point,
        [](const SymbolRange& range, std::uint32_t value) { return range.last < value; });
    return found != std::end(ranges) && found->first <= point;
}

bool is_symbol(std::uint32_t point) { return in_ranges(kSymbolRanges, point); }

} // namespace

bool is_ordinary_candidate_text(const std::string& text) {
    if (!is_valid_user_dict_text(text)) {
        return false;
    }
    // Emoji are admitted on their own (😄, ❤️, 1️⃣), other symbols only beside text.
    bool has_text = false;
    bool has_emoji = false;
    bool has_symbol = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::uint32_t point = next_codepoint(text, offset);
        // Digits remain text unless they form a keycap sequence.
        if (point >= '0' && point <= '9') {
            std::size_t end = offset;
            if (end < text.size()) {
                std::uint32_t suffix = next_codepoint(text, end);
                if (suffix == 0xfe0f && end < text.size()) {
                    suffix = next_codepoint(text, end);
                }
                if (suffix == 0x20e3) {
                    offset = end;
                    has_emoji = true;
                    continue;
                }
            }
        }
        const bool sequence_component = point == 0x200d || point == 0xfe0e || point == 0xfe0f ||
                                        point == 0x20e3 || (point >= 0xe0020 && point <= 0xe007f);
        if (sequence_component) {
            continue;
        }
        std::size_t end = offset;
        const bool emoji_style = end < text.size() && next_codepoint(text, end) == 0xfe0f;
        if (!is_symbol(point)) {
            has_text = true;
        } else if (in_ranges(kEmojiPresentationRanges, point) ||
                   (emoji_style && in_ranges(kEmojiRanges, point))) {
            has_emoji = true;
        } else {
            has_symbol = true;
        }
    }
    return has_text || (has_emoji && !has_symbol);
}

} // namespace cxxime
