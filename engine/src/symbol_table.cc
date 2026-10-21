// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/symbol_table.h>

#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <utility>

#include <json.hpp>

namespace cxxime {

namespace {

bool is_valid_code(const std::string& code) {
    if (code.size() != 2) {
        return false;
    }
    return std::all_of(code.begin(), code.end(),
                       [](char ch) { return ch >= 'a' && ch <= 'z'; });
}

} // namespace

bool SymbolTable::load(const std::string& path) {
    categories_.clear();
    category_order_.clear();
    emoji_words_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    try {
        const nlohmann::json root = nlohmann::json::parse(file);
        if (!root.is_object() ||
            root.value("version", 0) != 1 ||
            !root.contains("categories") ||
            !root["categories"].is_array()) {
            return false;
        }

        std::unordered_map<std::string, Category> loaded;
        std::vector<std::string> loaded_order;
        for (const auto& item : root["categories"]) {
            if (!item.is_object() || !item.contains("code") || !item["code"].is_string() ||
                !item.contains("name") || !item["name"].is_string() ||
                !item.contains("candidates") || !item["candidates"].is_array()) {
                return false;
            }

            const std::string code = item["code"].get<std::string>();
            if (!is_valid_code(code) || loaded.find(code) != loaded.end()) {
                return false;
            }

            Category category;
            category.name = item["name"].get<std::string>();
            std::unordered_set<std::string> seen;
            for (const auto& value : item["candidates"]) {
                if (!value.is_string()) {
                    return false;
                }
                std::string text = value.get<std::string>();
                if (text.empty() || !candidate_text_fits(text)) {
                    return false;
                }
                if (seen.insert(text).second) {
                    category.candidates.push_back(std::move(text));
                }
            }
            if (category.name.empty() || !candidate_text_fits(category.name) ||
                category.candidates.empty()) {
                return false;
            }
            loaded_order.push_back(code);
            loaded.emplace(code, std::move(category));
        }
        if (loaded.empty()) {
            return false;
        }
        std::unordered_map<std::string, std::vector<std::string>> emoji_words;
        if (root.contains("emoji_words")) {
            if (!root["emoji_words"].is_object()) {
                return false;
            }
            for (const auto& [word, values] : root["emoji_words"].items()) {
                if (word.empty() || !candidate_text_fits(word) || !values.is_array() ||
                    values.empty()) {
                    return false;
                }
                std::vector<std::string>& texts = emoji_words[word];
                for (const auto& value : values) {
                    if (!value.is_string() || value.get<std::string>().empty() ||
                        !candidate_text_fits(value.get<std::string>())) {
                        return false;
                    }
                    texts.push_back(value.get<std::string>());
                }
            }
        }
        categories_ = std::move(loaded);
        category_order_ = std::move(loaded_order);
        emoji_words_ = std::move(emoji_words);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

const std::vector<std::string>* SymbolTable::emoji_for(const std::string& word) const {
    const auto found = emoji_words_.find(word);
    return found == emoji_words_.end() ? nullptr : &found->second;
}

CandidatePage SymbolTable::translate_page(const std::string& code, int page_index, int page_size,
                                          int candidate_offset) const {
    CandidatePage page;
    if (page_size <= 0) {
        return page;
    }

    if (code.empty()) {
        const int category_page_size = (std::min)(page_size, 9);
        const int offset =
            candidate_offset >= 0 ? candidate_offset : page_index * category_page_size;
        page.page_index = page_index;
        page.page_offset = offset;
        page.page_size = category_page_size;
        const int known_count = static_cast<int>(category_order_.size());
        page.extent = make_candidate_extent(
            known_count, (std::min)(offset + category_page_size, known_count), false);
        if (offset < 0 || offset >= known_count) {
            return page;
        }

        const int end = (std::min)(offset + category_page_size, known_count);
        page.candidates.reserve(end - offset);
        for (int index = offset; index < end; ++index) {
            const std::string& category_code = category_order_[index];
            const auto category = categories_.find(category_code);
            if (category == categories_.end()) {
                continue;
            }
            Candidate candidate;
            candidate.text = category->second.name;
            candidate.comment = std::string(1, kSymbolPrefix) + category_code;
            candidate.source = CandidateSource::kSymbol;
            candidate.code = candidate.comment;
            page.candidates.push_back(std::move(candidate));
        }
        if (!page.candidates.empty()) {
            page.highlighted = 0;
        }
        return page;
    }

    const auto category = categories_.find(code);
    if (category == categories_.end()) {
        return page;
    }

    const int offset = candidate_offset >= 0 ? candidate_offset : page_index * page_size;
    page.page_index = page_index;
    page.page_offset = offset;
    page.page_size = page_size;
    const int known_count = static_cast<int>(category->second.candidates.size());
    page.extent = make_candidate_extent(
        known_count, (std::min)(offset + page_size, known_count), false);
    if (offset < 0 || offset >= known_count) {
        return page;
    }

    const int end = (std::min)(offset + page_size, known_count);
    page.candidates.reserve(end - offset);
    for (int index = offset; index < end; ++index) {
        Candidate candidate;
        candidate.text = category->second.candidates[index];
        candidate.source = CandidateSource::kSymbol;
        candidate.code = std::string(1, kSymbolPrefix) + code;
        page.candidates.push_back(std::move(candidate));
    }
    if (!page.candidates.empty()) {
        page.highlighted = 0;
    }
    return page;
}

TranslationResult SymbolTable::translate(const TranslationRequest& request) const {
    CandidatePage page =
        translate_page(request.input, request.page_index, request.page_size, request.page_offset);
    TranslationResult result;
    result.page_index = page.page_index;
    result.page_offset = page.page_offset;
    result.page_size = page.page_size;
    result.extent = page.extent;
    result.highlighted = page.highlighted;
    result.entries.reserve(page.candidates.size());
    for (auto& candidate : page.candidates) {
        CandidateEntry entry;
        entry.hint = candidate.comment;
        if (request.input.empty()) {
            ReplaceActiveInputAction action;
            action.scheme = CompositionScheme::kSymbol;
            action.input = candidate.code;
            action.cursor = action.input.size();
            entry.selection = std::move(action);
        } else {
            entry = make_text_candidate_entry(std::move(candidate), request.input.size() + 1);
            result.entries.push_back(std::move(entry));
            continue;
        }
        entry.candidate = std::move(candidate);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

} // namespace cxxime
