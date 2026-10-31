// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PUNCT_TYPES_H_
#define CXXIME_PUNCT_TYPES_H_

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cxxime {

enum class PunctType { COMMIT, PAIR, ALTERNATIVES };

// Three fields are mutually exclusive; which one is valid depends on `type`.
struct PunctEntry {
    PunctType type;
    std::string commit;                    // type == COMMIT
    std::vector<std::string> pair;         // type == PAIR: [left, right]
    std::vector<std::string> alternatives; // type == ALTERNATIVES

    bool operator==(const PunctEntry& other) const {
        return type == other.type && commit == other.commit && pair == other.pair &&
               alternatives == other.alternatives;
    }
};

// The keys whose English value, typed during a composition, joins the code as literal text
// (C++, a+b, name@example) instead of committing the candidate before it, unless
// data/punctuation.json says otherwise (issue #43, docs/punctuation.md).
inline constexpr const char kDefaultJoinCodeKeys[] = "~@#$%^&*_+{}|";

// What each printable symbol key types with Chinese punctuation on (punctuation.h): one table
// for half width, one for full width.
struct PunctMapping {
    std::unordered_map<std::string, PunctEntry> half_shape;
    std::unordered_map<std::string, PunctEntry> full_shape;
    std::string join_code = kDefaultJoinCodeKeys;
};

// One key of Settings' custom punctuation (config.json "punctuation"): what is set replaces
// data/punctuation.json, what is not set keeps it.
struct PunctuationKeyConfig {
    char key = 0;
    std::optional<PunctEntry> half_shape;
    std::optional<PunctEntry> full_shape;
    std::optional<bool> join_code;

    bool operator==(const PunctuationKeyConfig& other) const {
        return key == other.key && half_shape == other.half_shape &&
               full_shape == other.full_shape && join_code == other.join_code;
    }
};

}  // namespace cxxime

#endif  // CXXIME_PUNCT_TYPES_H_
