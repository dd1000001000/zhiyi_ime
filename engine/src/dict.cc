// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/dict.h>

#include <algorithm>
#include <cstring>

#include <windows.h>

#include <cxxime/candidate_preference.h>
#include <cxxime/disabled_system_lexicon.h>
#include <cxxime/logging.h>
#include <cxxime/manual_candidate_order.h>
#include <cxxime/query_budget.h>
#include <cxxime/query_trace.h>
#include <cxxime/topk_collector.h>
#include <cxxime/user_lexicon.h>

#include "binary_format.h"
#include "wubi_prefix_index.h"

static const char DICT_MAGIC_V2[] = "CXDIC\x02\x00\x00";
static constexpr int kExactSpanCollectionBoost = 100000000;

namespace cxxime {

// Linear dedup — cheaper than hash set for bounded result vectors (≤128)
static bool contains_text(const std::vector<Candidate>& items, const std::string& text) {
    for (auto& c : items)
        if (c.text == text) return true;
    return false;
}

static void merge_candidate_by_score(std::vector<Candidate>& items, Candidate candidate) {
    for (auto& item : items) {
        if (item.text == candidate.text) {
            if (candidate.frequency > item.frequency)
                item = std::move(candidate);
            return;
        }
    }
    items.push_back(std::move(candidate));
}

static void sort_candidates_by_score(std::vector<Candidate>& items) {
    std::sort(items.begin(), items.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.frequency != b.frequency) return a.frequency > b.frequency;
            if (a.text.size() != b.text.size()) return a.text.size() < b.text.size();
            return a.text < b.text;
        });
}

static std::string compact_syllable_code(const char* syllable_ids, uint32_t len) {
    std::string code;
    code.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        if (syllable_ids[i] != ':')
            code.push_back(syllable_ids[i]);
    }
    return code;
}

static void set_candidate_code(Candidate& candidate, const char* syllable_ids, uint32_t len) {
    candidate.syllables.assign(syllable_ids, len);
    candidate.code = compact_syllable_code(syllable_ids, len);
}

Dict::Dict(UserDictKind kind)
    : kind_(kind)
    , user_lexicon_(std::make_unique<UserLexicon>(kind))
    , candidate_preference_(std::make_unique<CandidatePreference>(kind))
    , manual_candidate_order_(std::make_unique<ManualCandidateOrder>(kind))
    , disabled_system_lexicon_(std::make_unique<DisabledSystemLexicon>(kind)) {}

Dict::~Dict() { unload_dict(); }

void Dict::fill_system_candidate(uint32_t entry_index, Candidate& candidate,
                                 int frequency_boost) const {
    const auto& entry = dict_entries_[entry_index];
    candidate.text.assign(dict_strings_ + entry.text_offset, entry.text_len);
    set_candidate_code(candidate, dict_strings_ + entry.syllable_ids_offset,
                       entry.syllable_ids_len);
    candidate.source_frequency = entry.frequency;
    candidate.frequency = entry.frequency + frequency_boost;
}

bool Dict::open(const std::string& dict_path, const std::string& user_dict_path) {
    if (!open_dict(dict_path))
        return false;
    return load_user_dict(user_dict_path);
}

bool Dict::open_bundle(const std::string& dict_path,
                       const std::string& user_dict_path,
                       const std::string& idx_path,
                       const std::string& topn_path) {
    if (kind_ != UserDictKind::PINYIN) {
        return false;
    }
    if (!open_dict_with_aux(dict_path, idx_path, topn_path, false))
        return false;
    return load_user_dict(user_dict_path);
}

bool Dict::open_wubi_dict(const std::string& dict_path, const std::string& prefix_index_path) {
    if (kind_ != UserDictKind::WUBI) {
        return false;
    }
    if (!open_dict_with_aux(dict_path, prefix_index_path, {}, false, true)) {
        return false;
    }
    return true;
}

bool Dict::open_wubi_bundle(const std::string& dict_path, const std::string& user_dict_path,
                            const std::string& prefix_index_path) {
    if (!open_wubi_dict(dict_path, prefix_index_path)) {
        return false;
    }
    return load_user_dict(user_dict_path);
}

bool Dict::has_wubi_prefix_index() const {
    return wubi_prefix_index_ != nullptr && wubi_prefix_index_->is_loaded();
}

