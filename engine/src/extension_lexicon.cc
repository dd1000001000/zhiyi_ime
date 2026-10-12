// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/extension_lexicon.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace cxxime {

namespace {

// The Top-N index's keys and scores (scripts/build_pinyin_topn.py, which keeps them in sync
// with rank_fallback_candidate in pinyin_translator.cc).
constexpr std::size_t kMaxMaterializedPrefixLength = 6;
constexpr std::size_t kMaxMixedKeyLength = 16;
constexpr std::size_t kMaxMixedKeysPerEntry = 8;
constexpr int kCompleteBase = 60000000;
constexpr int kCompletionBase = 45000000;
constexpr int kExtensionBase = 30000000;
constexpr double kLogScale = 500000.0;
constexpr double kLogOffset = 10.0;
constexpr long long kMaxLogScore = 14999999;
constexpr double kAbbreviationCredibility = -0.6931471805599453;  // ln 0.5
constexpr double kCompletionCredibility = -2.995732273553991;     // ln 0.05
constexpr int kRareCharacterFrequency = 100;
constexpr int kRareCharacterScale = 100;
// Keys of up to four letters start hundreds or thousands of words (the composer asks for every
// span of a sentence, hundreds a keystroke): their best words are scored when the
// lexicon is built (they rank mostly as extensions, below the dictionary's own words, and the
// fast path asks for a page or two). Longer keys are scored when typed, up to kMaxKeyScan words.
constexpr std::size_t kShortKeyLength = 4;
constexpr std::size_t kMaxShortKeyWords = 32;
constexpr std::size_t kMaxKeyScan = 256;

std::vector<std::string> split_syllables(const std::string& syllables) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = syllables.find(':', start);
        parts.push_back(syllables.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

bool is_retroflex(const std::string& s) {
    return s.size() >= 2 && s[1] == 'h' && (s[0] == 'z' || s[0] == 'c' || s[0] == 's');
}

bool single_character(const std::string& text) {
    if (text.empty() || text.size() > 4) return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) return false;
    }
    return true;
}

// build_pinyin_topn.py _generate_mixed.
std::vector<std::string> mixed_keys(const std::vector<std::string>& s) {
    const std::size_t n = s.size();
    if (n <= 1) return {};
    std::set<std::string> results;
    std::string enhanced;
    for (const std::string& x : s) enhanced += is_retroflex(x) ? x.substr(0, 2) : x.substr(0, 1);
    results.insert(enhanced);
    if (n >= 5) {
        std::string head;
        for (const std::string& x : s) head += x[0];
        results.insert(head);
    }
    std::string rest_first;
    for (std::size_t i = 1; i < n; ++i) rest_first += s[i][0];
    results.insert(s[0] + rest_first);
    std::string rest_from_3;
    for (std::size_t i = 2; i < n; ++i) rest_from_3 += s[i][0];
    if (n >= 3) results.insert(s[0] + s[1] + rest_from_3);
    results.insert(std::string(1, s[0][0]) + s[1] + rest_from_3);
    if (s[0].size() >= 2) results.insert(s[0].substr(0, 2) + rest_first);
    std::string exact;
    for (const std::string& x : s) exact += x;
    results.erase(exact);
    results.erase(std::string{});
    return {results.begin(), results.end()};
}

// build_pinyin_topn.py key_quality: reading `key` as the start of these syllables, each in
// full, by its initial (z / zh) or, the last one typed, cut short.
struct Quality {
    bool complete = false;
    bool unfinished = false;
    double credibility = 0.0;
};

