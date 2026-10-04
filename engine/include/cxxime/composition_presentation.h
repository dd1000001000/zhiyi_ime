// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_COMPOSITION_PRESENTATION_H_
#define CXXIME_COMPOSITION_PRESENTATION_H_

#include <cstddef>
#include <string>

#include <cxxime/composition_state.h>

namespace cxxime {

class PinyinResourceSet;

struct CompositionPresentation {
    std::string logical_preedit;
    std::string display_preedit;
    std::string preview_preedit;
    std::size_t cursor_bytes = 0;
    std::size_t converted_prefix_bytes = 0;
    std::size_t display_cursor_bytes = 0;
    std::size_t display_converted_prefix_bytes = 0;
    std::size_t focused_preedit_start_bytes = 0;
    std::size_t focused_preedit_end_bytes = 0;

    bool fits_transport() const;
};

CompositionPresentation derive_composition_presentation(const CompositionState& state);
CompositionPresentation derive_composition_presentation(const CompositionState& state,
                                                        const PinyinResourceSet* pinyin_resources,
                                                        std::size_t focused_input_bytes,
                                                        bool show_syllable_boundaries,
                                                        const std::string& preferred_syllables = {},
                                                        bool terminal_completion = false,
                                                        bool enable_fuzzy = true,
                                                        bool initials_only = false);

} // namespace cxxime

#endif // CXXIME_COMPOSITION_PRESENTATION_H_
