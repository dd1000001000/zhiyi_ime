// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// User word lists (custom dictionaries the user imports, docs/user-dictionary.md):
//   read_word_list      a file in one of the supported formats -> the words as written there;
//   resolve_word_list   those words -> entries with pinyin and Wubi 86 codes, the words the
//                       system dictionary already has left out, a frequency for each;
//   write / load        the resolved list in Zhiyi's own TSV, kept in the user data folder;
//   collect             the enabled lists -> the words of the extension dictionaries
//                       (Dict::set_extension, EnglishLexicon::set_extension).
// Supported formats: Sogou cell dictionaries (.scel), Rime dictionaries (.dict.yaml: pinyin,
// Wubi or English), pinyin text exports ('a'li'ba'ba 阿里巴巴, a'li'ba'ba 阿里巴巴 1, 阿里巴巴
// a li ba ba) and English word lists (a word or phrase per line, optionally a weight).
#ifndef CXXIME_WORD_LIST_H_
#define CXXIME_WORD_LIST_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cxxime {

class Dict;
class EnglishLexicon;

enum class WordListLanguage { kChinese, kEnglish };

enum class WordListFormat { kUnknown, kSogouCell, kRime, kPinyinText, kEnglishText };

// A word as the file has it.
struct WordListWord {
    std::string text;
    std::string syllables;  // pinyin as "a:li:ba:ba"; empty: the file has none
    std::string wubi;       // Wubi 86 code; empty: the file has none
    double weight = -1.0;   // the file's own weight or frequency; below 0: none
};

struct WordListSource {
    WordListFormat format = WordListFormat::kUnknown;
    WordListLanguage language = WordListLanguage::kChinese;
    std::string name;  // the name the file gives (scel, Rime name:), else empty
    std::vector<WordListWord> words;
    std::size_t skipped_lines = 0;  // lines or records that are not a word of the list
};

enum class WordListReadError {
    kNone,
    kOpen,      // cannot read the file
    kTooLarge,  // over kMaxWordListBytes
    kFormat,    // not a supported format
    kNoWords,   // a supported format without a word
    kNoPinyin,  // Chinese words without pinyin (a plain list is not supported)
    kExtension, // a file name extension no supported format has
};

// The file name extensions of the supported formats (case-insensitive): .scel, .yaml / .yml
// (Rime's .dict.yaml), .txt, .csv, .tsv. Other files are refused before they are read.
bool word_list_extension_supported(const std::string& file_name);

inline constexpr std::size_t kMaxWordListBytes = 64u * 1024u * 1024u;

WordListReadError read_word_list(const std::wstring& path, WordListSource* source);
// The same from the file's bytes; `file_name` helps tell the format (extension).
WordListReadError parse_word_list(const std::string& bytes, const std::string& file_name,
                                  WordListSource* source);

// A resolved entry. Chinese: typed with pinyin when `syllables` is set, with Wubi when `wubi`
// is set (at least one of them). English: `text` and `frequency` (100 * Zipf, as
// english.words.tsv) only.
struct WordListEntry {
    std::string text;
    std::string syllables;
    std::string wubi;
    int frequency = 0;
    bool guessed_reading = false;  // pinyin worked out from the characters (one reading each)
};

struct WordListStats {
    std::size_t read = 0;       // words in the file
    std::size_t added = 0;      // entries kept
    std::size_t existing = 0;   // the system dictionary has them already (left out)
    std::size_t invalid = 0;    // unusable text or code, or a duplicate
    std::size_t pinyin = 0;     // entries typed with pinyin
    std::size_t wubi = 0;       // entries typed with Wubi
    std::size_t guessed = 0;    // entries whose pinyin was worked out
};

struct ResolvedWordList {
    WordListLanguage language = WordListLanguage::kChinese;
    std::string name;
    std::vector<WordListEntry> entries;
    WordListStats stats;
};

// Frequencies given to imported Chinese words: above the dictionary's long tail (weight 100,
// most words of three characters or more), below its common words. A file's own weights
// spread them from half to twice these by rank.
inline constexpr int kWordListTwoCharacterFrequency = 1000;
inline constexpr int kWordListLongWordFrequency = 500;
inline constexpr int kWordListSingleCharacterFrequency = 100;
// English: Zipf 4 (english.words.tsv scores are 100 * Zipf), 3 to 5 by the file's weights.
inline constexpr int kWordListEnglishScore = 400;

// `pinyin` / `wubi` / `english`: the system dictionaries (any may be null: then nothing is
// left out as existing, and no code is worked out from that dictionary).
ResolvedWordList resolve_word_list(const WordListSource& source, const Dict* pinyin,
                                   const Dict* wubi, const EnglishLexicon* english);

bool write_word_list(const std::string& path, const ResolvedWordList& list);
bool load_word_list(const std::string& path, ResolvedWordList* list);
// The same as text (backups carry the lists this way).
std::string word_list_contents(const ResolvedWordList& list);
bool parse_word_list_contents(const std::string& contents, ResolvedWordList* list);

// Words for the extension dictionaries.
struct ExtensionWord {
    std::string text;
    std::string code;  // pinyin: syllables "a:li"; Wubi: the code
    int frequency = 0;
};

struct ExtensionWords {
    std::vector<ExtensionWord> pinyin;
    std::vector<ExtensionWord> wubi;
    std::vector<std::pair<std::string, int>> english;  // text, score
};

// Adds the entries of a list (an enabled one). The same word and code from several lists is
// kept once, with the highest frequency.
void collect_extension_words(const ResolvedWordList& list, ExtensionWords* words);
void deduplicate_extension_words(ExtensionWords* words);

}  // namespace cxxime

#endif  // CXXIME_WORD_LIST_H_