bool Dict::is_open() const {
    return dict_data_ != nullptr;
}

bool Dict::open_dict(const std::string& bin_path) {
    if (!open_dict_with_aux(bin_path, {}, {}, true)) {
        return false;
    }
    return true;
}

bool Dict::open_dict_with_aux(const std::string& bin_path,
                              const std::string& idx_path,
                              const std::string& topn_path,
                              bool derive_aux_paths,
                              bool use_wubi_prefix_index) {
    unload_dict();
    CXXIME_LOG(L"Dict::open_dict path=%S", bin_path.c_str());

    HANDLE hFile = CreateFileA(bin_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CXXIME_LOG(L"Dict::open_dict CreateFileA FAILED");
        return false;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart < (LONGLONG)sizeof(DictHeader)) {
        CloseHandle(hFile);
        CXXIME_LOG(L"Dict::open_dict file too small");
        return false;
    }
    dict_data_size_ = (size_t)li.QuadPart;

    // Load entire file into heap memory (no mmap — avoids page-out latency)
    dict_data_ = new (std::nothrow) char[dict_data_size_];
    if (!dict_data_) {
        CloseHandle(hFile);
        CXXIME_LOG(L"Dict::open_dict allocation failed (%zu bytes)", dict_data_size_);
        return false;
    }

    DWORD bytes_read = 0;
    BOOL ok = ReadFile(hFile, dict_data_, (DWORD)dict_data_size_, &bytes_read, nullptr);
    CloseHandle(hFile);
    if (!ok || bytes_read != dict_data_size_) {
        CXXIME_LOG(L"Dict::open_dict ReadFile FAILED");
        unload_dict();
        return false;
    }

    auto* hdr = (const DictHeader*)dict_data_;
    if (std::memcmp(hdr->magic, DICT_MAGIC_V2, 8) != 0) {
        CXXIME_LOG(L"Dict::open_dict bad magic");
        unload_dict();
        return false;
    }

    // Bounds validation: ensure header fields don't point outside the file
    uint32_t version = hdr->version;
    if (version != 2) {
        CXXIME_LOG(L"Dict::open_dict bad version=%u", version);
        unload_dict();
        return false;
    }
    if (hdr->entries_offset > dict_data_size_ ||
        hdr->strings_offset > dict_data_size_ ||
        hdr->entry_count > (dict_data_size_ / sizeof(DictEntry)) ||
        hdr->entries_offset + (uint64_t)hdr->entry_count * sizeof(DictEntry) > dict_data_size_ ||
        hdr->strings_offset + (uint64_t)hdr->string_data_size > dict_data_size_) {
        CXXIME_LOG(L"Dict::open_dict bounds check FAILED");
        unload_dict();
        return false;
    }

    dict_entry_count_ = hdr->entry_count;
    dict_string_size_ = hdr->string_data_size;
    dict_entries_ = (const DictEntry*)(dict_data_ + hdr->entries_offset);
    dict_strings_ = dict_data_ + hdr->strings_offset;

    CandidateStoreView store = candidate_store();
    for (uint32_t i = 0; i < store.entry_count; ++i) {
        if (!candidate_store_entry_valid(store, i)) {
            CXXIME_LOG(L"Dict::open_dict invalid candidate entry=%u", i);
            unload_dict();
            return false;
        }
    }
    dict_fingerprint_ = candidate_store_fingerprint(store);

    CXXIME_LOG(L"Dict::open_dict OK entries=%u", dict_entry_count_);

    if (use_wubi_prefix_index) {
        wubi_prefix_index_ = std::make_unique<WubiPrefixIndex>();
        if (idx_path.empty() || !wubi_prefix_index_->load(idx_path, dict_entry_count_)) {
            CXXIME_LOG(L"Dict::open_dict Wubi prefix index not loaded");
            unload_dict();
            return false;
        }
    } else {
        // Try to load pre-built ID index (.dict.idx); build from scratch if absent
        bool index_loaded = false;
        if (derive_aux_paths) {
            index_loaded = load_id_index(bin_path);
        } else if (!idx_path.empty()) {
            index_loaded = load_id_index_file(idx_path);
            if (!index_loaded) {
                CXXIME_LOG(L"Dict::open_dict manifest idx not loaded");
                unload_dict();
                return false;
            }
        }
        if (!index_loaded) {
            build_syllabary();
            build_id_index();
        }

        // Try to load the pre-built Top-N index (pinyin.topn.bin).
        if (derive_aux_paths) {
            // Derive path from dict_bin_path: pinyin.dict.bin -> pinyin.topn.bin
            std::string derived_topn_path = bin_path;
            auto pos = derived_topn_path.rfind(".dict.bin");
            if (pos != std::string::npos)
                derived_topn_path.replace(pos, std::string::npos, ".topn.bin");
            else
                derived_topn_path += ".topn.bin";
            if (!short_cache_.load(derived_topn_path, candidate_store())) {
                CXXIME_LOG(L"Dict::open_dict Top-N index not loaded (standalone mode)");
                // Not fatal for standalone tools/tests. Server runtime uses open_bundle()
                // with manifest-declared topn_path and treats load failure as fatal.
            }
        } else if (!topn_path.empty()) {
            if (!short_cache_.load(topn_path, candidate_store())) {
                CXXIME_LOG(L"Dict::open_dict manifest topn not loaded");
                unload_dict();
                return false;
            }
        }
    }

    return true;
}

