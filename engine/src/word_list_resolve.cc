// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Resolving, storing and collecting user word lists (word_list.h).

#include <cxxime/word_list.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

#include <cxxime/candidate_store.h>
#include <cxxime/dict.h>
#include <cxxime/english_lexicon.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>

namespace cxxime {

namespace {

constexpr char kWordListHeader[] = "# zhiyi-wordlist format=1";
constexpr std::size_t kMaxReadingWordLength = 8;  // characters of a word used for a reading
constexpr std::size_t kMaxEnglishTextLength = 64;

// UTF-8 code points of `text` (byte offsets of each, plus the end).
std::vector<std::size_t> codepoint_offsets(std::string_view text) {
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) offsets.push_back(i);
    }
    offsets.push_back(text.size());
    return offsets;
}

std::size_t syllable_count(std::string_view syllables) {
    return syllables.empty() ? 0 : 1 + std::count(syllables.begin(), syllables.end(), ':');
}

bool usable_chinese_text(const std::string& text) {
    return is_valid_user_dict_text(text) && is_ordinary_candidate_text(text);
}

bool usable_english_text(const std::string& text) {
    if (text.empty() || text.size() > kMaxEnglishTextLength) return false;
    bool letter = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) return false;
        letter = letter || std::isalpha(u) != 0;
    }
    return letter;
}

// The English lexicon's key: the code without spaces, lowercase (build_english_dictionary.py).
std::string english_key(const std::string& text) {
    std::string key;
    for (char c : text) {
        if (c != ' ') key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

int default_chinese_frequency(std::size_t characters) {
    if (characters <= 1) return kWordListSingleCharacterFrequency;
    return characters == 2 ? kWordListTwoCharacterFrequency : kWordListLongWordFrequency;
}

// Rank of each word by the file's weight, 0 (heaviest) .. 1; -1 without a weight. Only when
// most words have one: a file that weights a few words says nothing about the others.
std::vector<double> weight_ranks(const std::vector<WordListWord>& words) {
    std::vector<double> ranks(words.size(), -1.0);
    std::vector<std::size_t> weighted;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i].weight >= 0) weighted.push_back(i);
    }
    if (weighted.size() < 2 || weighted.size() * 2 < words.size()) return ranks;
    std::stable_sort(weighted.begin(), weighted.end(),
                     [&](std::size_t a, std::size_t b) { return words[a].weight > words[b].weight; });
    // Equal weights share their rank (the middle of their run).
    for (std::size_t begin = 0; begin < weighted.size();) {
        std::size_t end = begin + 1;
        while (end < weighted.size() && words[weighted[end]].weight == words[weighted[begin]].weight) ++end;
        const double rank = (static_cast<double>(begin + end - 1) / 2.0) /
                            static_cast<double>(weighted.size() - 1);
        for (std::size_t k = begin; k < end; ++k) ranks[weighted[k]] = rank;
        begin = end;
    }
    return ranks;
}

std::string_view entry_text(const CandidateStoreView& store, std::uint32_t i) {
    const auto& e = store.entries[i];
    return {store.strings + e.text_offset, e.text_len};
}

std::string_view entry_code(const CandidateStoreView& store, std::uint32_t i) {
    const auto& e = store.entries[i];
    return {store.strings + e.syllable_ids_offset, e.syllable_ids_len};
}

// The Wubi 86 code of a word from its characters' full codes (the rules of Wubi 86 and of
// rime-wubi's encoder): two characters AaAbBaBb, three AaBaCaCb, four or more AaBaCaZa.
std::string wubi_word_code(const std::vector<std::string>& codes) {
    const std::size_t n = codes.size();
    auto take = [&](std::size_t character, std::size_t letters, std::string* out) {
        if (codes[character].size() < letters) return false;
        out->append(codes[character], 0, letters);
        return true;
    };
    std::string code;
    bool ok = false;
    if (n == 1) {
        code = codes[0];
        ok = !code.empty();
    } else if (n == 2) {
        ok = take(0, 2, &code) && take(1, 2, &code);
    } else if (n == 3) {
        ok = take(0, 1, &code) && take(1, 1, &code) && take(2, 2, &code);
    } else if (n >= 4) {
        ok = take(0, 1, &code) && take(1, 1, &code) && take(2, 1, &code) && take(n - 1, 1, &code);
    }
    return ok ? code : std::string{};
}

class Resolver {
public:
    Resolver(const Dict* pinyin, const Dict* wubi) : pinyin_(pinyin), wubi_(wubi) {}