bool key_quality(const std::string& key, const std::vector<std::string>& syllables, bool erhua,
                 Quality* out) {
    // The composer asks for every span of a sentence (hundreds of keys a keystroke): states are
    // kept in two arrays reused by the thread, index position * 2 + cut, instead of allocating.
    constexpr double kNone = -1e300;
    const std::size_t n = syllables.size();
    const std::size_t m = key.size();
    thread_local std::vector<double> frontier, next;
    frontier.assign(2 * (m + 1), kNone);
    next.assign(2 * (m + 1), kNone);
    frontier[0] = 0.0;
    bool found = false;
    // best compared as (complete, finished, credibility)
    bool best_complete = false, best_finished = false;
    double best_credibility = 0.0;
    const auto offer = [&](std::size_t p, bool cut, double credibility) {
        double& slot = next[p * 2 + (cut ? 1 : 0)];
        if (credibility > slot) slot = credibility;
    };
    for (std::size_t i = 0;; ++i) {
        bool any = false;
        for (std::size_t p = 0; p <= m; ++p) {
            for (int cut = 0; cut < 2; ++cut) {
                const double credibility = frontier[p * 2 + cut];
                if (credibility == kNone) continue;
                any = true;
                if (p == m) {
                    if (i > 0) {
                        const bool complete = i == n, finished = cut == 0;
                        if (!found || std::make_tuple(complete, finished, credibility) >
                                          std::make_tuple(best_complete, best_finished, best_credibility)) {
                            found = true;
                            best_complete = complete;
                            best_finished = finished;
                            best_credibility = credibility;
                        }
                    }
                    continue;
                }
                if (i == n) continue;
                const std::string& s = syllables[i];
                if (key.compare(p, s.size(), s) == 0) offer(p + s.size(), false, credibility);
                // Initials: zh / ch / sh, and the first letter.
                const bool retroflex = is_retroflex(s) && s.size() > 2;
                if (retroflex && key.compare(p, 2, s, 0, 2) == 0) {
                    offer(p + 2, false, credibility + kAbbreviationCredibility);
                }
                if (s.size() > 1 && key[p] == s[0]) {
                    offer(p + 1, false, credibility + kAbbreviationCredibility);
                }
                // The last typed syllable cut short (longer than an initial).
                const std::size_t rest = m - p;
                if (rest < s.size() && s.compare(0, rest, key, p, rest) == 0 && rest != 1 &&
                    !(retroflex && rest == 2)) {
                    offer(m, true, credibility + kCompletionCredibility);
                }
                if (erhua && i == n - 1 && i > 0 && s == "er" && key[p] == 'r') {
                    offer(p + 1, false, credibility);
                }
            }
        }
        if (!any) break;
        frontier.swap(next);
        std::fill(next.begin(), next.end(), kNone);
    }
    if (!found) return false;
    out->complete = best_complete;
    out->unfinished = !best_finished;
    out->credibility = best_credibility;
    return true;
}

long long python_round(double value) {
    // Python's round(): halves to even, as the index builder.
    const double floor_value = std::floor(value);
    const double diff = value - floor_value;
    long long r = static_cast<long long>(floor_value);
    if (diff > 0.5 || (diff == 0.5 && (r % 2 != 0))) ++r;
    return r;
}

}  // namespace

std::shared_ptr<const ExtensionLexicon> ExtensionLexicon::build_pinyin(
    std::vector<ExtensionWord> words, const std::vector<std::string>& syllabary,
    std::vector<std::string> withheld) {
    std::shared_ptr<ExtensionLexicon> lexicon(new ExtensionLexicon);
    lexicon->pinyin_ = true;
    std::unordered_map<std::string, std::uint32_t> ids;
    for (std::uint32_t i = 0; i < syllabary.size(); ++i) {
        ids.emplace(syllabary[i], i);
        lexicon->syllables_.insert(syllabary[i]);
    }
    struct Pending {
        Word word;
        std::vector<std::uint32_t> ids;
    };
    std::vector<Pending> pending;
    pending.reserve(words.size());
    for (ExtensionWord& w : words) {
        Pending p;
        bool known = !w.code.empty() && !w.text.empty();
        for (std::string& syllable : split_syllables(w.code)) {
            const auto it = ids.find(syllable);
            if (it == ids.end()) {
                known = false;
                break;
            }
            p.ids.push_back(it->second);
            p.word.compact += syllable;
            p.word.syllables.push_back(std::move(syllable));
        }
        if (!known) continue;
        p.word.text = std::move(w.text);
        p.word.code = std::move(w.code);
        p.word.frequency = w.frequency;
        p.word.id_count = static_cast<std::uint32_t>(p.ids.size());
        const std::string& t = p.word.text;
        p.word.erhua = p.ids.size() >= 2 && t.size() >= 3 && t.compare(t.size() - 3, 3, "儿") == 0 &&
                       p.word.code.size() >= 3 &&
                       p.word.code.compare(p.word.code.size() - 3, 3, ":er") == 0;
        pending.push_back(std::move(p));
    }
    // By syllable ids, the most frequent first (as dict.bin: the span lookup stops early).
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        if (a.ids != b.ids) return a.ids < b.ids;
        if (a.word.frequency != b.word.frequency) return a.word.frequency > b.word.frequency;
        return a.word.text < b.word.text;
    });
    lexicon->words_.reserve(pending.size());
    for (Pending& p : pending) {
        p.word.ids = static_cast<std::uint32_t>(lexicon->ids_.size());
        lexicon->ids_.insert(lexicon->ids_.end(), p.ids.begin(), p.ids.end());
        lexicon->words_.push_back(std::move(p.word));
    }
    const auto count = static_cast<std::uint32_t>(lexicon->words_.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        lexicon->by_code_.push_back(i);
        lexicon->by_compact_.push_back(i);
    }
    const auto& w = lexicon->words_;
    std::sort(lexicon->by_code_.begin(), lexicon->by_code_.end(),
              [&](std::uint32_t a, std::uint32_t b) { return w[a].code < w[b].code; });
    std::sort(lexicon->by_compact_.begin(), lexicon->by_compact_.end(),
              [&](std::uint32_t a, std::uint32_t b) { return w[a].compact < w[b].compact; });
    lexicon->build_key_index();
    lexicon->withheld_.insert(withheld.begin(), withheld.end());
    return lexicon;
}

