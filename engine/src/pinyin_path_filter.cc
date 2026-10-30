// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "pinyin_path_filter.h"

#include <algorithm>

namespace cxxime {

namespace {

bool has_consistent_metadata(const SegmentedPath& path) {
    return !path.syllables.empty() && path.syllables.size() == path.spelling_types.size() &&
           path.syllables.size() == path.input_lengths.size();
}

} // namespace

bool path_consumes_entire_input(const std::string& input, const SegmentedPath& path) {
    if (!has_consistent_metadata(path)) {
        return false;
    }
    size_t consumed = 0;
    for (uint16_t length : path.input_lengths) {
        if (length == 0 || length > input.size() - consumed) {
            return false;
        }
        consumed += length;
    }
    return consumed == input.size();
}

// The typed span is the syllable itself, or its ü written as v (jv / qv / xv / yv for ju / qu /
// xu / yu, lve / nve for lue / nue), with the separators typed after it (xi'an).
static bool typed_as_syllable(const std::string& input, size_t position, size_t length,
                              const std::string& syllable) {
    while (length > syllable.size() && input[position + length - 1] == '\'') {
        --length;
    }
    if (length != syllable.size()) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char typed = input[position + i];
        if (typed != syllable[i] && !(typed == 'v' && syllable[i] == 'u')) {
            return false;
        }
    }
    return true;
}

bool is_normal_composition_path(const std::string& input, const SegmentedPath& path) {
    if (!path_consumes_entire_input(input, path) || path.syllables.size() < 2) {
        return false;
    }

    size_t input_position = 0;
    for (size_t i = 0; i < path.syllables.size(); ++i) {
        if (path.spelling_types[i] != kNormalSpelling ||
            !typed_as_syllable(input, input_position, path.input_lengths[i], path.syllables[i])) {
            return false;
        }
        input_position += path.input_lengths[i];
    }
    return true;
}

bool is_complete_normal_path(const std::string& input, const SegmentedPath& path) {
    return path_consumes_entire_input(input, path) && path.syllables.size() >= 2 &&
           std::all_of(path.spelling_types.begin(), path.spelling_types.end(),
                       [](uint8_t type) { return type == kNormalSpelling; });
}

std::string without_separators(const std::string& input) {
    std::string letters = input;
    letters.erase(std::remove(letters.begin(), letters.end(), '\''), letters.end());
    return letters;
}

bool respects_separators(const std::string& input, const std::string& syllables) {
    if (syllables.empty() || input.find('\'') == std::string::npos) {
        return true;
    }
    std::vector<std::string> parts;
    for (std::size_t begin = 0;;) {
        const std::size_t end = syllables.find(':', begin);
        parts.push_back(syllables.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    auto skip_separators = [&](std::size_t p) {
        while (p < input.size() && input[p] == '\'') ++p;
        return p;
    };
    // reached[i]: the input positions where syllable i can start.
    std::vector<std::vector<uint8_t>> reached(parts.size() + 1,
                                              std::vector<uint8_t>(input.size() + 1, 0));
    reached[0][0] = 1;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string& s = parts[i];
        for (std::size_t p = 0; p <= input.size(); ++p) {
            if (!reached[i][p] || p == input.size() || input[p] == '\'') continue;
            std::vector<std::size_t> lengths;
            if (s.size() <= input.size() - p &&
                std::equal(s.begin(), s.end(), input.begin() + static_cast<std::ptrdiff_t>(p),
                           [](char syllable, char typed) {
                               return typed == syllable || (typed == 'v' && syllable == 'u');
                           })) {
                lengths.push_back(s.size());
            }
            if (!s.empty() && input[p] == s[0]) lengths.push_back(1);  // an initial
            if (s.size() > 2 && s[1] == 'h' && p + 1 < input.size() && input[p] == s[0] &&
                input[p + 1] == 'h') {
                lengths.push_back(2);  // zh, ch, sh
            }
            if (s == "er" && input[p] == 'r') lengths.push_back(1);  // 儿 typed as r
            for (std::size_t length : lengths) {
                reached[i + 1][skip_separators(p + length)] = 1;
            }
        }
    }
    return reached[parts.size()][input.size()] != 0;
}

void keep_separator_matches(const std::string& input, std::vector<Candidate>& candidates) {
    if (input.find('\'') == std::string::npos) {
        return;
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const Candidate& candidate) {
                                        return !respects_separators(input, candidate.syllables);
                                    }),
                     candidates.end());
}

bool is_repeated_short_code_path(const std::string& input, const SegmentedPath& path) {
    if (!path_consumes_entire_input(input, path) || path.syllables.size() < 2) {
        return false;
    }

    size_t suffix_begin = path.syllables.size();
    while (suffix_begin > 0 && path.spelling_types[suffix_begin - 1] == kAbbreviation) {
        --suffix_begin;
    }
    if (path.syllables.size() - suffix_begin < 2) {
        return false;
    }
    if (!std::all_of(path.spelling_types.begin(), path.spelling_types.begin() + suffix_begin,
                     [](uint8_t type) { return type == kNormalSpelling; })) {
        return false;
    }

    size_t suffix_input_position = 0;
    for (size_t i = 0; i < suffix_begin; ++i) {
        if (path.input_lengths[i] != path.syllables[i].size() ||
            input.compare(suffix_input_position, path.input_lengths[i], path.syllables[i]) != 0) {
            return false;
        }
        suffix_input_position += path.input_lengths[i];
    }
    if (suffix_input_position >= input.size()) {
        return false;
    }

    const std::string& repeated_syllable = path.syllables[suffix_begin];
    const char repeated_key = input[suffix_input_position];
    for (size_t i = suffix_begin; i < path.syllables.size(); ++i) {
        if (path.input_lengths[i] != 1 || path.syllables[i] != repeated_syllable ||
            suffix_input_position >= input.size() || input[suffix_input_position] != repeated_key) {
            return false;
        }
        ++suffix_input_position;
    }
    return suffix_input_position == input.size();
}

} // namespace cxxime
