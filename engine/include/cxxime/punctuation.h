// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// The punctuation table (data/punctuation.json) and Settings' custom punctuation (issue #43,
// docs/punctuation.md): what each printable symbol key types with Chinese punctuation on, at
// half and full width, and whether an English value joins the code during a composition.

#ifndef CXXIME_PUNCTUATION_H_
#define CXXIME_PUNCTUATION_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxime/punct_types.h>

namespace cxxime {

// The keys Settings can change: every printable ASCII symbol but /, which opens the symbol menu.
inline constexpr std::string_view kCustomPunctuationKeys = "`~!@#$%^&*()-_=+[]{}\\|;:'\",.<>?";

bool is_custom_punctuation_key(char key);

// A value Settings accepts: 1 to 8 characters of valid UTF-8, no control characters.
bool valid_punctuation_text(const std::string& text);
// COMMIT with a valid text, or PAIR with two.
bool valid_punctuation_entry(const PunctEntry& entry);

bool is_ascii_text(std::string_view text);

// The table in data/punctuation.json: "half_shape" entries, "join_code" (the keys whose English
// value joins the code), and the full-width form of every printable symbol where "full_shape"
// leaves it out (full-shape mode's literal full-width symbols).
std::optional<PunctMapping> parse_punctuation_json(const std::string& json_text);
std::shared_ptr<PunctMapping> load_punctuation_file(const std::string& path);

// The table with Settings' keys applied.
PunctMapping apply_punctuation_config(PunctMapping table,
                                      const std::vector<PunctuationKeyConfig>& keys);

// The text an entry types first (a pair's left half), for showing it.
std::string punctuation_entry_text(const PunctEntry& entry);

// Voice input's text with Chinese punctuation on (server/src/voice_service.cc): each Chinese
// mark the model writes becomes what its key types in the half- or full-width table, an English
// sentence mark followed by a space as with English punctuation (none doubled, none at the end).
std::string voice_punctuation(const std::string& text, const PunctMapping& table, bool full_shape);

}  // namespace cxxime

#endif  // CXXIME_PUNCTUATION_H_
