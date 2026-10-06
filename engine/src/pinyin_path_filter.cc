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
// xu / yu, lve / nve for lue / nue).
static bool typed_as_syllable(const std::string& input, size_t position, size_t length,
                              const std::string& syllable) {
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
