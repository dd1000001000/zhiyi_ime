// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/composition_presentation.h>

#include <algorithm>
#include <vector>

#include <cxxime/input_limits.h>
#include <cxxime/pinyin_resource.h>

namespace cxxime {

bool CompositionPresentation::fits_transport() const {
    return logical_preedit.size() < kCandidateTextCapacity &&
           display_preedit.size() < kCandidateTextCapacity &&
           preview_preedit.size() < kCandidateTextCapacity &&
           cursor_bytes <= logical_preedit.size() &&
           converted_prefix_bytes <= logical_preedit.size() &&
           display_cursor_bytes <= display_preedit.size() &&
           display_converted_prefix_bytes <= display_preedit.size() &&
           focused_preedit_start_bytes <= focused_preedit_end_bytes &&
           focused_preedit_end_bytes <= display_preedit.size();
}

CompositionPresentation derive_composition_presentation(const CompositionState& state) {
    return derive_composition_presentation(state, nullptr, state.active().input.size(), false);
}

CompositionPresentation derive_composition_presentation(const CompositionState& state,
                                                        const PinyinResourceSet* pinyin_resources,
                                                        std::size_t focused_input_bytes,
                                                        bool show_syllable_boundaries,
                                                        const std::string& preferred_syllables,
                                                        bool terminal_completion,
                                                        bool enable_fuzzy,
                                                        bool initials_only,
                                                        uint8_t fuzzy_groups) {
    CompositionPresentation presentation;
    for (const auto& segment : state.converted_segments()) {
        presentation.logical_preedit += segment.text;
    }
    presentation.converted_prefix_bytes = presentation.logical_preedit.size();
    presentation.logical_preedit += state.active().input;
    presentation.preview_preedit = presentation.logical_preedit;
    presentation.cursor_bytes = presentation.converted_prefix_bytes + state.active().cursor;

    std::vector<std::size_t> boundaries;
    if (pinyin_resources && show_syllable_boundaries && !state.active().input.empty()) {
        SyllabifierOptions options;
        options.enable_fuzzy = enable_fuzzy;
        options.fuzzy_groups = fuzzy_groups;
        options.initials_only = initials_only;
        options.enable_terminal_completion = terminal_completion;
        options.collect_path_metadata = true;
        const SegmentResult segmented =
            pinyin_resources->segment(state.active().input, nullptr, options);
        std::vector<std::string> preferred_path;
        std::size_t begin = 0;
        while (begin < preferred_syllables.size()) {
            const std::size_t end = preferred_syllables.find(':', begin);
            preferred_path.push_back(preferred_syllables.substr(begin, end - begin));
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        auto has_consistent_metadata = [&](const SegmentedPath& candidate) {
            if (candidate.syllables.empty() ||
                candidate.syllables.size() != candidate.spelling_types.size() ||
                candidate.syllables.size() != candidate.input_lengths.size()) {
                return false;
            }
            std::size_t consumed = 0;
            for (uint16_t length : candidate.input_lengths) {
                consumed += length;
            }
            return consumed == state.active().input.size();
        };
        auto contains_focus_boundary = [&](const SegmentedPath& candidate) {
            if (focused_input_bytes >= state.active().input.size()) {
                return true;
            }
            std::size_t consumed = 0;
            for (std::size_t index = 0; index + 1 < candidate.input_lengths.size(); ++index) {
                consumed += candidate.input_lengths[index];
                if (consumed == focused_input_bytes) {
                    return true;
                }
            }
            return false;
        };
        auto is_natural = [&](const SegmentedPath& candidate) {
            return has_consistent_metadata(candidate) &&
                   std::all_of(candidate.spelling_types.begin(), candidate.spelling_types.end(),
                               [](uint8_t type) { return type <= kFuzzySpelling; });
        };
        auto matches_preferred = [&](const SegmentedPath& candidate) {
            if (preferred_path.empty() || preferred_path.size() > candidate.syllables.size() ||
                !std::equal(preferred_path.begin(), preferred_path.end(),
                            candidate.syllables.begin())) {
                return false;
            }
            if (focused_input_bytes >= state.active().input.size()) {
                return preferred_path.size() == candidate.syllables.size();
            }
            std::size_t consumed = 0;
            for (std::size_t index = 0; index < preferred_path.size(); ++index) {
                consumed += candidate.input_lengths[index];
            }
            return consumed == focused_input_bytes;
        };
        auto path = std::find_if(segmented.paths.begin(), segmented.paths.end(),
                                 [&](const SegmentedPath& candidate) {
                                     return is_natural(candidate) && matches_preferred(candidate);
                                 });
        if (path == segmented.paths.end()) {
            path = std::find_if(segmented.paths.begin(), segmented.paths.end(),
                                [&](const SegmentedPath& candidate) {
                                    return has_consistent_metadata(candidate) &&
                                           matches_preferred(candidate);
                                });
        }
        if (path == segmented.paths.end()) {
            path =
                std::find_if(segmented.paths.begin(), segmented.paths.end(),
                             [&](const SegmentedPath& candidate) {
                                 return is_natural(candidate) && contains_focus_boundary(candidate);
                             });
        }
        if (path == segmented.paths.end()) {
            path = std::find_if(segmented.paths.begin(), segmented.paths.end(),
                                [&](const SegmentedPath& candidate) {
                                    return has_consistent_metadata(candidate) &&
                                           contains_focus_boundary(candidate);
                                });
        }
        if (path == segmented.paths.end() && focused_input_bytes < state.active().input.size()) {
            focused_input_bytes = state.active().input.size();
            path = std::find_if(segmented.paths.begin(), segmented.paths.end(), is_natural);
            if (path == segmented.paths.end()) {
                path = std::find_if(segmented.paths.begin(), segmented.paths.end(),
                                    has_consistent_metadata);
            }
        }
        if (path != segmented.paths.end()) {
            std::size_t boundary = 0;
            for (std::size_t index = 0; index + 1 < path->input_lengths.size(); ++index) {
                boundary += path->input_lengths[index];
                boundaries.push_back(boundary);
            }
        }
    }

    presentation.display_preedit.reserve(presentation.logical_preedit.size() + boundaries.size());
    presentation.display_preedit.append(presentation.logical_preedit, 0,
                                        presentation.converted_prefix_bytes);
    for (std::size_t index = 0; index < state.active().input.size(); ++index) {
        if (std::binary_search(boundaries.begin(), boundaries.end(), index)) {
            presentation.display_preedit.push_back('\'');
        }
        presentation.display_preedit.push_back(state.active().input[index]);
    }

    if (presentation.display_preedit.size() >= kCandidateTextCapacity) {
        boundaries.clear();
        presentation.display_preedit = presentation.logical_preedit;
    }
    auto mapped_active_offset = [&](std::size_t offset, bool include_boundary) {
        const auto end = include_boundary
                             ? std::upper_bound(boundaries.begin(), boundaries.end(), offset)
                             : std::lower_bound(boundaries.begin(), boundaries.end(), offset);
        return presentation.converted_prefix_bytes + offset +
               static_cast<std::size_t>(end - boundaries.begin());
    };

    focused_input_bytes = (std::min)(focused_input_bytes, state.active().input.size());
    presentation.display_converted_prefix_bytes = presentation.converted_prefix_bytes;
    presentation.display_cursor_bytes = mapped_active_offset(state.active().cursor, true);
    presentation.focused_preedit_start_bytes = presentation.display_converted_prefix_bytes;
    presentation.focused_preedit_end_bytes = mapped_active_offset(
        focused_input_bytes, focused_input_bytes == state.active().input.size());
    return presentation;
}

} // namespace cxxime