std::shared_ptr<const ExtensionLexicon> ExtensionLexicon::build_wubi(std::vector<ExtensionWord> words,
                                                                     std::vector<std::string> withheld) {
    std::shared_ptr<ExtensionLexicon> lexicon(new ExtensionLexicon);
    lexicon->pinyin_ = false;
    for (ExtensionWord& w : words) {
        if (w.text.empty() || w.code.empty()) continue;
        Word word;
        word.text = std::move(w.text);
        word.code = std::move(w.code);
        word.frequency = w.frequency;
        lexicon->words_.push_back(std::move(word));
    }
    std::sort(lexicon->words_.begin(), lexicon->words_.end(), [](const Word& a, const Word& b) {
        if (a.code != b.code) return a.code < b.code;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        return a.text < b.text;
    });
    lexicon->withheld_.insert(withheld.begin(), withheld.end());
    return lexicon;
}

void ExtensionLexicon::build_key_index() {
    std::unordered_map<std::string, KeyIsSyllables> short_key_syllables;
    for (std::uint32_t index = 0; index < words_.size(); ++index) {
        const Word& word = words_[index];
        const std::vector<std::string>& s = word.syllables;
        std::vector<std::string> complete{word.compact};
        std::string abbr;
        for (const std::string& x : s) abbr += x[0];
        if (abbr != word.compact) complete.push_back(abbr);
        if (s.size() > 1) {
            std::vector<std::string> mixed = mixed_keys(s);
            if (mixed.size() > kMaxMixedKeysPerEntry) mixed.resize(kMaxMixedKeysPerEntry);
            for (const std::string& m : mixed) {
                if (m != word.compact && m.size() <= kMaxMixedKeyLength) complete.push_back(m);
            }
        }
        if (word.erhua) {
            const std::size_t n = complete.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::string& k = complete[i];
                if (k.size() > 2 && k.compare(k.size() - 2, 2, "er") == 0) {
                    complete.push_back(k.substr(0, k.size() - 2) + "r");
                }
            }
        }
        std::sort(complete.begin(), complete.end());
        complete.erase(std::unique(complete.begin(), complete.end()), complete.end());
        for (const std::string& k : complete) {
            keys_.push_back({static_cast<std::uint32_t>(key_chars_.size()),
                             static_cast<std::uint32_t>(k.size()), index});
            key_chars_ += k;
        }
        // The short keys this word answers: the first letters of its complete keys.
        std::vector<std::string> prefixes;
        for (const std::string& k : complete) {
            for (std::size_t length = 1; length <= k.size() && length <= kShortKeyLength; ++length) {
                prefixes.push_back(k.substr(0, length));
            }
        }
        std::sort(prefixes.begin(), prefixes.end());
        prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());
        for (const std::string& prefix : prefixes) {
            auto syllables = short_key_syllables.find(prefix);
            if (syllables == short_key_syllables.end()) {
                syllables = short_key_syllables.emplace(prefix, KeyIsSyllables{this}).first;
            }
            int score = 0;
            if (key_score(prefix, word, syllables->second, &score)) {
                short_keys_[prefix].push_back({index, score});
            }
        }
    }
    std::sort(keys_.begin(), keys_.end(), [&](const Key& a, const Key& b) {
        const int c = key_chars_.compare(a.offset, a.length, key_chars_, b.offset, b.length);
        if (c != 0) return c < 0;
        return words_[a.word].frequency > words_[b.word].frequency;
    });
    for (auto& [key, scored] : short_keys_) {
        std::sort(scored.begin(), scored.end(), [&](const Scored& a, const Scored& b) {
            if (a.score != b.score) return a.score > b.score;
            return words_[a.word].text < words_[b.word].text;
        });
        if (scored.size() > kMaxShortKeyWords) scored.resize(kMaxShortKeyWords);
        scored.shrink_to_fit();
    }
}

