// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SYMBOL_TABLE_H_
#define CXXIME_SYMBOL_TABLE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/translation_result.h>

namespace cxxime {

inline constexpr char kSymbolPrefix = '\\';

class SymbolTable {
public:
    bool load(const std::string& path);
    bool empty() const { return categories_.empty(); }

    TranslationResult translate(const TranslationRequest& request) const;
    CandidatePage translate_page(const std::string& code, int page_index, int page_size,
                                 int candidate_offset = -1) const;
    // The emoji of a Chinese word (symbols.json "emoji_words", from the CLDR zh annotations),
    // best first; nullptr when it has none.
    const std::vector<std::string>* emoji_for(const std::string& word) const;

private:
    struct Category {
        std::string name;
        std::vector<std::string> candidates;
    };

    std::unordered_map<std::string, Category> categories_;
    std::vector<std::string> category_order_;
    std::unordered_map<std::string, std::vector<std::string>> emoji_words_;
};

} // namespace cxxime

#endif // CXXIME_SYMBOL_TABLE_H_
