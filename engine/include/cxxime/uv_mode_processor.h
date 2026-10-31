// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The keys of U mode and V mode (uv_mode.h): the composition is CompositionScheme::kUvMode, its
// input starting with the u or v. In V mode digits and . + - * / ^ % ! ( ) , : are input and the
// letters a, b, c ... choose (when the page is labelled so); in U mode the digits choose and ' cuts
// parts.
#ifndef CXXIME_UV_MODE_PROCESSOR_H_
#define CXXIME_UV_MODE_PROCESSOR_H_

#include <optional>

#include <cxxime/context.h>
#include <cxxime/key_event.h>
#include <cxxime/processor.h>

namespace cxxime {

class UvModeProcessor {
public:
    static bool is_active(const Context& context);
    // V mode: the digits are input, not choices.
    static bool digits_are_input(const Context& context);
    // `ch` is the key as ASCII (normalize_ascii_key), if it is one.
    ProcessResult process_key(const KeyEvent& event, std::optional<char> ch, Context& context) const;
};

}  // namespace cxxime

#endif  // CXXIME_UV_MODE_PROCESSOR_H_