    // Readings of the texts the words need (their pieces of up to kMaxReadingWordLength
    // characters), from the pinyin dictionary: one pass over it.
    void prepare_readings(const std::vector<const std::string*>& texts) {
        if (!pinyin_ || !pinyin_->is_open() || texts.empty()) return;
        for (const std::string* text : texts) {
            const std::vector<std::size_t> at = codepoint_offsets(*text);
            const std::size_t n = at.size() - 1;
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j <= n && j - i <= kMaxReadingWordLength; ++j) {
                    readings_.emplace(text->substr(at[i], at[j] - at[i]), Reading{});
                }
            }
        }
        const CandidateStoreView store = pinyin_->candidate_store();
        std::string text;
        for (std::uint32_t i = 0; i < store.entry_count; ++i) {
            const std::string_view t = entry_text(store, i);
            text.assign(t.data(), t.size());
            const auto it = readings_.find(text);
            if (it == readings_.end()) continue;
            const int frequency = store.entries[i].frequency;
            if (it->second.syllables.empty() || frequency > it->second.frequency) {
                const std::string_view code = entry_code(store, i);
                // Only a reading with one syllable per character.
                if (syllable_count(code) != codepoint_offsets(t).size() - 1) continue;
                it->second.syllables.assign(code.data(), code.size());
                it->second.frequency = frequency;
            }
        }
    }

    // A reading made of known words: the fewest pieces, then the most common ones.
    std::string guess_reading(const std::string& text) const {
        const std::vector<std::size_t> at = codepoint_offsets(text);
        const std::size_t n = at.size() - 1;
        struct Best {
            int pieces = INT32_MAX;
            double score = 0;
            std::size_t from = 0;
            std::string syllables;
        };
        std::vector<Best> best(n + 1);
        best[0].pieces = 0;
        for (std::size_t j = 1; j <= n; ++j) {
            for (std::size_t i = (j > kMaxReadingWordLength ? j - kMaxReadingWordLength : 0); i < j; ++i) {
                if (best[i].pieces == INT32_MAX) continue;
                const auto it = readings_.find(text.substr(at[i], at[j] - at[i]));
                if (it == readings_.end() || it->second.syllables.empty()) continue;
                const int pieces = best[i].pieces + 1;
                const double score = best[i].score + std::log(1.0 + (std::max)(0, it->second.frequency));
                if (pieces < best[j].pieces || (pieces == best[j].pieces && score > best[j].score)) {
                    best[j] = {pieces, score, i, it->second.syllables};
                }
            }
        }
        if (best[n].pieces == INT32_MAX) return {};
        std::vector<const std::string*> parts;
        for (std::size_t j = n; j > 0; j = best[j].from) parts.push_back(&best[j].syllables);
        std::string out;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (!out.empty()) out += ':';
            out += **it;
        }
        return out;
    }

    // Full codes of the characters, and the texts the Wubi dictionary has.
    void prepare_wubi() {
        if (wubi_prepared_ || !wubi_ || !wubi_->is_open()) return;
        wubi_prepared_ = true;
        const CandidateStoreView store = wubi_->candidate_store();
        for (std::uint32_t i = 0; i < store.entry_count; ++i) {
            const std::string_view text = entry_text(store, i);
            const std::string_view code = entry_code(store, i);
            if (code.empty() || !std::all_of(code.begin(), code.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
                continue;
            }
            wubi_texts_.emplace(text);
            if (codepoint_offsets(text).size() != 2) continue;
            auto& full = character_codes_[std::string(text)];
            const int frequency = store.entries[i].frequency;
            if (code.size() > full.code.size() || (code.size() == full.code.size() && frequency > full.frequency)) {
                full.code.assign(code.data(), code.size());
                full.frequency = frequency;
            }
        }
    }

    std::string wubi_code(const std::string& text) const {
        const std::vector<std::size_t> at = codepoint_offsets(text);
        std::vector<std::string> codes;
        for (std::size_t i = 0; i + 1 < at.size(); ++i) {
            const auto it = character_codes_.find(text.substr(at[i], at[i + 1] - at[i]));
            if (it == character_codes_.end()) return {};
            codes.push_back(it->second.code);
        }
        return wubi_word_code(codes);
    }

    bool wubi_has_text(const std::string& text) const { return wubi_texts_.count(text) != 0; }

    bool pinyin_valid(const std::string& syllables) const {
        if (!pinyin_ || !pinyin_->has_syllabary()) return !syllables.empty();
        std::size_t start = 0;
        while (start <= syllables.size()) {
            const std::size_t end = syllables.find(':', start);
            const std::string syllable =
                syllables.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (syllable.empty() || pinyin_->syllable_to_id(syllable) == UINT32_MAX) return false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return true;
    }

    bool pinyin_has(const std::string& text, const std::string& syllables) const {
        return pinyin_ && pinyin_->contains_system_entry(text, syllables);
    }

private:
    struct Reading {
        std::string syllables;
        int frequency = 0;
    };
    struct FullCode {
        std::string code;
        int frequency = 0;
    };
    const Dict* pinyin_;
    const Dict* wubi_;
    std::unordered_map<std::string, Reading> readings_;
    bool wubi_prepared_ = false;
    std::unordered_map<std::string, FullCode> character_codes_;
    std::unordered_set<std::string> wubi_texts_;
};

ResolvedWordList resolve_chinese(const WordListSource& source, const Dict* pinyin, const Dict* wubi) {
    ResolvedWordList list;
    list.language = WordListLanguage::kChinese;
    list.name = source.name;
    list.stats.read = source.words.size();
    Resolver resolver(pinyin, wubi);
    resolver.prepare_wubi();

    std::vector<const std::string*> unread;
    for (const WordListWord& word : source.words) {
        if (word.syllables.empty() && usable_chinese_text(word.text)) unread.push_back(&word.text);
    }
    resolver.prepare_readings(unread);

    const std::vector<double> ranks = weight_ranks(source.words);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < source.words.size(); ++i) {
        const WordListWord& word = source.words[i];
        if (!usable_chinese_text(word.text)) {
            ++list.stats.invalid;
            continue;
        }
        const std::size_t characters = codepoint_offsets(word.text).size() - 1;
        WordListEntry entry;
        entry.text = word.text;
        bool existing = false;

        std::string syllables = word.syllables;
        bool guessed = false;
        if (syllables.empty()) {
            syllables = resolver.guess_reading(word.text);
            guessed = !syllables.empty();
        }
        if (!syllables.empty() &&
            (syllable_count(syllables) != characters || !resolver.pinyin_valid(syllables))) {
            syllables.clear();
            guessed = false;
        }
        if (!syllables.empty()) {
            if (resolver.pinyin_has(word.text, syllables)) {
                existing = true;
            } else {
                entry.syllables = syllables;
                entry.guessed_reading = guessed;
            }
        }

        // Wubi: the file's code, else the word's code from its characters. A word the Wubi
        // dictionary has (under any code) is not added again.
        std::string code = word.wubi.empty() ? resolver.wubi_code(word.text) : word.wubi;
        if (!code.empty()) {
            if (resolver.wubi_has_text(word.text)) {
                existing = true;
            } else {
                entry.wubi = code;
            }
        }

        if (entry.syllables.empty() && entry.wubi.empty()) {
            ++(existing ? list.stats.existing : list.stats.invalid);
            continue;
        }
        if (!seen.insert(entry.text + '\x1f' + entry.syllables + '\x1f' + entry.wubi).second) {
            ++list.stats.invalid;
            continue;
        }
        const int base = default_chinese_frequency(characters);
        entry.frequency = base;
        if (ranks[i] >= 0 && characters > 1) {
            entry.frequency = static_cast<int>(std::lround(base * std::pow(2.0, 1.0 - 2.0 * ranks[i])));
        }
        list.stats.pinyin += entry.syllables.empty() ? 0 : 1;
        list.stats.wubi += entry.wubi.empty() ? 0 : 1;
        list.stats.guessed += entry.guessed_reading ? 1 : 0;
        list.entries.push_back(std::move(entry));
    }
    list.stats.added = list.entries.size();
    return list;
}

