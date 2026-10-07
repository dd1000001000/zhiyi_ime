// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Learning mode glossaries (docs/learning-mode.md): one language pack per direction and target
// language, %USERPROFILE%\zhiyi\glossary\<source>-<target>.gloss, mapped read-only.
//
// File layout (little-endian):
//   0   "ZYGLOSS\0"
//   8   u16 format_version (kGlossaryFormatVersion), u16 reserved
//   12  u32 content_version   the pack's translations; raised whenever they change
//   16  char source[8], 24 char target[8]   language codes, NUL-padded
//   32  u32 entry_count, 36 u32 built (yyyymmdd)
//   40  u8 dictionary_sha256[32]   the word list the pack was made from (for tracing only)
//   72  u32 index_offset, 76 u32 blob_offset, 80 u32 blob_size, 84..95 reserved
//   index  entry_count x {u32 key, u32 value}: offsets into the blob, sorted by key bytes
//   key    UTF-8, NUL-terminated: "text\tsyl:la:bles" (Chinese) or the lowercase word (English)
//   value  u8 flags (bit 0: the text's most common reading), then 1-3 senses
//          {u8 part of speech, UTF-8 translation, NUL}, ended by a 0xFF part of speech
#ifndef CXXIME_GLOSSARY_H_
#define CXXIME_GLOSSARY_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <cxxime/mapped_file.h>

namespace cxxime {

inline constexpr std::uint16_t kGlossaryFormatVersion = 1;
inline constexpr int kMaxGlossSenses = 3;

// Parts of speech; the label is the English abbreviation shown in the candidate window.
enum class GlossPos : std::uint8_t {
    kNone = 0,
    kNoun,
    kVerb,
    kAdjective,
    kNumeral,
    kClassifier,
    kPronoun,
    kAdverb,
    kPreposition,
    kConjunction,
    kParticle,
    kInterjection,
    kOnomatopoeia,
    kDeterminer,
};
const char* gloss_pos_label(GlossPos pos);  // "n.", "v.", ...; "" for kNone

struct GlossSense {
    GlossPos pos = GlossPos::kNone;
    std::string text;
};

struct GlossaryHeader {
    std::uint16_t format_version = 0;
    std::uint32_t content_version = 0;
    std::string source;  // "zh" or "en"
    std::string target;
    std::uint32_t entry_count = 0;
    std::uint32_t built = 0;
};

class Glossary {
public:
    bool open(const std::string& path);
    void close();
    bool is_open() const { return file_.is_open(); }
    const GlossaryHeader& header() const { return header_; }

    // Chinese: `syllables` ("xian:zai") picks the reading; without them, or for a reading the
    // pack does not have, the text's most common reading. English: the word in any case.
    std::vector<GlossSense> lookup(std::string_view text, std::string_view syllables = {}) const;

    // The header of a pack file without mapping it; false when it is not a pack this program
    // can read.
    static bool read_header(const std::string& path, GlossaryHeader* header);

private:
    std::string_view key(std::uint32_t index) const;
    std::vector<GlossSense> senses(std::uint32_t index, bool* primary) const;
    // First index whose key is not less than `key`.
    std::uint32_t lower_bound(std::string_view key) const;

    MappedFile file_;
    GlossaryHeader header_;
    const char* index_ = nullptr;
    const char* blob_ = nullptr;
    std::uint32_t blob_size_ = 0;
};

// The installed packs, opened on first use. A pack's file is checked again at most every two
// seconds, so a pack the settings program installs, updates or removes is picked up without a
// restart. Thread-safe.
class GlossaryStore {
public:
    std::vector<GlossSense> lookup(const std::string& source, const std::string& target,
                                   std::string_view text, std::string_view syllables);

private:
    struct Pack {
        std::unique_ptr<Glossary> glossary;
        std::uint64_t write_time = 0;  // of the file when opened; 0: no file
        std::uint64_t size = 0;
        std::uint64_t checked_ms = 0;
    };
    std::mutex mutex_;
    std::map<std::string, Pack> packs_;
};
GlossaryStore& glossary_store();  // the process's store

// Learning mode translation of a candidate as the candidate window takes it
// (candidate_presentation.h); empty without a pack, a target or an entry.
std::string candidate_gloss(const std::string& chinese_target, const std::string& english_target,
                            const std::string& text, const std::string& syllables,
                            bool english_candidate);

// The packs: <source>-<target>, source "zh" (targets en ja ko fr de es ru) or "en" (targets
// zh ja ko fr de es ru).
const std::vector<std::string>& glossary_targets(const std::string& source);
std::string glossary_pack_id(const std::string& source, const std::string& target);
std::string glossary_directory();  // %USERPROFILE%\zhiyi\glossary
std::string glossary_pack_path(const std::string& source, const std::string& target);

} // namespace cxxime

#endif // CXXIME_GLOSSARY_H_
