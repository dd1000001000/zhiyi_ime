// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TRANSLATOR_H_
#define CXXIME_TRANSLATOR_H_

#include <memory>
#include <string>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/dict.h>
#include <cxxime/pinyin_resource.h>
#include <cxxime/segmentor.h>
#include <cxxime/translation_result.h>

namespace cxxime {

class ShortCodeCache;
class CompositionLearningService;
struct QueryTrace;
struct QueryBudget;
struct QueryScratch;

// Abstract translator interface
class ITranslator {
public:
    virtual ~ITranslator() = default;
    virtual TranslationResult translate(const TranslationRequest& request) = 0;
    virtual void clear_query_cache() {}
    virtual void set_sentence_composition_enabled(bool enabled) {}
    virtual void set_candidate_learning_enabled(bool enabled) {}
    virtual void set_composition_learning_service(CompositionLearningService* service) {}
};

// Pinyin translator implementation
class PinyinTranslator : public ITranslator {
public:
    void set_dict(Dict* dict);
    void bind_pinyin(std::shared_ptr<const PinyinResourceSet> resources,
                     PinyinQueryPolicy policy);
    void set_short_cache(const ShortCodeCache* cache) { short_cache_ = cache; }

    void clear_query_cache() override { query_cache_.clear(); }
    void set_sentence_composition_enabled(bool enabled) override;
    void set_candidate_learning_enabled(bool enabled) override;
    void set_composition_learning_service(CompositionLearningService* service) override;

    TranslationResult translate(const TranslationRequest& request) override;
    CandidatePage translate_page(const std::string& pinyin, int page_index = 0,
                                 int page_size = 9, QueryTrace* trace = nullptr,
                                 const QueryBudget* budget = nullptr,
                                 QueryScratch* scratch = nullptr,
                                 int candidate_offset = -1,
                                 bool require_runtime_paths = false);

private:
    static bool is_indexable_key(const std::string& pinyin);
    struct IndexedFastResult {
        bool hit = false;
        bool complete_index_hit = false;
        std::vector<Candidate> candidates;
    };
    struct QueryCacheEntry {
        std::string input;
        int page_index = 0;
        int candidate_offset = 0;
        int page_size = 0;
        uint64_t user_dict_version = 0;
        uint64_t candidate_preference_version = 0;
        uint64_t manual_candidate_order_version = 0;
        uint64_t disabled_system_entry_version = 0;
        uint64_t composition_learning_version = 0;
        uint64_t sequence = 0;
        CandidatePage page;
    };
    struct QueryCacheVersions {
        uint64_t user_dict = 0;
        uint64_t candidate_preference = 0;
        uint64_t manual_candidate_order = 0;
        uint64_t disabled_system_entry = 0;
        uint64_t composition_learning = 0;
    };
    IndexedFastResult lookup_indexed_fast(const std::string& key, int limit,
                                          QueryTrace* trace) const;
    QueryCacheVersions query_cache_versions() const;
    bool lookup_query_cache(const std::string& input, int page_index, int candidate_offset,
                            int page_size, const QueryCacheVersions& versions,
                            CandidatePage& page, QueryTrace* trace);
    void store_query_cache(const std::string& input, int page_index, int candidate_offset,
                           int page_size, const QueryCacheVersions& versions,
                           const CandidatePage& page);
    PinyinSchemeKind pinyin_scheme() const;
    Dict* dict_ = nullptr;
    std::shared_ptr<const PinyinResourceSet> pinyin_resources_;
    PinyinQueryPolicy pinyin_query_policy_;
    // Syllabifier options for the enabled fuzzy pinyin groups.
    SyllabifierOptions fuzzy_options() const;
    // Fuzzy pinyin: sets the correct pinyin as the comment of entries [begin, end) that the
    // input reaches only through fuzzy spellings.
    void annotate_fuzzy_matches(const std::string& input, std::vector<CandidateEntry>& entries,
                                int begin, int end) const;
    // Initials mode: drops candidates whose syllables do not match `pinyin` as initials.
    void keep_initials_matches(const std::string& pinyin, std::vector<Candidate>& candidates) const;
    const ShortCodeCache* short_cache_ = nullptr;
    PinyinSegmentor segmentor_;

    std::vector<QueryCacheEntry> query_cache_;
    uint64_t query_cache_sequence_ = 0;
    bool sentence_composition_enabled_ = true;
    bool candidate_learning_enabled_ = false;
    CompositionLearningService* composition_learning_ = nullptr;
    static constexpr size_t kMaxQueryCacheEntries = 64;
};

} // namespace cxxime

#endif // CXXIME_TRANSLATOR_H_
