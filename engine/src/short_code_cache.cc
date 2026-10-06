// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/short_code_cache.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include <windows.h>

#include <cxxime/logging.h>
#include <cxxime/query_trace.h>

#include "short_code_cache_format.h"

namespace cxxime {

namespace {

struct ShortCacheView {
    const uint32_t* code_index = nullptr;
    const ShortPostingList* posting_lists = nullptr;
    const ShortCandidatePosting* postings = nullptr;
};

bool advance_region(uint64_t* cursor, uint32_t offset, uint64_t size, size_t file_size) {
    if (*cursor > file_size || offset != *cursor || size > file_size - *cursor) {
        return false;
    }
    *cursor += size;
    return true;
}

uint32_t darts_offset(uint32_t unit) {
    return (unit >> 10) << ((unit & (1U << 9)) >> 6);
}

bool parse_short_cache(const char* data, size_t size, CandidateStoreView candidate_store,
                       ShortCacheView* view, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (size < sizeof(ShortCacheHeader) ||
        size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return fail("invalid file size");
    }
    if (candidate_store.entries == nullptr || candidate_store.strings == nullptr ||
        candidate_store.entry_count == 0 || candidate_store.fingerprint == 0) {
        return fail("invalid candidate store");
    }

    const auto* header = reinterpret_cast<const ShortCacheHeader*>(data);
    if (std::memcmp(header->magic, kShortCacheMagic, sizeof(header->magic)) != 0 ||
        header->version != kShortCacheVersion ||
        header->header_size != sizeof(ShortCacheHeader) ||
        header->file_size != size || header->reserved != 0) {
        return fail("invalid CXTOPN v4 header");
    }
    if (header->key_count == 0 || header->code_index_count == 0 ||
        header->posting_list_count != header->key_count ||
        header->dictionary_entry_count != candidate_store.entry_count ||
        header->dictionary_fingerprint != candidate_store.fingerprint) {
        return fail("candidate store does not match the Top-N index");
    }

    uint64_t cursor = sizeof(ShortCacheHeader);
    if (!advance_region(&cursor, header->code_index_offset,
                        static_cast<uint64_t>(header->code_index_count) * sizeof(uint32_t),
                        size) ||
        !advance_region(&cursor, header->posting_lists_offset,
                        static_cast<uint64_t>(header->posting_list_count) *
                            sizeof(ShortPostingList),
                        size) ||
        !advance_region(&cursor, header->postings_offset,
                        static_cast<uint64_t>(header->posting_count) *
                            sizeof(ShortCandidatePosting),
                        size) ||
        cursor != size) {
        return fail("CXTOPN v4 sections are not canonical");
    }

    const auto* posting_lists = reinterpret_cast<const ShortPostingList*>(
        data + header->posting_lists_offset);
    const auto* postings = reinterpret_cast<const ShortCandidatePosting*>(
        data + header->postings_offset);
    if (header->posting_count > kShortPostingOffsetMask ||
        short_posting_offset(posting_lists[0]) != 0) {
        return fail("invalid Top-N posting list range");
    }
    for (uint32_t i = 0; i < header->posting_list_count; ++i) {
        const uint32_t begin = short_posting_offset(posting_lists[i]);
        const uint32_t end = i + 1 < header->posting_list_count
                                 ? short_posting_offset(posting_lists[i + 1])
                                 : header->posting_count;
        if (begin > end || end > header->posting_count) {
            return fail("invalid Top-N posting list");
        }
    }
    for (uint32_t i = 0; i < header->posting_count; ++i) {
        if (postings[i].dictionary_entry_index >= candidate_store.entry_count) {
            return fail("Top-N posting contains an invalid dictionary reference");
        }
    }

    view->code_index = reinterpret_cast<const uint32_t*>(data + header->code_index_offset);
    view->posting_lists = posting_lists;
    view->postings = postings;
    return true;
}

} // namespace

ShortCodeCache::~ShortCodeCache() {
    unload();
}

bool ShortCodeCache::load(const std::string& path, CandidateStoreView candidate_store) {
    unload();
    CXXIME_LOG(L"ShortCodeCache::load path=%S", path.c_str());

    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CXXIME_LOG(L"ShortCodeCache::load CreateFileA FAILED");
        return false;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li) ||
        li.QuadPart < static_cast<LONGLONG>(sizeof(ShortCacheHeader)) ||
        static_cast<uint64_t>(li.QuadPart) > std::numeric_limits<uint32_t>::max()) {
        CloseHandle(hFile);
        CXXIME_LOG(L"ShortCodeCache::load invalid file size");
        return false;
    }
    data_size_ = static_cast<size_t>(li.QuadPart);
    data_ = new (std::nothrow) char[data_size_];
    if (!data_) {
        CloseHandle(hFile);
        CXXIME_LOG(L"ShortCodeCache::load allocation failed (%zu bytes)", data_size_);
        return false;
    }

    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(hFile, data_, static_cast<DWORD>(data_size_),
                             &bytes_read, nullptr);
    CloseHandle(hFile);
    if (!ok || bytes_read != data_size_) {
        CXXIME_LOG(L"ShortCodeCache::load ReadFile FAILED");
        unload();
        return false;
    }

    ShortCacheView view;
    std::string error;
    if (!parse_short_cache(data_, data_size_, candidate_store, &view, &error)) {
        CXXIME_LOG(L"ShortCodeCache::load format rejected: %S", error.c_str());
        unload();
        return false;
    }

    const auto* header = reinterpret_cast<const ShortCacheHeader*>(data_);
    code_index_ = view.code_index;
    posting_lists_ = view.posting_lists;
    postings_ = view.postings;
    candidate_store_ = candidate_store;
    code_index_count_ = header->code_index_count;
    posting_list_count_ = header->posting_list_count;
    posting_count_ = header->posting_count;
    CXXIME_LOG(L"ShortCodeCache::load OK keys=%u units=%u postings=%u",
               header->key_count, header->code_index_count, header->posting_count);
    return true;
}

