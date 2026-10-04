// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/english_lexicon.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include <cxxime/data_path.h>

namespace cxxime {

namespace {

// ---- Spelling correction: weighted optimal-string-alignment distance ----
constexpr float kEditCost = 1.0f;            // any other substitution, extra or missing letter
constexpr float kAdjacentKeyCost = 0.5f;     // neighbouring QWERTY key
constexpr float kDoubledLetterCost = 0.5f;   // adress / untill
constexpr float kTransposeCost = 0.75f;      // teh, recieve
constexpr float kFirstLetterPenalty = 0.5f;  // the first letter is rarely wrong
// Ranking only: a word matched by its beginning (thier -> there) is a weaker guess than a whole
// word within the same cost (thier -> their).
constexpr float kCompletionPenalty = 0.5f;
constexpr float kNoMatch = 1e9f;

bool key_position(char c, float* x, int* row) {
    static const char* const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const float offsets[] = {0.0f, 0.25f, 0.75f};
    if (c < 'a' || c > 'z') return false;
    for (int r = 0; r < 3; ++r) {
        if (const char* p = std::strchr(rows[r], c)) {
            *x = offsets[r] + static_cast<float>(p - rows[r]);
            *row = r;
            return true;
        }
    }
    return false;
}

bool adjacent_keys(char a, char b) {
    float xa = 0, xb = 0;
    int ra = 0, rb = 0;
    if (a == b || !key_position(a, &xa, &ra) || !key_position(b, &xb, &rb)) return false;
    return std::abs(ra - rb) <= 1 && std::fabs(xa - xb) <= 1.0f;
}

// typed[j - 1] was typed but is not in the word.
float extra_letter_cost(const std::string& typed, size_t j) {
    float cost = j >= 2 && typed[j - 1] == typed[j - 2] ? kDoubledLetterCost : kEditCost;
    return j == 1 ? cost + kFirstLetterPenalty : cost;
}

// word[i - 1] is missing from what was typed.
float missing_letter_cost(const std::string& word, size_t i) {
    float cost = i >= 2 && word[i - 1] == word[i - 2] ? kDoubledLetterCost : kEditCost;
    return i == 1 ? cost + kFirstLetterPenalty : cost;
}

// Row i (word prefix of length i) from rows i-1 and i-2 (nullptr for i < 2); n + 1 values.
void step_row(const std::string& typed, const std::string& word, size_t i, const float* prev2,
              const float* prev, float* out) {
    const size_t n = typed.size();
    const char w = word[i - 1];
    out[0] = prev[0] + missing_letter_cost(word, i);
    for (size_t j = 1; j <= n; ++j) {
        const char t = typed[j - 1];
        float sub = 0.0f;
        if (w != t) {
            sub = adjacent_keys(w, t) ? kAdjacentKeyCost : kEditCost;
            if (i == 1 && j == 1) sub += kFirstLetterPenalty;
        }
        float v = prev[j - 1] + sub;
        v = (std::min)(v, prev[j] + missing_letter_cost(word, i));
        v = (std::min)(v, out[j - 1] + extra_letter_cost(typed, j));
        if (prev2 && j >= 2 && w == typed[j - 2] && word[i - 2] == t && w != t)
            v = (std::min)(v, prev2[j - 2] + kTransposeCost);
        out[j] = v;
    }
}

void first_row(const std::string& typed, float* out) {
    out[0] = 0.0f;
    for (size_t j = 1; j <= typed.size(); ++j) out[j] = out[j - 1] + extra_letter_cost(typed, j);
}

bool plain_lowercase_letters(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < 'a' || c > 'z') return false;
    return true;
}

// A correction must be an ordinary word: its text is its code, apart from case and apostrophes
// (no names expanded from abbreviations such as "abs" -> "Anti-lock Braking System").
bool ordinary_word(const std::string& key, const std::string& text) {
    std::string folded;
    for (char c : text) {
        if (c == '\'') continue;
        folded += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return folded == key;
}

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

bool EnglishLexicon::contains(const std::string& code) const {
    const std::string key = to_lower_ascii(code);
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const Entry& e, const std::string& k) { return e.key < k; });
    return it != entries_.end() && it->key == key;
}

bool EnglishLexicon::has_prefix(const std::string& code) const {
    const std::string key = to_lower_ascii(code);
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const Entry& e, const std::string& k) { return e.key < k; });
    return it != entries_.end() && it->key.compare(0, key.size(), key) == 0;
}

float english_correction_limit(std::size_t length) {
    if (length < 3) return 0.0f;
    if (length <= 4) return 1.0f;
    if (length <= 7) return 1.5f;
    return 2.0f;
}

