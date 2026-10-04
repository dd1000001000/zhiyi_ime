// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Query benchmark integration, regression thresholds, and trace semantics.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include <cxxime/engine.h>
#include <cxxime/query_trace.h>

#include "support/testutil.h"
#include "support/test_runtime.h"

// Build a path relative to the project root.
static std::string project_path(const char* rel) {
    return std::string(CXXIME_PROJECT_DIR) + rel;
}

class BenchmarkEngineFixture {
public:
    explicit BenchmarkEngineFixture(const char* tag)
        : user_dict_path_(project_path(
              (std::string("data/_bench_user_") + tag + ".tsv").c_str())) {
        std::remove(user_dict_path_.c_str());
    }

    ~BenchmarkEngineFixture() {
        engine.finalize();
        dict_->close();
        std::remove(user_dict_path_.c_str());
    }

    bool initialize(bool load_topn = true) {
        std::string dict_path = project_path("data/pinyin.dict.bin");
        std::string topn_path = load_topn
            ? project_path("data/pinyin.topn.bin")
            : std::string();
        if (!dict_->open_bundle(dict_path, user_dict_path_,
                               project_path("data/pinyin.dict.idx"), topn_path))
            return false;

        config_.load(project_path("data/default.json"));

        const std::string sp_path = cxxime::Engine::derive_spellings_path(dict_path);
        pinyin_resources_ = cxxime::PinyinResourceSet::create(
            "full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin, sp_path);
        return engine.initialize(cxxime::EngineRuntimeState::create(
            config_, dict_, nullptr, pinyin_resources_));
    }

    void set_page_size(int page_size) {
        config_.page_size = page_size;
        if (pinyin_resources_) {
            ASSERT_TRUE(engine.apply_runtime_state(cxxime::EngineRuntimeState::create(
                config_, dict_, nullptr, pinyin_resources_)));
        }
    }

    cxxime::Engine engine;

private:
    std::shared_ptr<cxxime::Dict> dict_ =
        std::make_shared<cxxime::Dict>(cxxime::UserDictKind::PINYIN);
    std::shared_ptr<const cxxime::PinyinResourceSet> pinyin_resources_;
    cxxime::Config config_;
    std::string user_dict_path_;
};

TEST(Benchmark, TraceFieldsPopulated) {
    BenchmarkEngineFixture fixture("trace_fields");
    auto& engine = fixture.engine;

    ASSERT_TRUE(fixture.initialize(false));

    // Disable deadline for this test — we want to verify trace fields are populated,
    // not that deadline is respected. Debug builds are slow and may exceed 30ms.
    engine.set_query_deadline_ms(0);

    engine.set_trace_enabled(true);

    // Top-N is disabled for this fixture so the input exercises the full
    // syllabifier and dictionary lookup pipeline.
    const char* input = "zhonghuarenmin";  // a dictionary word (the compact dictionary has no 你好世界)
    for (const char* p = input; *p; ++p) {
        cxxime::KeyEvent event;
        event.keycode = *p - 'a' + 'A';  // Convert to uppercase VK code
        event.is_key_up = false;
        engine.process_key(event);
    }

    const auto& trace = engine.last_trace();

    printf("Trace fields for 'zhonghuarenmin':\n");
    printf("  total_us: %lld\n", trace.total_us);
    printf("  processor_us: %lld\n", trace.processor_us);
    printf("  translate_us: %lld\n", trace.translate_us);
    printf("  syllable_path_count: %d\n", trace.syllable_path_count);
    printf("  live_path_count: %d\n", trace.live_path_count);
    printf("  candidate_count: %d\n", trace.candidate_count);
    printf("  exact_scan_count: %u\n", trace.exact_scan_count);
    printf("  prefix_scan_count: %u\n", trace.prefix_scan_count);
    printf("  cache_hit: %d\n", trace.cache_hit ? 1 : 0);

    // Verify trace fields are populated
    ASSERT_GT(trace.total_us, 0) << "total_us should be > 0";
    // processor_us may be 0 for very fast operations (sub-microsecond)
    // so we don't enforce it to be > 0
    ASSERT_GT(trace.translate_us, 0) << "translate_us should be > 0";
    ASSERT_GT(trace.syllable_path_count, 0) << "syllable_path_count should be > 0";
    ASSERT_GT(trace.live_path_count, 0) << "live_path_count should be > 0";
    ASSERT_GT(trace.candidate_count, 0) << "candidate_count should be > 0";

    engine.finalize();
}

TEST(Benchmark, FallbackQueryCacheHitOnRepeat) {
    BenchmarkEngineFixture fixture("fallback_query_cache");
    auto& engine = fixture.engine;

    ASSERT_TRUE(fixture.initialize(false));

    engine.set_query_deadline_ms(0);
    engine.set_trace_enabled(true);

    const char* input = "nihao";
    for (const char* p = input; *p; ++p) {
        cxxime::KeyEvent event;
        event.keycode = *p - 'a' + 'A';
        event.is_key_up = false;
        engine.process_key(event);
    }
    auto first = engine.last_trace();
    ASSERT_TRUE(!first.cache_hit) << "First query should exercise the full pipeline";
    ASSERT_GT(first.exact_scan_count + first.prefix_scan_count + first.user_scan_count, 0u);

    engine.clear();
    for (const char* p = input; *p; ++p) {
        cxxime::KeyEvent event;
        event.keycode = *p - 'a' + 'A';
        event.is_key_up = false;
        engine.process_key(event);
    }
    const auto& second = engine.last_trace();
    ASSERT_TRUE(second.cache_hit) << "Repeated fallback query should hit query page cache";
    ASSERT_EQ(second.exact_scan_count, 0u);
    ASSERT_EQ(second.prefix_scan_count, 0u);
    ASSERT_EQ(second.user_scan_count, 0u);
    ASSERT_EQ(second.syllable_path_count, 0);
    ASSERT_EQ(second.live_path_count, 0);

    engine.finalize();
}

// Benchmark test helpers

#ifdef _WIN32
inline int get_exit_code(int rc) { return rc; }
#else
inline int get_exit_code(int rc) { return WEXITSTATUS(rc); }
#endif

static std::string format_trace_json(const cxxime::QueryTrace& trace, const std::string& input,
                                     const char* mode, int repeat_index,
                                     int page_size, int deadline_ms) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"input\":\"%s\",\"repeat_index\":%d,"
        "\"mode\":\"%s\",\"page_size\":%d,\"deadline_ms\":%d,"
        "\"elapsed_us\":%lld,"
        "\"processor_us\":%lld,\"translate_us\":%lld,"
        "\"lookup_us\":%lld,\"merge_us\":%lld,"
        "\"candidate_count\":%d,"
        "\"exact_scan_count\":%u,\"prefix_scan_count\":%u,\"user_scan_count\":%u,"
        "\"syllable_path_count\":%d,\"live_path_count\":%d,"
        "\"cache_hit\":%s,\"truncated\":%s,\"deadline_exceeded\":%s}",
        input.c_str(), repeat_index,
        mode, page_size, deadline_ms,
        (long long)trace.total_us,
        (long long)trace.processor_us, (long long)trace.translate_us,
        (long long)trace.lookup_us, (long long)trace.merge_us,
        trace.candidate_count,
        trace.exact_scan_count, trace.prefix_scan_count, trace.user_scan_count,
        trace.syllable_path_count, trace.live_path_count,
        trace.cache_hit ? "true" : "false",
        trace.truncated ? "true" : "false",
        trace.deadline_exceeded ? "true" : "false");
    return std::string(buf);
}