void ShortCodeCache::unload() {
    delete[] data_;
    data_ = nullptr;
    data_size_ = 0;
    code_index_ = nullptr;
    posting_lists_ = nullptr;
    postings_ = nullptr;
    candidate_store_ = {};
    code_index_count_ = 0;
    posting_list_count_ = 0;
    posting_count_ = 0;
}

std::vector<Candidate> ShortCodeCache::lookup(const std::string& key, int limit,
                                              QueryTrace* trace,
                                              bool* prefix_complete) const {
    std::vector<Candidate> results;
    if (prefix_complete != nullptr) {
        *prefix_complete = false;
    }
    if (!code_index_ || key.empty() || limit <= 0) {
        return results;
    }

    uint32_t node = 0;
    uint32_t unit = code_index_[node];
    for (char character : key) {
        const uint32_t label = static_cast<unsigned char>(character);
        node ^= darts_offset(unit) ^ label;
        if (node >= code_index_count_) {
            return results;
        }
        unit = code_index_[node];
        if ((unit & ((1U << 31) | 0xFF)) != label) {
            return results;
        }
    }
    if (((unit >> 8) & 1U) == 0) {
        return results;
    }

    const uint32_t leaf = node ^ darts_offset(unit);
    if (leaf >= code_index_count_) {
        return results;
    }
    const uint32_t leaf_unit = code_index_[leaf];
    if ((leaf_unit & (1U << 31)) == 0) {
        return results;
    }
    const uint32_t posting_list_index = leaf_unit & ((1U << 31) - 1);
    if (posting_list_index >= posting_list_count_) {
        return results;
    }

    const auto& list = posting_lists_[posting_list_index];
    if (prefix_complete != nullptr) {
        *prefix_complete = short_posting_prefix_complete(list);
    }
    const uint32_t posting_offset = short_posting_offset(list);
    const uint32_t posting_end = posting_list_index + 1 < posting_list_count_
                                     ? short_posting_offset(posting_lists_[posting_list_index + 1])
                                     : posting_count_;
    const size_t count = std::min<size_t>(posting_end - posting_offset,
                                          static_cast<size_t>(limit));
    results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& posting = postings_[posting_offset + i];
        const auto& entry = candidate_store_.entries[posting.dictionary_entry_index];
        Candidate candidate;
        candidate.text.assign(candidate_store_.strings + entry.text_offset,
                              entry.text_len);
        candidate.syllables.assign(
            candidate_store_.strings + entry.syllable_ids_offset,
            entry.syllable_ids_len);
        candidate.code.reserve(candidate.syllables.size());
        for (char character : candidate.syllables) {
            if (character != ':') {
                candidate.code.push_back(character);
            }
        }
        candidate.frequency = posting.score;
        candidate.origin = CandidateOrigin::kCache;
        candidate.source_frequency = entry.frequency;
        results.push_back(std::move(candidate));
    }

    if (!results.empty() && trace != nullptr) {
        trace->cache_hit = true;
    }
    return results;
}

} // namespace cxxime
