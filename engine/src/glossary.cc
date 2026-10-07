// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/glossary.h>

#include <windows.h>

#include <algorithm>
#include <cstring>

#include <cxxime/candidate_presentation.h>
#include <cxxime/data_path.h>

namespace cxxime {
namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

constexpr char kMagic[8] = {'Z', 'Y', 'G', 'L', 'O', 'S', 'S', '\0'};
constexpr std::size_t kHeaderSize = 96;
constexpr std::uint8_t kPrimaryReading = 1;
constexpr std::uint8_t kEndOfSenses = 0xFF;

std::uint16_t read_u16(const char* data) {
    std::uint16_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint32_t read_u32(const char* data) {
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::string read_code(const char* data) {
    const char* end = static_cast<const char*>(std::memchr(data, '\0', 8));
    return std::string(data, end ? end : data + 8);
}

// False when the header is not one this program reads.
bool parse_header(const char* data, std::size_t size, GlossaryHeader* header) {
    if (size < kHeaderSize || std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return false;
    header->format_version = read_u16(data + 8);
    if (header->format_version != kGlossaryFormatVersion) return false;
    header->content_version = read_u32(data + 12);
    header->source = read_code(data + 16);
    header->target = read_code(data + 24);
    header->entry_count = read_u32(data + 32);
    header->built = read_u32(data + 36);
    return true;
}

std::string lowercase_ascii(std::string_view text) {
    std::string lower(text);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return lower;
}

} // namespace

const char* gloss_pos_label(GlossPos pos) {
    switch (pos) {
    case GlossPos::kNoun: return "n.";
    case GlossPos::kVerb: return "v.";
    case GlossPos::kAdjective: return "adj.";
    case GlossPos::kNumeral: return "num.";
    case GlossPos::kClassifier: return "mw.";
    case GlossPos::kPronoun: return "pron.";
    case GlossPos::kAdverb: return "adv.";
    case GlossPos::kPreposition: return "prep.";
    case GlossPos::kConjunction: return "conj.";
    case GlossPos::kParticle: return "part.";
    case GlossPos::kInterjection: return "int.";
    case GlossPos::kOnomatopoeia: return "onom.";
    case GlossPos::kDeterminer: return "det.";
    case GlossPos::kNone: break;
    }
    return "";
}

bool Glossary::open(const std::string& path) {
    close();
    if (!file_.open(path)) return false;
    const char* data = file_.data();
    const std::size_t size = file_.size();
    GlossaryHeader header;
    if (!parse_header(data, size, &header)) {
        close();
        return false;
    }
    const std::uint32_t index_offset = read_u32(data + 72);
    const std::uint32_t blob_offset = read_u32(data + 76);
    const std::uint32_t blob_size = read_u32(data + 80);
    const std::uint64_t index_end =
        static_cast<std::uint64_t>(index_offset) + std::uint64_t{header.entry_count} * 8;
    if (index_offset < kHeaderSize || index_end > size ||
        static_cast<std::uint64_t>(blob_offset) + blob_size > size) {
        close();
        return false;
    }
    header_ = std::move(header);
    index_ = data + index_offset;
    blob_ = data + blob_offset;
    blob_size_ = blob_size;
    return true;
}

void Glossary::close() {
    file_.close();
    header_ = {};
    index_ = nullptr;
    blob_ = nullptr;
    blob_size_ = 0;
}

std::string_view Glossary::key(std::uint32_t index) const {
    const std::uint32_t offset = read_u32(index_ + std::size_t{index} * 8);
    if (offset >= blob_size_) return {};
    const char* start = blob_ + offset;
    const char* end = static_cast<const char*>(std::memchr(start, '\0', blob_size_ - offset));
    return end ? std::string_view(start, end - start) : std::string_view{};
}

std::vector<GlossSense> Glossary::senses(std::uint32_t index, bool* primary) const {
    std::vector<GlossSense> result;
    std::uint32_t offset = read_u32(index_ + std::size_t{index} * 8 + 4);
    if (offset >= blob_size_) return result;
    *primary = (static_cast<std::uint8_t>(blob_[offset]) & kPrimaryReading) != 0;
    ++offset;
    while (offset < blob_size_ && result.size() < static_cast<std::size_t>(kMaxGlossSenses)) {
        const auto pos = static_cast<std::uint8_t>(blob_[offset++]);
        if (pos == kEndOfSenses || offset >= blob_size_) break;
        const char* start = blob_ + offset;
        const char* end = static_cast<const char*>(std::memchr(start, '\0', blob_size_ - offset));
        if (!end) break;
        result.push_back({pos <= static_cast<std::uint8_t>(GlossPos::kDeterminer)
                              ? static_cast<GlossPos>(pos)
                              : GlossPos::kNone,
                          std::string(start, end - start)});
        offset += static_cast<std::uint32_t>(end - start) + 1;
    }
    return result;
}

std::uint32_t Glossary::lower_bound(std::string_view wanted) const {
    std::uint32_t low = 0;
    std::uint32_t high = header_.entry_count;
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        if (key(middle) < wanted) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::vector<GlossSense> Glossary::lookup(std::string_view text, std::string_view syllables) const {
    if (!is_open() || text.empty() || header_.entry_count == 0) return {};
    bool primary = false;
    if (header_.source != "zh") {
        const std::string word = lowercase_ascii(text);
        const std::uint32_t at = lower_bound(word);
        if (at < header_.entry_count && key(at) == word) return senses(at, &primary);
        return {};
    }
    std::string prefix(text);
    prefix += '\t';
    if (!syllables.empty()) {
        const std::string exact = prefix + std::string(syllables);
        const std::uint32_t at = lower_bound(exact);
        if (at < header_.entry_count && key(at) == exact) return senses(at, &primary);
    }
    // Any reading: the most common one, else the first.
    std::vector<GlossSense> first;
    for (std::uint32_t at = lower_bound(prefix); at < header_.entry_count; ++at) {
        const std::string_view found = key(at);
        if (found.substr(0, prefix.size()) != prefix) break;
        std::vector<GlossSense> candidate = senses(at, &primary);
        if (primary) return candidate;
        if (first.empty()) first = std::move(candidate);
    }
    return first;
}

bool Glossary::read_header(const std::string& path, GlossaryHeader* header) {
    MappedFile file;  // UTF-8 paths (the user directory may have any name)
    return file.open(path) && parse_header(file.data(), file.size(), header);
}

std::vector<GlossSense> GlossaryStore::lookup(const std::string& source, const std::string& target,
                                              std::string_view text, std::string_view syllables) {
    if (target.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    Pack& pack = packs_[glossary_pack_id(source, target)];
    const std::uint64_t now = GetTickCount64();
    if (pack.checked_ms == 0 || now - pack.checked_ms >= 2000) {
        pack.checked_ms = now;
        const std::string path = glossary_pack_path(source, target);
        const std::wstring wide = to_wide(path);
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        std::uint64_t write_time = 0;
        std::uint64_t size = 0;
        if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
            write_time = (std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) |
                         data.ftLastWriteTime.dwLowDateTime;
            size = (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
        }
        if (write_time != pack.write_time || size != pack.size || !pack.glossary) {
            pack.write_time = write_time;
            pack.size = size;
            pack.glossary.reset();
            if (write_time != 0) {
                auto glossary = std::make_unique<Glossary>();
                if (glossary->open(path) && glossary->header().source == source &&
                    glossary->header().target == target) {
                    pack.glossary = std::move(glossary);
                }
            }
        }
    }
    return pack.glossary ? pack.glossary->lookup(text, syllables) : std::vector<GlossSense>{};
}

GlossaryStore& glossary_store() {
    static GlossaryStore store;
    return store;
}

std::string candidate_gloss(const std::string& chinese_target, const std::string& english_target,
                            const std::string& text, const std::string& syllables,
                            bool english_candidate) {
    // English words also come up in Chinese input (iPhone, WiFi): letters use the English pack.
    const bool letters = !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '\'';
    });
    const bool english = english_candidate || letters;
    const std::string& target = english ? english_target : chinese_target;
    if (target.empty()) return {};
    const std::vector<GlossSense> senses = glossary_store().lookup(
        english ? "en" : "zh", target, text, english ? std::string_view{} : syllables);
    std::string gloss;
    for (const GlossSense& sense : senses) {
        if (!gloss.empty()) gloss += kGlossSenseSeparator;
        gloss += gloss_pos_label(sense.pos);
        gloss += kGlossLabelSeparator;
        gloss += sense.text;
    }
    return gloss;
}

const std::vector<std::string>& glossary_targets(const std::string& source) {
    static const std::vector<std::string> chinese = {"en", "ja", "ko", "fr", "de", "es", "ru"};
    static const std::vector<std::string> english = {"zh", "ja", "ko", "fr", "de", "es", "ru"};
    static const std::vector<std::string> none;
    return source == "zh" ? chinese : source == "en" ? english : none;
}

std::string glossary_pack_id(const std::string& source, const std::string& target) {
    return source + "-" + target;
}

std::string glossary_directory() { return user_data_path("glossary"); }  // user_data_dir() ends in '\'

std::string glossary_pack_path(const std::string& source, const std::string& target) {
    return glossary_directory() + "\\" + glossary_pack_id(source, target) + ".gloss";
}

} // namespace cxxime