float english_typing_cost(const std::string& typed, const std::string& word, bool prefix) {
    const size_t n = typed.size();
    std::vector<float> rows((word.size() + 1) * (n + 1));
    first_row(typed, rows.data());
    float best = kNoMatch;
    for (size_t i = 1; i <= word.size(); ++i) {
        step_row(typed, word, i, i >= 2 ? &rows[(i - 2) * (n + 1)] : nullptr,
                 &rows[(i - 1) * (n + 1)], &rows[i * (n + 1)]);
        best = (std::min)(best, rows[i * (n + 1) + n]);
    }
    return prefix ? best : rows[word.size() * (n + 1) + n];
}

std::vector<EnglishWord> EnglishLexicon::corrections(const std::string& code, int limit,
                                                     int min_score, bool completions) const {
    const std::string typed = to_lower_ascii(code);
    const float max_cost = english_correction_limit(typed.size());
    if (limit <= 0 || max_cost <= 0.0f || !plain_lowercase_letters(typed)) return {};

    // The keys are sorted, so consecutive keys share prefixes: one DP row per prefix length is
    // kept and only the rows past the common prefix are recomputed. A row whose cheapest cell
    // (and the one before it) is over the limit ends the search below that prefix.
    const size_t width = typed.size() + 1;
    std::vector<float> rows(width);
    std::vector<float> row_min(1, 0.0f);
    std::vector<float> best_prefix(1, kNoMatch);  // cheapest D[d][n] for 1 <= d <= depth
    first_row(typed, rows.data());
    size_t valid_depth = 0;
    std::string previous;

    struct Hit {
        size_t index;
        float cost;
    };
    std::vector<Hit> hits;
    auto consider = [&](size_t index, float cost, bool completion) {
        const Entry& e = entries_[index];
        if (cost > 0.0f && cost <= max_cost && e.score >= min_score && plain_lowercase_letters(e.key) &&
            ordinary_word(e.key, e.text))
            hits.push_back({index, completion ? cost + kCompletionPenalty : cost});
    };

    size_t index = 0;
    while (index < entries_.size()) {
        const std::string& key = entries_[index].key;
        size_t common = 0;
        const size_t shared = (std::min)({previous.size(), key.size(), valid_depth});
        while (common < shared && previous[common] == key[common]) ++common;

        size_t depth = common;
        bool pruned = false;
        for (size_t i = common + 1; i <= key.size(); ++i) {
            if (rows.size() < (i + 1) * width) {
                rows.resize((i + 1) * width);
                row_min.resize(i + 1);
                best_prefix.resize(i + 1);
            }
            float* row = &rows[i * width];
            step_row(typed, key, i, i >= 2 ? &rows[(i - 2) * width] : nullptr,
                     &rows[(i - 1) * width], row);
            row_min[i] = *std::min_element(row, row + width);
            best_prefix[i] = (std::min)(i >= 2 ? best_prefix[i - 1] : kNoMatch, row[width - 1]);
            depth = i;
            if (row_min[i] > max_cost && row_min[i - 1] > max_cost) {
                pruned = true;
                break;
            }
        }
        previous = key;
        valid_depth = depth;

        if (!pruned) {
            const float whole = rows[key.size() * width + width - 1];
            if (whole <= max_cost) {
                consider(index, whole, false);
            } else if (completions && key.size() > 0) {
                consider(index, best_prefix[key.size()], true);
            }
            ++index;
            continue;
        }
        // Every key starting with key[0, depth) is over the limit as a whole word; as a
        // completion each costs best_prefix[depth].
        const std::string stem = key.substr(0, depth);
        for (; index < entries_.size() && entries_[index].key.compare(0, depth, stem) == 0; ++index) {
            if (completions) consider(index, best_prefix[depth], true);
        }
    }

    auto rank = [&](const Hit& h) {
        return entries_[h.index].score / 100.0f - kEnglishCostRankWeight * h.cost;
    };
    std::stable_sort(hits.begin(), hits.end(),
                     [&](const Hit& a, const Hit& b) { return rank(a) > rank(b); });
    std::vector<EnglishWord> out;
    std::unordered_set<std::string> seen;
    for (const Hit& h : hits) {
        if (static_cast<int>(out.size()) >= limit) break;
        const Entry& e = entries_[h.index];
        if (!seen.insert(e.text).second) continue;
        EnglishWord w{e.text, e.score, false};
        w.cost = h.cost;
        w.corrected = true;
        out.push_back(std::move(w));
    }
    return out;
}

}  // namespace cxxime