void Dict::unload_dict() {
    unload_id_index();
    short_cache_.unload();
    wubi_prefix_index_.reset();
    delete[] dict_data_;
    dict_data_ = nullptr;
    dict_entries_ = nullptr;
    dict_strings_ = nullptr;
    dict_entry_count_ = 0;
    dict_string_size_ = 0;
    dict_fingerprint_ = 0;
    dict_data_size_ = 0;
}

CandidateStoreView Dict::candidate_store() const {
    return {dict_entries_, dict_strings_, dict_entry_count_, dict_string_size_,
            dict_fingerprint_};
}

void Dict::close() {
    save_user_dict();
    save_candidate_preferences();
    save_disabled_system_entries();
    unload_dict();
}

std::vector<Candidate> Dict::lookup_by_syllables(
    const std::vector<std::string>& syllables, int limit, QueryTrace* trace) {
    std::vector<Candidate> results;
    if (!dict_entries_ || syllables.empty())
        return results;

    // Build syllable_ids key: ["ni","hao"] → "ni:hao"
    std::string key;
    for (size_t i = 0; i < syllables.size(); ++i) {
        if (i > 0) key += ":";
        key += syllables[i];
    }
    std::string concat_code;
    for (const auto& s : syllables)
        concat_code += s;
    const uint32_t key_len = (uint32_t)key.size();
    const char* key_data = key.data();
    const bool filter_disabled = disabled_system_entry_count() != 0;

    // Binary search for first entry with matching syllable_ids
    uint32_t lo = 0, hi = dict_entry_count_;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const auto& e = dict_entries_[mid];
        const char* sid = dict_strings_ + e.syllable_ids_offset;
        int cmp = std::memcmp(sid, key_data, std::min(e.syllable_ids_len, key_len));
        if (cmp < 0 || (cmp == 0 && e.syllable_ids_len < key_len)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // Collect all entries with matching syllable_ids (SQL already sorted by freq desc)
    while (lo < dict_entry_count_) {
        const auto& e = dict_entries_[lo];
        if (e.syllable_ids_len != key_len)
            break;
        if (std::memcmp(dict_strings_ + e.syllable_ids_offset, key_data, key_len) != 0)
            break;

        Candidate c;
        c.text.assign(dict_strings_ + e.text_offset, e.text_len);
        c.code = concat_code;
        c.syllables = key;
        c.frequency = e.frequency;
        if (filter_disabled && is_system_entry_disabled(c.text)) {
            ++lo;
            continue;
        }
        if (!contains_text(results, c.text)) {
            merge_candidate_by_score(results, std::move(c));
            if ((int)results.size() >= limit)
                break;
        }
        ++lo;
    }

    // Query the user dictionary through the exact index.
    QueryBudget ub;
    UserLookupStats ustats;
    auto user_results = lookup_user_exact(concat_code, limit, ub, trace, &ustats);
    for (auto& c : user_results) {
        merge_candidate_by_score(results, std::move(c));
    }

    // Sort by frequency descending
    sort_candidates_by_score(results);

    if ((int)results.size() > limit)
        results.resize(limit);

    return results;
}

// Budget-aware overload: delegates to non-budget version, adds deadline check
std::vector<Candidate> Dict::lookup_by_syllables(const std::vector<std::string>& syllables,
                                                   int limit, const QueryBudget& budget,
                                                   QueryTrace* trace) {
    if (budget.deadline.enabled && budget.deadline.expired()) {
        if (trace) {
            trace->deadline_exceeded = true;
            trace->truncated = true;
        }
        return {};
    }
    return lookup_by_syllables(syllables, limit, trace);
}

int Dict::count(const std::string& code_prefix, QueryTrace* trace) {
    if (!dict_entries_)
        return 0;

    const uint32_t prefix_len = (uint32_t)code_prefix.size();
    const char* prefix_data = code_prefix.data();
    int result = 0;

    // Count matching entries in binary dict
    uint32_t lo = 0, hi = dict_entry_count_;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const auto& e = dict_entries_[mid];
        const char* sid = dict_strings_ + e.syllable_ids_offset;
        uint32_t cmp_len = std::min(e.syllable_ids_len, prefix_len);
        int cmp = std::memcmp(sid, prefix_data, cmp_len);
        if (cmp < 0 || (cmp == 0 && e.syllable_ids_len < prefix_len)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    while (lo < dict_entry_count_) {
        const auto& e = dict_entries_[lo];
        if (e.syllable_ids_len < prefix_len)
            break;
        if (std::memcmp(dict_strings_ + e.syllable_ids_offset, prefix_data, prefix_len) != 0)
            break;
        ++result;
        ++lo;
    }

result += user_lexicon_->count_prefix(code_prefix, trace);

    return result;
}

bool Dict::create_test_dict(const std::string& path,
                            const std::vector<std::tuple<std::string, std::string, int>>& entries) {
    // Build string data and entry list
    std::string strings;
    std::vector<std::pair<uint32_t, uint32_t>> offsets; // (syllable_ids_off, text_off)
    std::vector<std::pair<uint32_t, uint32_t>> lens;    // (syllable_ids_len, text_len)
    std::vector<int> freqs;

    auto intern = [&strings](const std::string& s) -> std::pair<uint32_t, uint32_t> {
        uint32_t off = (uint32_t)strings.size();
        strings += s;
        return {off, (uint32_t)s.size()};
    };

    // Sort entries by syllable_ids for binary format
    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end());

    for (auto& [sid, text, freq] : sorted) {
        auto [sio, sil] = intern(sid);
        auto [to, tl] = intern(text);
        offsets.push_back({sio, to});
        lens.push_back({sil, tl});
        freqs.push_back(freq);
    }

    uint32_t count = (uint32_t)sorted.size();
    uint32_t entries_offset = sizeof(DictHeader);
    uint32_t strings_offset = entries_offset + count * sizeof(DictEntry);

    // Write file
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD written;
    DictHeader hdr = {};
    std::memcpy(hdr.magic, DICT_MAGIC_V2, 8);
    hdr.version = 2;
    hdr.entry_count = count;
    hdr.string_data_size = (uint32_t)strings.size();
    hdr.entries_offset = entries_offset;
    hdr.strings_offset = strings_offset;
    WriteFile(hFile, &hdr, sizeof(hdr), &written, nullptr);

    for (uint32_t i = 0; i < count; ++i) {
        DictEntry de = {};
        de.syllable_ids_offset = offsets[i].first;
        de.text_offset = offsets[i].second;
        de.syllable_ids_len = lens[i].first;
        de.text_len = lens[i].second;
        de.frequency = freqs[i];
        WriteFile(hFile, &de, sizeof(de), &written, nullptr);
    }

    WriteFile(hFile, strings.data(), (DWORD)strings.size(), &written, nullptr);
    CloseHandle(hFile);
    return true;
}

// ─── Syllable ID index (zero-copy mmap, v3 format) ────────────────────

void Dict::unload_id_index() {
    delete[] idx_data_;
    idx_data_ = nullptr;
    idx_data_size_ = 0;
    syllabary_.clear();
    syllable_to_id_.clear();
    id_index_.clear();
}

bool Dict::load_id_index(const std::string& dict_bin_path) {
    std::string idx_path = dict_bin_path;
    auto pos = idx_path.rfind(".dict.bin");
    if (pos == std::string::npos) {
        pos = idx_path.rfind(".dict.db");
        if (pos == std::string::npos)
            return false;
    }
    idx_path.replace(pos, std::string::npos, ".dict.idx");
    return load_id_index_file(idx_path);
}

bool Dict::load_id_index_file(const std::string& idx_path) {
    HANDLE hFile = CreateFileA(idx_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) || li.QuadPart < 28) {
        CloseHandle(hFile);
        return false;
    }
    size_t file_size = (size_t)li.QuadPart;

    // Load entire file into heap memory
    idx_data_ = new (std::nothrow) char[file_size];
    if (!idx_data_) {
        CloseHandle(hFile);
        return false;
    }

    DWORD bytes_read = 0;
    BOOL ok = ReadFile(hFile, idx_data_, (DWORD)file_size, &bytes_read, nullptr);
    CloseHandle(hFile);
    if (!ok || bytes_read != file_size) {
        delete[] idx_data_;
        idx_data_ = nullptr;
        return false;
    }
    idx_data_size_ = file_size;

    const char* base = idx_data_;

    // Header: magic(8) version(4) syl_count(4) syl_str_size(4) idx_count(4) idx_data_size(4) = 28
    if (std::memcmp(base, "CXIDX\0\0\0\0", 8) != 0) {
        unload_id_index();
        return false;
    }
    const uint32_t* h = (const uint32_t*)(base + 8);
    uint32_t ver = h[0], syl_count = h[1], syl_str_size = h[2];
    uint32_t idx_count = h[3], idx_data_size = h[4];
    if (ver != 3) {
        unload_id_index();
        return false;
    }

    // Syllabary
    const uint32_t* syl_offs = (const uint32_t*)(base + 28);
    if (28ULL + (uint64_t)syl_count * sizeof(uint32_t) > file_size) {
        unload_id_index();
        return false;
    }
    const char* syl_strs = (const char*)(syl_offs + syl_count);
    const char* file_end = base + file_size;
    if (syl_strs > file_end || syl_strs + syl_str_size > file_end) {
        unload_id_index();
        return false;
    }
    syllabary_.resize(syl_count);
    syllable_to_id_.clear();
    for (uint32_t i = 0; i < syl_count; ++i) {
        if (syl_offs[i] >= syl_str_size) {
            unload_id_index();
            return false;
        }
        const char* s = syl_strs + syl_offs[i];
        syllabary_[i] = s;
        syllable_to_id_[s] = i;
    }

    const uint8_t* after_syl = (const uint8_t*)(syl_strs + syl_str_size);

    // v3: offsets table + ID data stored directly in the loaded file buffer.
    const uint32_t* id_offsets = (const uint32_t*)after_syl;
    const uint8_t*  id_data    = after_syl + idx_count * 4;
    const uint8_t*  id_end     = id_data + idx_data_size;
    if (after_syl + (uint64_t)idx_count * 4 > (const uint8_t*)file_end ||
        id_end > (const uint8_t*)file_end) {
        unload_id_index();
        return false;
    }

    id_index_.clear();
    id_index_.reserve(idx_count);
    for (uint32_t i = 0; i < idx_count; ++i) {
        if (id_offsets[i] >= idx_data_size) {
            unload_id_index();
            return false;
        }
        const uint8_t* e = id_data + id_offsets[i];
        if (e + 8 > id_end) {
            unload_id_index();
            return false;
        }
        uint32_t cnt = *(const uint32_t*)e;
        if (e + 4 + (uint64_t)cnt * 4 + 4 > id_end) {
            unload_id_index();
            return false;
        }
        uint32_t dict_index = *(const uint32_t*)(e + 4 + cnt * 4);
        if (dict_index >= dict_entry_count_) {
            unload_id_index();
            return false;
        }
        id_index_.push_back({(const uint32_t*)(e + 4), cnt,
                                dict_index});
    }

    CXXIME_LOG(L"Dict::load_id_index v%u syllables=%u idx=%zu",
               ver, syl_count, id_index_.size());
    return true;
}

