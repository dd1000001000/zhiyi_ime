// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/english_lexicon.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include <cxxime/data_path.h>

namespace cxxime {

namespace {

std::string to_lower_ascii(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

std::shared_ptr<const EnglishLexicon> EnglishLexicon::shared() {
    static std::once_flag once;
    static std::shared_ptr<const EnglishLexicon> instance;
    std::call_once(once, [] {
        auto lexicon = std::make_shared<EnglishLexicon>();
        if (lexicon->load(data_path("english.words.tsv")) && lexicon->size() > 0) {
            instance = std::move(lexicon);
        }
    });
    return instance;
}

bool EnglishLexicon::load(const std::string& path) {
    std::ifstream f(widen(path), std::ios::binary);
    if (!f) return false;
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Entry e;
        e.key = to_lower_ascii(line.substr(0, t1));
        e.text = line.substr(t1 + 1, t2 - t1 - 1);
        e.score = std::atoi(line.c_str() + t2 + 1);
        if (!e.key.empty() && !e.text.empty()) entries.push_back(std::move(e));
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.key < b.key; });
    entries_ = std::move(entries);
    return true;
}

std::vector<EnglishWord> EnglishLexicon::lookup(const std::string& code, int limit, int min_score,
                                                bool include_rare_exact) const {
    std::vector<EnglishWord> exact, completions;
    if (code.empty() || limit <= 0) return {};
    const std::string key = to_lower_ascii(code);
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const Entry& e, const std::string& k) { return e.key < k; });
    for (; it != entries_.end() && it->key.compare(0, key.size(), key) == 0; ++it) {
        if (it->key.size() == key.size()) {
            if (include_rare_exact || it->score >= min_score) exact.push_back({it->text, it->score, true});
        } else if (it->score >= min_score) {
            completions.push_back({it->text, it->score, false});
        }
    }
    auto by_score = [](const EnglishWord& a, const EnglishWord& b) { return a.score > b.score; };
    std::stable_sort(exact.begin(), exact.end(), by_score);
    std::stable_sort(completions.begin(), completions.end(), by_score);
    std::vector<EnglishWord> out;
    std::unordered_set<std::string> seen;
    for (auto* list : {&exact, &completions}) {
        for (auto& w : *list) {
            if (static_cast<int>(out.size()) >= limit) return out;
            if (seen.insert(w.text).second) out.push_back(std::move(w));
        }
    }
    return out;
}

}  // namespace cxxime