bool ExtensionLexicon::key_score(const std::string& key, const Word& word, KeyIsSyllables& key_is_syllables,
                                 int* score) const {
    Quality q;
    if (!key_quality(key, word.syllables, word.erhua, &q)) return false;
    if (single_character(word.text) && word.frequency <= kRareCharacterFrequency) {
        *score = static_cast<int>(python_round(kLogScale * (q.credibility + kLogOffset)) / kRareCharacterScale);
        return true;
    }
    const bool abbreviated = q.credibility < (q.unfinished ? kCompletionCredibility : 0.0) - 1e-9;
    int base = kCompleteBase;
    if (!q.complete) {
        base = kExtensionBase;
    } else if (q.unfinished || (abbreviated && key_is_syllables(key))) {
        base = kCompletionBase;
    }
    const long long log_score = python_round(
        kLogScale * (std::log((std::max)(word.frequency, 0) + 1.0) + q.credibility + kLogOffset));
    *score = base + static_cast<int>((std::min)((std::max)(log_score, 0LL), kMaxLogScore));
    return true;
}

Candidate ExtensionLexicon::key_candidate(const Word& word, int score) const {
    Candidate c;
    c.text = word.text;
    c.code = word.compact;
    c.syllables = word.code;
    c.frequency = score;
    c.source_frequency = word.frequency;
    c.origin = CandidateOrigin::kSystem;
    c.source = CandidateSource::kPinyin;
    return c;
}

bool ExtensionLexicon::reads_as_syllables(const std::string& key) const {
    std::vector<bool> ok(key.size() + 1, false);
    ok[0] = true;
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (!ok[i]) continue;
        for (std::size_t j = i + 1; j <= key.size() && j <= i + 6; ++j) {
            if (syllables_.count(key.substr(i, j - i))) ok[j] = true;
        }
        if (i > 0 && key[i] == 'r') ok[i + 1] = true;  // erhua
    }
    return ok[key.size()];
}

bool ExtensionLexicon::has_prefix(const std::uint32_t* query, std::size_t count) const {
    if (!pinyin_ || count == 0 || words_.empty()) return false;
    const auto less = [&](const Word& w) {
        const std::uint32_t* ids = ids_.data() + w.ids;
        const std::size_t common = (std::min<std::size_t>)(w.id_count, count);
        for (std::size_t i = 0; i < common; ++i) {
            if (ids[i] != query[i]) return ids[i] < query[i];
        }
        return w.id_count < count;
    };
    const auto it = std::partition_point(words_.begin(), words_.end(), less);
    if (it == words_.end() || it->id_count < count) return false;
    return std::equal(query, query + count, ids_.data() + it->ids);
}

std::vector<const ExtensionLexicon::Word*> ExtensionLexicon::match_ids(const std::uint32_t* query,
                                                                       std::size_t count,
                                                                       bool exact_only,
                                                                       std::size_t max_words) const {
    std::vector<const Word*> out;
    if (!pinyin_ || count == 0 || words_.empty() || max_words == 0) return out;
    const auto less = [&](const Word& w) {
        const std::uint32_t* ids = ids_.data() + w.ids;
        const std::size_t common = (std::min<std::size_t>)(w.id_count, count);
        for (std::size_t i = 0; i < common; ++i) {
            if (ids[i] != query[i]) return ids[i] < query[i];
        }
        return w.id_count < count;
    };
    for (auto it = std::partition_point(words_.begin(), words_.end(), less);
         it != words_.end() && out.size() < max_words; ++it) {
        if (it->id_count < count || !std::equal(query, query + count, ids_.data() + it->ids)) break;
        if (exact_only && it->id_count != count) break;  // exact matches come first
        out.push_back(&*it);
    }
    return out;
}