void Dict::build_syllabary() {
    syllabary_.clear();
    syllable_to_id_.clear();

    for (uint32_t i = 0; i < dict_entry_count_; ++i) {
        const auto& e = dict_entries_[i];
        const char* sid = dict_strings_ + e.syllable_ids_offset;
        uint32_t len = e.syllable_ids_len;

        // Split by ':'
        uint32_t start = 0;
        for (uint32_t j = 0; j <= len; ++j) {
            if (j == len || sid[j] == ':') {
                if (j > start) {
                    std::string syl(sid + start, j - start);
                    if (syllable_to_id_.find(syl) == syllable_to_id_.end()) {
                        syllable_to_id_[syl] = (uint32_t)syllabary_.size();
                        syllabary_.push_back(syl);
                    }
                }
                start = j + 1;
            }
        }
    }

    CXXIME_LOG(L"Dict::build_syllabary %zu syllables", syllabary_.size());
}

void Dict::build_id_index() {
    id_index_.clear();
    runtime_ids_.clear();
    runtime_ids_.reserve(dict_entry_count_);
    id_index_.reserve(dict_entry_count_);

    for (uint32_t i = 0; i < dict_entry_count_; ++i) {
        const auto& e = dict_entries_[i];
        const char* sid = dict_strings_ + e.syllable_ids_offset;
        uint32_t len = e.syllable_ids_len;

        std::vector<uint32_t> ids;
        uint32_t start = 0;
        for (uint32_t j = 0; j <= len; ++j) {
            if (j == len || sid[j] == ':') {
                if (j > start) {
                    std::string syl(sid + start, j - start);
                    auto it = syllable_to_id_.find(syl);
                    if (it != syllable_to_id_.end())
                        ids.push_back(it->second);
                }
                start = j + 1;
            }
        }
        runtime_ids_.push_back(std::move(ids));
    }

    for (uint32_t i = 0; i < dict_entry_count_; ++i) {
        auto& v = runtime_ids_[i];
        if (!v.empty())
            id_index_.push_back({v.data(), (uint32_t)v.size(), i});
    }

    std::sort(id_index_.begin(), id_index_.end(),
        [](const IdEntry& a, const IdEntry& b) {
            uint32_t n = a.count < b.count ? a.count : b.count;
            for (uint32_t k = 0; k < n; ++k) {
                if (a.ids[k] < b.ids[k]) return true;
                if (a.ids[k] > b.ids[k]) return false;
            }
            return a.count < b.count;
        });

    CXXIME_LOG(L"Dict::build_id_index %zu entries", id_index_.size());
}