// page_size affects candidate_count

TEST(Benchmark, PageSizeAffectsCandidates) {
    BenchmarkEngineFixture fixture("page_size");
    auto& engine = fixture.engine;
    ASSERT_TRUE(fixture.initialize());

    engine.set_query_deadline_ms(0);
    engine.set_trace_enabled(true);

    const std::string input = "nihaoshijie";

    // Run with page_size=3
    fixture.set_page_size(3);
    engine.clear_composition();
    for (char c : input) {
        cxxime::KeyEvent event;
        event.keycode = c - 'a' + 'A';
        event.is_key_up = false;
        engine.process_key(event);
    }
    int cands_small = engine.last_trace().candidate_count;

    // Run with page_size=7
    fixture.set_page_size(7);
    engine.clear_composition();
    for (char c : input) {
        cxxime::KeyEvent event;
        event.keycode = c - 'a' + 'A';
        event.is_key_up = false;
        engine.process_key(event);
    }
    int cands_large = engine.last_trace().candidate_count;

    printf("PageSize test: page_size=3 -> %d cands, page_size=7 -> %d cands\n",
           cands_small, cands_large);

    ASSERT_LE(cands_small, 3) << "page_size=3 should limit candidates to <= 3";
    ASSERT_GE(cands_large, cands_small) << "page_size=7 should return >= page_size=3 candidates";

    engine.finalize();
}

// check_query_bench.py threshold pass/fail

static int run_check_script(const std::string& threshold_path, const std::string& jsonl_path,
                            const std::string& output_dir) {
    std::string script = project_path("scripts/check_query_bench.py");
    std::string cmd = "python \"" + script + "\" --input \"" + jsonl_path +
                      "\" --threshold \"" + threshold_path +
                      "\" --output-dir \"" + output_dir + "\"";
    return std::system(cmd.c_str());
}

