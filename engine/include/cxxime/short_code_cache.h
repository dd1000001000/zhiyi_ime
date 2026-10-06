// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Pre-built Top-N lookup indexed by pinyin input code.

#ifndef CXXIME_SHORT_CODE_CACHE_H_
#define CXXIME_SHORT_CODE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/candidate_store.h>

namespace cxxime {

struct QueryTrace;
struct ShortCandidatePosting;
struct ShortPostingList;

class ShortCodeCache {
public:
    ShortCodeCache() = default;
    ~ShortCodeCache();
    ShortCodeCache(const ShortCodeCache&) = delete;
    ShortCodeCache& operator=(const ShortCodeCache&) = delete;

    bool load(const std::string& path, CandidateStoreView candidate_store);
    void unload();
    bool is_loaded() const { return data_ != nullptr; }

    // Return up to `limit` candidates for an exact input-code match.
    // Sets trace->cache_hit = true on successful lookup.
    std::vector<Candidate> lookup(const std::string& key, int limit,
                                   QueryTrace* trace = nullptr,
                                   bool* prefix_complete = nullptr) const;

private:
    char* data_ = nullptr;
    size_t data_size_ = 0;
    const uint32_t* code_index_ = nullptr;
    const ShortPostingList* posting_lists_ = nullptr;
    const ShortCandidatePosting* postings_ = nullptr;
    CandidateStoreView candidate_store_;
    uint32_t code_index_count_ = 0;
    uint32_t posting_list_count_ = 0;
    uint32_t posting_count_ = 0;
};

} // namespace cxxime

#endif // CXXIME_SHORT_CODE_CACHE_H_