uint32_t Dict::syllable_to_id(const std::string& syllable) const {
    auto it = syllable_to_id_.find(syllable);
    return it != syllable_to_id_.end() ? it->second : UINT32_MAX;
}

bool Dict::has_prefix(const std::vector<uint32_t>& query_ids, QueryTrace* trace) const {
    if (query_ids.empty() || id_index_.empty())
        return false;
    auto ids_less = [&](const IdEntry& e, const std::vector<uint32_t>& q) {
        uint32_t n = e.count < q.size() ? e.count : (uint32_t)q.size();
        for (uint32_t k = 0; k < n; ++k) {
            if (e.ids[k] < q[k]) return true;
            if (e.ids[k] > q[k]) return false;
        }
        return e.count < q.size();
    };
    uint32_t lo = 0, hi = (uint32_t)id_index_.size();
    uint32_t steps = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        ++steps;
        if (ids_less(id_index_[mid], query_ids))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= (uint32_t)id_index_.size())
        return false;
    const auto& e = id_index_[lo];
    if (e.count < query_ids.size())
        return false;
    for (size_t k = 0; k < query_ids.size(); ++k)
        if (e.ids[k] != query_ids[k]) return false;

    if (trace)
        trace->prefix_scan_count += steps;

    return true;
}

std::vector<Candidate> Dict::lookup_by_ids(const std::vector<uint32_t>& query_ids, int limit,
                                            QueryTrace* trace, const QueryBudget* budget) {
    std::vector<Candidate> results;
    if (query_ids.empty() || id_index_.empty())
        return results;

    // Binary search: compare (ids_ptr, count) tuples
    auto ids_less = [&](const IdEntry& e, const std::vector<uint32_t>& q) {
        uint32_t n = e.count < q.size() ? e.count : (uint32_t)q.size();
        for (uint32_t k = 0; k < n; ++k) {
            if (e.ids[k] < q[k]) return true;
            if (e.ids[k] > q[k]) return false;
        }
        return e.count < q.size();
    };

    uint32_t lo = 0, hi = (uint32_t)id_index_.size();
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ids_less(id_index_[mid], query_ids))
            lo = mid + 1;
        else
            hi = mid;
    }

    // Determine TopK capacity: min(limit, max_results_before_merge)
    // The collector should not retain more results than the caller requests.
    size_t topk_cap = (size_t)limit;
    if (budget && budget->max_results_before_merge > 0 &&
        (size_t)budget->max_results_before_merge < topk_cap)
        topk_cap = (size_t)budget->max_results_before_merge;
    TopKCollector collector(topk_cap);

    // Dedup via linear scan on collector items (bounded by topk_cap, typically ≤128)
    bool deadline_hit = false;

    // First pass: exact matches
    auto ids_eq = [&](const IdEntry& e) {
        if (e.count != query_ids.size()) return false;
        for (size_t k = 0; k < query_ids.size(); ++k)
            if (e.ids[k] != query_ids[k]) return false;
        return true;
    };

    uint32_t pos = lo;
    uint32_t exact_count = 0;
    uint32_t check_interval = budget ? budget->deadline.check_interval : 64;
    const bool filter_disabled = disabled_system_entry_count() != 0;

    // Upstream work may already have exhausted the deadline before the scan starts.
    if (budget && budget->deadline.enabled && budget->deadline.expired()) {
        deadline_hit = true;
        if (trace) {
            trace->deadline_exceeded = true;
            trace->truncated = true;
        }
    }

    while (!deadline_hit && pos < (uint32_t)id_index_.size() && ids_eq(id_index_[pos])) {
        // Check scan budget
        if (budget && exact_count >= budget->max_exact_scan) {
            if (trace) {
                trace->truncated = true;
                trace->scan_budget_truncated = true;
            }
            break;
        }
        // Check the deadline every check_interval entries.
        if (budget && exact_count > 0 && exact_count % check_interval == 0 && budget->deadline.expired()) {
            deadline_hit = true;
            if (trace) {
                trace->deadline_exceeded = true;
                trace->truncated = true;
            }
            break;
        }
        Candidate c;
        fill_system_candidate(id_index_[pos].index, c, kExactSpanCollectionBoost);
        if (filter_disabled && is_system_entry_disabled(c.text)) {
            ++pos;
            ++exact_count;
            continue;
        }
        if (!contains_text(collector.items(), c.text)) {
            if (collector.full() && trace) {
                trace->truncated = true;
                trace->topk_truncated = true;
            }
            collector.offer(std::move(c));
        }
        ++pos;
        ++exact_count;
    }

    // Second pass: prefix matches (skip if deadline already hit)
    uint32_t prefix_count = 0;
    if (!deadline_hit) {
        // Check the deadline before starting the prefix scan.
        if (budget && budget->deadline.enabled && budget->deadline.expired()) {
            deadline_hit = true;
            if (trace) {
                trace->deadline_exceeded = true;
                trace->truncated = true;
            }
        }
    }
    if (!deadline_hit) {
        auto ids_prefix = [&](const IdEntry& e) {
            if (e.count < query_ids.size()) return false;
            for (size_t k = 0; k < query_ids.size(); ++k)
                if (e.ids[k] != query_ids[k]) return false;
            return true;
        };

        while (pos < (uint32_t)id_index_.size() && ids_prefix(id_index_[pos])) {
            // Check scan budget
            if (budget && prefix_count >= budget->max_prefix_scan) {
                if (trace) {
                    trace->truncated = true;
                    trace->scan_budget_truncated = true;
                }
                break;
            }
            // Check the deadline every check_interval entries.
            if (budget && prefix_count > 0 && prefix_count % check_interval == 0 && budget->deadline.expired()) {
                if (trace) {
                    trace->deadline_exceeded = true;
                    trace->truncated = true;
                }
                break;
            }
            Candidate c;
            fill_system_candidate(id_index_[pos].index, c, 0);
            if (filter_disabled && is_system_entry_disabled(c.text)) {
                ++pos;
                ++prefix_count;
                continue;
            }
            if (!contains_text(collector.items(), c.text)) {
                if (collector.full() && trace) {
                    trace->truncated = true;
                    trace->topk_truncated = true;
                }
                collector.offer(std::move(c));
            }
            ++pos;
            ++prefix_count;
        }
    }

    if (trace) {
        trace->exact_scan_count += exact_count;
        trace->prefix_scan_count += prefix_count;
    }

    results = collector.finish();
    if ((int)results.size() > limit)
        results.resize(limit);

    return results;
}

} // namespace cxxime