ResolvedWordList resolve_english(const WordListSource& source, const EnglishLexicon* english) {
    ResolvedWordList list;
    list.language = WordListLanguage::kEnglish;
    list.name = source.name;
    list.stats.read = source.words.size();
    const std::vector<double> ranks = weight_ranks(source.words);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < source.words.size(); ++i) {
        const std::string& text = source.words[i].text;
        if (!usable_english_text(text) || !seen.insert(text).second) {
            ++list.stats.invalid;
            continue;
        }
        if (english) {
            const std::string key = english_key(text);
            bool known = false;
            for (const EnglishWord& w : english->lookup(key, 8, 0, true)) {
                known = known || (w.exact && w.text == text);
            }
            if (known) {
                ++list.stats.existing;
                continue;
            }
        }
        WordListEntry entry;
        entry.text = text;
        entry.frequency = kWordListEnglishScore;
        if (ranks[i] >= 0) entry.frequency += static_cast<int>(std::lround(100.0 * (1.0 - 2.0 * ranks[i])));
        list.entries.push_back(std::move(entry));
    }
    list.stats.added = list.entries.size();
    return list;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string single_line(const std::string& s) {
    std::string out;
    for (char c : s) out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

}  // namespace

ResolvedWordList resolve_word_list(const WordListSource& source, const Dict* pinyin,
                                   const Dict* wubi, const EnglishLexicon* english) {
    return source.language == WordListLanguage::kEnglish ? resolve_english(source, english)
                                                         : resolve_chinese(source, pinyin, wubi);
}