TEST(Benchmark, CheckQueryBenchPass) {
    // Create a minimal JSONL with relaxed timing
    std::string jsonl_path = project_path("data/_bench_check_pass.jsonl");
    std::string output_dir = project_path("data/_bench_reports");
    std::string threshold_path = project_path("tools/query_bench/thresholds.local.json");

    {
        BenchmarkEngineFixture fixture("check_pass");
        auto& engine = fixture.engine;
    ASSERT_TRUE(fixture.initialize());

        engine.set_query_deadline_ms(0);
        engine.set_trace_enabled(true);
        fixture.set_page_size(7);

        std::ofstream f(jsonl_path);
        // Run "s" 10 times — short input, should be fast
        for (int i = 0; i < 10; ++i) {
            engine.clear_composition();
            cxxime::KeyEvent event;
            event.keycode = 'S';
            event.is_key_up = false;
            engine.process_key(event);
            f << format_trace_json(engine.last_trace(), "s", "final_key", i, 7, 0) << "\n";
        }
        engine.finalize();
    }

    int rc = run_check_script(threshold_path, jsonl_path, output_dir);
    int exit_code = get_exit_code(rc);
    printf("CheckQueryBenchPass: exit_code=%d\n", exit_code);
    ASSERT_EQ(exit_code, 0) << "check_query_bench.py should pass with relaxed threshold";

    std::remove(jsonl_path.c_str());
}

TEST(Benchmark, CheckQueryBenchFail) {
    // Create a JSONL with artificially slow timing (100000us = 100ms)
    std::string jsonl_path = project_path("data/_bench_check_fail.jsonl");
    std::string output_dir = project_path("data/_bench_reports_fail");
    std::string threshold_path = project_path("tools/query_bench/thresholds.local.json");

    {
        // Write a hand-crafted JSONL line with p95 > 1000 (the short_inputs threshold)
        std::ofstream f(jsonl_path);
        for (int i = 0; i < 10; ++i) {
            // 50000us > short_inputs p95_us=1000
            char buf[512];
            snprintf(buf, sizeof(buf),
                "{\"input\":\"s\",\"repeat_index\":%d,"
                "\"mode\":\"final_key\",\"page_size\":7,\"deadline_ms\":0,"
                "\"elapsed_us\":50000,"
                "\"processor_us\":10,\"translate_us\":20,"
                "\"lookup_us\":10,\"merge_us\":5,"
                "\"candidate_count\":7,"
                "\"exact_scan_count\":0,\"prefix_scan_count\":0,\"user_scan_count\":0,"
                "\"syllable_path_count\":1,\"live_path_count\":1,"
                "\"cache_hit\":true,\"truncated\":false,\"deadline_exceeded\":false}",
                i);
            f << buf << "\n";
        }
    }

    int rc = run_check_script(threshold_path, jsonl_path, output_dir);
    int exit_code = get_exit_code(rc);
    printf("CheckQueryBenchFail: exit_code=%d\n", exit_code);
    ASSERT_EQ(exit_code, 3) << "check_query_bench.py should fail with strict threshold";

    std::remove(jsonl_path.c_str());
}

// State field semantic tests

TEST(Benchmark, CacheHitScanZero) {
    // Short input "s" should hit topn cache with all scan counts = 0
    BenchmarkEngineFixture fixture("cache_hit");
    auto& engine = fixture.engine;
    ASSERT_TRUE(fixture.initialize());

    engine.set_query_deadline_ms(0);
    engine.set_trace_enabled(true);
    fixture.set_page_size(7);

    cxxime::KeyEvent event;
    event.keycode = 'S';
    event.is_key_up = false;
    engine.process_key(event);

    const auto& trace = engine.last_trace();
    printf("CacheHitScanZero: cache_hit=%d, exact=%u, prefix=%u, user=%u\n",
           trace.cache_hit ? 1 : 0, trace.exact_scan_count,
           trace.prefix_scan_count, trace.user_scan_count);

    ASSERT_TRUE(trace.cache_hit) << "Short input 's' should hit cache";
    ASSERT_EQ(trace.exact_scan_count, 0u) << "Cache hit should have 0 exact scans";
    ASSERT_EQ(trace.prefix_scan_count, 0u) << "Cache hit should have 0 prefix scans";
    ASSERT_EQ(trace.user_scan_count, 0u) << "Cache hit should have 0 user scans";

    engine.finalize();
}

TEST(Benchmark, CacheMissScanPositive) {
    // With Top-N disabled, the query should miss cache and scan the dictionary.
    BenchmarkEngineFixture fixture("cache_miss");
    auto& engine = fixture.engine;
    ASSERT_TRUE(fixture.initialize(false));

    engine.set_query_deadline_ms(0);
    engine.set_trace_enabled(true);
    fixture.set_page_size(7);

    const char* input = "zhonghuarenmin";  // a dictionary word
    for (const char* p = input; *p; ++p) {
        cxxime::KeyEvent event;
        event.keycode = *p - 'a' + 'A';
        event.is_key_up = false;
        engine.process_key(event);
    }

    const auto& trace = engine.last_trace();
    printf("CacheMissScanPositive: cache_hit=%d, exact=%u, prefix=%u, user=%u\n",
           trace.cache_hit ? 1 : 0, trace.exact_scan_count,
           trace.prefix_scan_count, trace.user_scan_count);

    ASSERT_TRUE(!trace.cache_hit) << "Top-N-disabled query should miss cache";
    uint32_t total_scans = trace.exact_scan_count + trace.prefix_scan_count + trace.user_scan_count;
    ASSERT_GT(total_scans, 0u) << "Cache miss should have scan counts > 0";

    engine.finalize();
}

RUN_ALL_TESTS()
