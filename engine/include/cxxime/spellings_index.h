// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// SpellingsIndex — Prism layer: maps input strings to syllable interpretations.
// Loads from binary spellings.bin via memory-mapped file (Patricia trie format v2).

#ifndef CXXIME_SPELLINGS_INDEX_H_
#define CXXIME_SPELLINGS_INDEX_H_

#include <cstdint>
#include <cxxime/mapped_file.h>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cxxime {

enum SpellingType {
    kNormalSpelling = 0,
    kFuzzySpelling = 1,
    kAbbreviation = 2,
    kCompletionSpelling = 3,
};

// Fuzzy pinyin groups. A fuzzy spelling records the groups it needs in the byte after its type
// (SpellingEntry::padding); it is used only when all of them are enabled.
enum FuzzyGroup : uint8_t {
    kFuzzyZZh = 1 << 0,    // z = zh
    kFuzzyCCh = 1 << 1,    // c = ch
    kFuzzySSh = 1 << 2,    // s = sh
    kFuzzyNL = 1 << 3,     // n = l
    kFuzzyAnAng = 1 << 4,  // an = ang
    kFuzzyEnEng = 1 << 5,  // en = eng
    kFuzzyInIng = 1 << 6,  // in = ing
};
inline constexpr uint8_t kAllFuzzyGroups = 0x7F;

struct SpellingMatch {
    std::string syllable;
    int type = kNormalSpelling;
    float credibility = 0.0f;
    uint32_t input_key_len = 0;  // length of input key that produced this match
};

#pragma pack(push, 1)

struct SpellingsHeader {
    char magic[8];       // "CXSPL\x02\0\0"
    uint32_t version;    // 2
    uint32_t node_count;
    uint32_t string_data_size;
    uint32_t nodes_offset;
    uint32_t strings_offset;
};

// Each trie node (variable size in file):
//   uint32_t key_offset, key_len
//   uint8_t  num_spellings, num_children
//   uint16_t padding
//   SpellingEntry[num_spellings]
//   ChildEntry[num_children]

// v2 trie per-node spelling
struct SpellingEntry {
    uint32_t syllable_offset;
    uint32_t syllable_len;
    uint8_t type;
    uint8_t fuzzy_groups;  // FuzzyGroup bits this spelling needs (0: none; older files: 0)
    float credibility;
};

struct ChildEntry {
    uint8_t first_char;
    uint8_t padding[3];
    uint32_t node_index;
};

#pragma pack(pop)

class SpellingsIndex {
public:
    SpellingsIndex() = default;
    ~SpellingsIndex();
    SpellingsIndex(const SpellingsIndex&) = delete;
    SpellingsIndex& operator=(const SpellingsIndex&) = delete;

    bool load(const std::string& bin_path);
    void unload();
    bool has_spellings() const { return has_spelling_entries_; }

    // O(k) trie walk: returns all spellings where the stored key is a prefix of `prefix`.
    // `fuzzy_groups`: enabled FuzzyGroup bits (used when enable_fuzzy).
    std::vector<SpellingMatch> prefix_search(std::string_view prefix,
                                            bool enable_fuzzy = true,
                                            uint8_t fuzzy_groups = kAllFuzzyGroups) const;

    // Returns spellings whose stored input key is strictly longer than and starts with `prefix`.
    std::vector<SpellingMatch> completion_search(std::string_view prefix,
                                                 bool enable_fuzzy = true,
                                                 uint8_t fuzzy_groups = kAllFuzzyGroups) const;

    // For tests: create a v2 trie binary file from entries (input, syllable, type, credibility);
    // FuzzyGroup bits go in bits 8-15 of the type.
    static bool create_test_trie(const std::string& path,
                                 const std::vector<std::tuple<std::string, std::string, int, float>>& entries);

private:
    MappedFile file_;                  // the whole .spellings.bin, mapped read-only
    const char* data_ = nullptr;
    size_t data_size_ = 0;
    const char* nodes_ = nullptr;      // raw node data
    const char* strings_ = nullptr;
    uint32_t node_count_ = 0;
    bool has_spelling_entries_ = false;

    // Pre-built offset table for O(1) node access.
    std::unique_ptr<uint32_t[]> node_offsets_;
};

} // namespace cxxime

#endif // CXXIME_SPELLINGS_INDEX_H_