std::string word_list_contents(const ResolvedWordList& list) {
    std::string out = kWordListHeader;
    out += "\n# language=";
    out += list.language == WordListLanguage::kEnglish ? "en" : "zh";
    out += "\n# name=" + single_line(list.name) + "\n";
    for (const WordListEntry& e : list.entries) {
        out += e.text;
        if (list.language == WordListLanguage::kChinese) {
            out += '\t' + e.syllables + '\t' + e.wubi;
        }
        out += '\t' + std::to_string(e.frequency);
        if (list.language == WordListLanguage::kChinese) out += e.guessed_reading ? "\tg" : "\t";
        out += '\n';
    }
    return out;
}

bool write_word_list(const std::string& path, const ResolvedWordList& list) {
    const std::string out = word_list_contents(list);
    // Written beside and moved over the old file: a list is never left half written.
    const std::wstring target = widen(path);
    const std::wstring temporary = target + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(out.data(), static_cast<std::streamsize>(out.size()))) return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool load_word_list(const std::string& path, ResolvedWordList* list) {
    *list = ResolvedWordList{};
    std::ifstream file(widen(path), std::ios::binary);
    if (!file) return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    return parse_word_list_contents(contents.str(), list);
}

bool parse_word_list_contents(const std::string& text, ResolvedWordList* list) {
    *list = ResolvedWordList{};
    std::istringstream file(text);
    std::string line;
    if (!std::getline(file, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kWordListHeader) return false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("# language=", 0) == 0) {
                list->language = line.substr(11) == "en" ? WordListLanguage::kEnglish
                                                         : WordListLanguage::kChinese;
            } else if (line.rfind("# name=", 0) == 0) {
                list->name = line.substr(7);
            }
            continue;
        }
        const std::vector<std::string> fields = split_tabs(line);
        WordListEntry e;
        e.text = fields[0];
        if (list->language == WordListLanguage::kEnglish) {
            if (fields.size() < 2 || !usable_english_text(e.text)) continue;
            e.frequency = std::atoi(fields[1].c_str());
        } else {
            if (fields.size() < 4 || !usable_chinese_text(e.text)) continue;
            e.syllables = fields[1];
            e.wubi = fields[2];
            e.frequency = std::atoi(fields[3].c_str());
            e.guessed_reading = fields.size() > 4 && fields[4] == "g";
            if (!e.syllables.empty() && !is_valid_user_dict_syllables(e.syllables)) e.syllables.clear();
            if (!e.wubi.empty() && !is_valid_user_dict_code(e.wubi)) e.wubi.clear();
            if (e.syllables.empty() && e.wubi.empty()) continue;
            list->stats.pinyin += e.syllables.empty() ? 0 : 1;
            list->stats.wubi += e.wubi.empty() ? 0 : 1;
            list->stats.guessed += e.guessed_reading ? 1 : 0;
        }
        list->entries.push_back(std::move(e));
    }
    list->stats.added = list->entries.size();
    return true;
}

void collect_extension_words(const ResolvedWordList& list, ExtensionWords* words) {
    for (const WordListEntry& e : list.entries) {
        if (list.language == WordListLanguage::kEnglish) {
            words->english.emplace_back(e.text, e.frequency);
            continue;
        }
        if (!e.syllables.empty()) words->pinyin.push_back({e.text, e.syllables, e.frequency});
        if (!e.wubi.empty()) words->wubi.push_back({e.text, e.wubi, e.frequency});
    }
}

void deduplicate_extension_words(ExtensionWords* words) {
    auto dedupe = [](std::vector<ExtensionWord>& list) {
        std::sort(list.begin(), list.end(), [](const ExtensionWord& a, const ExtensionWord& b) {
            if (a.text != b.text) return a.text < b.text;
            if (a.code != b.code) return a.code < b.code;
            return a.frequency > b.frequency;
        });
        list.erase(std::unique(list.begin(), list.end(),
                               [](const ExtensionWord& a, const ExtensionWord& b) {
                                   return a.text == b.text && a.code == b.code;
                               }),
                   list.end());
    };
    dedupe(words->pinyin);
    dedupe(words->wubi);
    std::sort(words->english.begin(), words->english.end(),
              [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second > b.second; });
    words->english.erase(std::unique(words->english.begin(), words->english.end(),
                                     [](const auto& a, const auto& b) { return a.first == b.first; }),
                         words->english.end());
}

}  // namespace cxxime
