// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PINYIN_RESOURCE_H_
#define CXXIME_PINYIN_RESOURCE_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cxxime/pinyin_scheme.h>
#include <cxxime/spellings_index.h>
#include <cxxime/syllabifier.h>

namespace cxxime {

enum class PinyinSpellingRequirement {
    kRequired,
    kOptionalForFullPinyin,
};

struct PinyinQueryPolicy {
    bool enable_fuzzy = true;
    uint8_t fuzzy_groups = kAllFuzzyGroups;  // enabled FuzzyGroup bits
    // Initials mode: every letter is the initial of one syllable (zh/ch/sh count as one).
    bool initials_only = false;
};

class PinyinResourceSet final {
public:
    static std::shared_ptr<const PinyinResourceSet>
    create(std::string scheme_id, PinyinSchemeKind kind, const std::string& spellings_path,
           PinyinSpellingRequirement requirement = PinyinSpellingRequirement::kRequired);

    PinyinResourceSet(const PinyinResourceSet&) = delete;
    PinyinResourceSet& operator=(const PinyinResourceSet&) = delete;
    PinyinResourceSet(PinyinResourceSet&&) = delete;
    PinyinResourceSet& operator=(PinyinResourceSet&&) = delete;

    const std::string& scheme_id() const { return scheme_id_; }
    PinyinSchemeKind kind() const { return kind_; }
    bool has_spellings() const { return spellings_.has_spellings(); }

    SegmentResult segment(const std::string& input, const QueryDeadline* deadline = nullptr,
                          const SyllabifierOptions& options = {}) const;
    bool has_fuzzy_path(const std::string& input, const SyllabifierOptions& options = {}) const;
    bool matches_without_fuzzy(const std::string& input, const std::string& syllables) const {
        return syllabifier_.matches_without_fuzzy(input, syllables);
    }
    std::vector<SpellingMatch> prefix_search(std::string_view input,
                                             bool enable_fuzzy = true,
                                             uint8_t fuzzy_groups = kAllFuzzyGroups) const;
    std::vector<SpellingMatch> completion_search(std::string_view input,
                                                 bool enable_fuzzy = true,
                                                 uint8_t fuzzy_groups = kAllFuzzyGroups) const;

private:
    PinyinResourceSet(std::string scheme_id, PinyinSchemeKind kind);

    const std::string scheme_id_;
    const PinyinSchemeKind kind_;
    SpellingsIndex spellings_;
    Syllabifier syllabifier_;
};

} // namespace cxxime

#endif // CXXIME_PINYIN_RESOURCE_H_