std::vector<const ExtensionLexicon::Word*> ExtensionLexicon::match_syllables(
    std::string_view syllables) const {
    std::vector<const Word*> out;
    if (!pinyin_) return out;
    auto it = std::partition_point(by_code_.begin(), by_code_.end(),
                                   [&](std::uint32_t i) { return words_[i].code < syllables; });
    for (; it != by_code_.end() && words_[*it].code == syllables; ++it) out.push_back(&words_[*it]);
    return out;
}

std::vector<Candidate> ExtensionLexicon::lookup_key(const std::string& key, int limit,
                                                   int min_score) const {
    std::vector<Candidate> out;
    if (!pinyin_ || key.empty() || limit <= 0 || keys_.empty()) return out;
    const auto offer = [&](Candidate c) {
        const auto same = std::find_if(out.begin(), out.end(), [&](const Candidate& o) { return o.text == c.text; });
        if (same != out.end()) {
            if (c.frequency > same->frequency) *same = std::move(c);
            return;
        }
        out.push_back(std::move(c));
    };
    if (key.size() <= kShortKeyLength) {
        const auto found = short_keys_.find(key);
        if (found != short_keys_.end()) {
            // Best first: the first `limit` words (two readings of a word count once).
            for (const Scored& s : found->second) {
                if (static_cast<int>(out.size()) >= limit || s.score < min_score) break;
                offer(key_candidate(words_[s.word], s.score));
            }
        }
    } else {
        const bool prefixes = key.size() <= kMaxMaterializedPrefixLength;
        auto it = std::partition_point(keys_.begin(), keys_.end(), [&](const Key& k) {
            return key_chars_.compare(k.offset, k.length, key) < 0;
        });
        KeyIsSyllables key_is_syllables{this};
        std::unordered_set<std::uint32_t> seen;
        for (; it != keys_.end() && seen.size() < kMaxKeyScan; ++it) {
            if (it->length < key.size() || key_chars_.compare(it->offset, key.size(), key) != 0) break;
            if (!prefixes && it->length != key.size()) continue;
            if (!seen.insert(it->word).second) continue;
            int score = 0;
            if (key_score(key, words_[it->word], key_is_syllables, &score) && score >= min_score) {
                offer(key_candidate(words_[it->word], score));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        return a.text < b.text;
    });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<std::size_t>(limit));
    return out;
}

std::vector<const ExtensionLexicon::Word*> ExtensionLexicon::match_code_prefix(std::string_view prefix,
                                                                               std::size_t limit) const {
    std::vector<const Word*> out;
    if (pinyin_ || limit == 0) return out;
    auto it = std::partition_point(words_.begin(), words_.end(),
                                   [&](const Word& w) { return w.code < prefix; });
    for (; it != words_.end() && it->code.compare(0, prefix.size(), prefix) == 0; ++it) {
        out.push_back(&*it);
    }
    std::stable_sort(out.begin(), out.end(), [&](const Word* a, const Word* b) {
        const bool a_exact = a->code.size() == prefix.size(), b_exact = b->code.size() == prefix.size();
        if (a_exact != b_exact) return a_exact;
        if (a->code.size() != b->code.size()) return a->code.size() < b->code.size();
        return a->frequency > b->frequency;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

bool ExtensionLexicon::contains(std::string_view text, std::string_view code) const {
    if (words_.empty() || code.empty()) return false;
    if (!pinyin_) {
        auto it = std::partition_point(words_.begin(), words_.end(),
                                       [&](const Word& w) { return w.code < code; });
        for (; it != words_.end() && it->code == code; ++it) {
            if (it->text == text) return true;
        }
        return false;
    }
    const bool syllables = code.find(':') != std::string_view::npos;
    const std::vector<std::uint32_t>& order = syllables ? by_code_ : by_compact_;
    const auto field = [&](std::uint32_t i) -> const std::string& {
        return syllables ? words_[i].code : words_[i].compact;
    };
    auto it = std::partition_point(order.begin(), order.end(),
                                   [&](std::uint32_t i) { return field(i) < code; });
    for (; it != order.end() && field(*it) == code; ++it) {
        if (words_[*it].text == text) return true;
    }
    // A one-syllable word: its syllables are its compact code.
    if (!syllables) {
        for (const Word* w : match_syllables(code)) {
            if (w->text == text) return true;
        }
    }
    return false;
}

}  // namespace cxxime
