// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "lexicon_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <ostream>
#include <string_view>
#include <utility>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/dict.h>
#include <cxxime/engine.h>
#include <cxxime/engine_runtime.h>
#include <cxxime/key_event.h>
#include <cxxime/pinyin_resource.h>

#include "system_lexicon_inspector.h"

namespace cxxime::benchmark {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Measurement {
    TimingSummary timing;
    std::size_t result_count = 0;
    std::uint64_t result_checksum = 0;
};

struct EngineWorkload {
    InputMode mode;
    const char* mode_name;
    const char* input;
};

const EngineWorkload kEngineWorkloads[] = {
    {InputMode::PINYIN, "pinyin", "s"},        {InputMode::PINYIN, "pinyin", "nihao"},
    {InputMode::PINYIN, "pinyin", "wushuchu"}, {InputMode::WUBI, "wubi", "d"},
    {InputMode::WUBI, "wubi", "qdr"},          {InputMode::WUBI, "wubi", "utq"},
    {InputMode::MIXED, "mixed", "dd"},         {InputMode::MIXED, "mixed", "nihao"},
};

std::string join_path(const std::string& directory, const char* name) {
    if (directory.empty()) {
        return name;
    }
    const char last = directory.back();
    if (last == '\\' || last == '/') {
        return directory + name;
    }
    return directory + "\\" + name;
}

std::uint64_t append_hash(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t append_hash(std::uint64_t hash, std::string_view value) {
    return append_hash(hash, value.data(), value.size());
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    const std::size_t rank = static_cast<std::size_t>(std::ceil(fraction * sorted.size()));
    return sorted[(std::min)(rank == 0 ? 0 : rank - 1, sorted.size() - 1)];
}

TimingSummary summarize(std::vector<std::uint64_t> samples) {
    std::sort(samples.begin(), samples.end());
    TimingSummary result;
    result.samples = samples.size();
    result.p50_ns = percentile(samples, 0.50);
    result.p95_ns = percentile(samples, 0.95);
    result.p99_ns = percentile(samples, 0.99);
    result.max_ns = samples.empty() ? 0 : samples.back();
    return result;
}

std::uint64_t hash_entries(const std::vector<SystemLexiconEntry>& entries) {
    std::uint64_t hash = kFnvOffset;
    for (const auto& entry : entries) {
        hash = append_hash(hash, entry.text);
        hash = append_hash(hash, entry.code);
        hash = append_hash(hash, &entry.frequency, sizeof(entry.frequency));
        hash = append_hash(hash, &entry.entry_id, sizeof(entry.entry_id));
    }
    return hash;
}

std::uint64_t hash_candidates(const CandidatePage& page) {
    std::uint64_t hash = kFnvOffset;
    for (const auto& candidate : page.candidates) {
        hash = append_hash(hash, candidate.text);
        hash = append_hash(hash, candidate.code);
        hash = append_hash(hash, &candidate.frequency, sizeof(candidate.frequency));
        hash = append_hash(hash, &candidate.source, sizeof(candidate.source));
        hash = append_hash(hash, &candidate.origin, sizeof(candidate.origin));
    }
    return hash;
}

template <typename Query>
bool measure_inspector_query(SystemLexiconInspector* inspector, std::size_t repeat,
                             std::size_t warmup, Query query, Measurement* measurement,
                             std::string* error) {
    for (std::size_t index = 0; index < warmup; ++index) {
        query();
        if (!inspector->last_error().empty()) {
            *error = inspector->last_error();
            return false;
        }
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(repeat);
    for (std::size_t index = 0; index < repeat; ++index) {
        const auto start = Clock::now();
        const auto entries = query();
        const auto end = Clock::now();
        if (!inspector->last_error().empty()) {
            *error = inspector->last_error();
            return false;
        }
        const std::uint64_t checksum = hash_entries(entries);
        if (index == 0) {
            measurement->result_count = entries.size();
            measurement->result_checksum = checksum;
        } else if (measurement->result_count != entries.size() ||
                   measurement->result_checksum != checksum) {
            *error = "Inspector query returned unstable results";
            return false;
        }
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    measurement->timing = summarize(std::move(samples));
    return true;
}

bool add_inspector_queries(const LexiconBenchmarkOptions& options, SystemLexiconType type,
                           const char* variant, const char* dictionary_name,
                           const char* reverse_index_name, std::string_view exact_text,
                           std::string_view prefix_text,
                           const std::vector<std::string_view>& continuous_codes,
                           LexiconBenchmarkReport* report, std::string* error) {
    const std::string dictionary_path = join_path(options.data_directory, dictionary_name);
    const std::string reverse_index_path = join_path(options.data_directory, reverse_index_name);

    Measurement cold;
    std::vector<std::uint64_t> cold_samples;
    cold_samples.reserve(options.repeat);
    for (std::size_t index = 0; index < options.repeat; ++index) {
        SystemLexiconInspector inspector;
        const auto start = Clock::now();
        const bool opened = inspector.open(type, dictionary_path, reverse_index_path);
        const auto entries = opened
                   ? inspector.query_text(exact_text, SystemLexiconTextMatch::kExact,
                                          options.result_limit)
                   : std::vector<SystemLexiconEntry>{};
        const auto end = Clock::now();
        if (!opened || !inspector.last_error().empty()) {
            *error = inspector.last_error().empty() ? "Failed to open system lexicon"
                                                    : inspector.last_error();
            return false;
        }
        const std::uint64_t checksum = hash_entries(entries);
        if (index == 0) {
            cold.result_count = entries.size();
            cold.result_checksum = checksum;
        } else if (cold.result_count != entries.size() || cold.result_checksum != checksum) {
            *error = "Fresh inspector query returned unstable results";
            return false;
        }
        cold_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    cold.timing = summarize(std::move(cold_samples));
    report->records.push_back({"inspector", variant, "cold_open_exact", "fixed_text", cold.timing,
                               cold.result_count, cold.result_checksum});

    SystemLexiconInspector inspector;
    if (!inspector.open(type, dictionary_path, reverse_index_path)) {
        *error = inspector.last_error();
        return false;
    }

    Measurement exact;
    if (!measure_inspector_query(
            &inspector, options.repeat, options.warmup,
            [&] {
                return inspector.query_text(exact_text, SystemLexiconTextMatch::kExact,
                                            options.result_limit);
            },
            &exact, error)) {
        return false;
    }
    report->records.push_back({"inspector", variant, "exact_text", "fixed_text", exact.timing,
                               exact.result_count, exact.result_checksum});

    Measurement prefix;
    if (!measure_inspector_query(
            &inspector, options.repeat, options.warmup,
            [&] {
                return inspector.query_text(prefix_text, SystemLexiconTextMatch::kPrefix,
                                            options.result_limit);
            },
            &prefix, error)) {
        return false;
    }
    report->records.push_back({"inspector", variant, "prefix_text", "fixed_text", prefix.timing,
                               prefix.result_count, prefix.result_checksum});

    Measurement continuous;
    if (!measure_inspector_query(
            &inspector, options.repeat, options.warmup,
            [&] {
                std::vector<SystemLexiconEntry> combined;
                for (const std::string_view code : continuous_codes) {
                    auto entries = inspector.query_code_prefix(code, options.result_limit);
                    combined.insert(combined.end(), std::make_move_iterator(entries.begin()),
                                    std::make_move_iterator(entries.end()));
                }
                return combined;
            },
            &continuous, error)) {
        return false;
    }
    report->records.push_back({"inspector", variant, "continuous_code", "fixed_sequence",
                               continuous.timing, continuous.result_count,
                               continuous.result_checksum});
    return true;
}

bool make_disabled_file(std::string* path, std::string* error) {
    char directory[MAX_PATH] = {};
    char temporary[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, directory) == 0 ||
        GetTempFileNameA(directory, "cxb", 0, temporary) == 0) {
        *error = "Failed to allocate a temporary disabled-entry path";
        return false;
    }
    *path = temporary;
    return true;
}

bool write_small_disabled_set(const std::string& path, std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < 16; ++index) {
        output << "zhiyi-benchmark-disabled-" << std::setw(2) << std::setfill('0') << index
               << '\n';
    }
    if (!output.good()) {
        *error = "Failed to write the temporary disabled-entry set";
        return false;
    }
    return true;
}

Measurement measure_engine_input(Engine* engine, std::string_view input, std::size_t repeat,
                                 std::size_t warmup, std::string* error) {
    Measurement measurement;
    auto run_once = [&]() {
        engine->clear_composition();
        engine->clear_query_cache();
        for (std::size_t index = 0; index + 1 < input.size(); ++index) {
            KeyEvent event;
            event.keycode = static_cast<std::uint32_t>(input[index] - 'a' + 'A');
            engine->process_key(event);
        }
        KeyEvent event;
        event.keycode = static_cast<std::uint32_t>(input.back() - 'a' + 'A');
        const auto start = Clock::now();
        engine->process_key(event);
        const auto end = Clock::now();
        return std::make_pair(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()),
            hash_candidates(engine->context().candidate_page()));
    };

    for (std::size_t index = 0; index < warmup; ++index) {
        run_once();
    }
    std::vector<std::uint64_t> samples;
    samples.reserve(repeat);
    for (std::size_t index = 0; index < repeat; ++index) {
        const auto sample = run_once();
        const std::size_t result_count = engine->context().candidate_page().candidates.size();
        if (index == 0) {
            measurement.result_count = result_count;
            measurement.result_checksum = sample.second;
        } else if (measurement.result_count != result_count ||
                   measurement.result_checksum != sample.second) {
            *error = "Engine query returned unstable results";
            return {};
        }
        samples.push_back(sample.first);
    }
    measurement.timing = summarize(std::move(samples));
    return measurement;
}

double delta_percent(std::uint64_t baseline, std::uint64_t current) {
    if (baseline == 0) {
        return current == 0 ? 0.0 : 100.0;
    }
    return 100.0 * (static_cast<double>(current) - static_cast<double>(baseline)) /
           static_cast<double>(baseline);
}

std::int64_t delta_ns(std::uint64_t baseline, std::uint64_t current) {
    return static_cast<std::int64_t>(current) - static_cast<std::int64_t>(baseline);
}

bool add_engine_queries(const LexiconBenchmarkOptions& options, LexiconBenchmarkReport* report,
                        std::string* error) {
    auto pinyin_dictionary = std::make_shared<Dict>(UserDictKind::PINYIN);
    auto wubi_dictionary = std::make_shared<Dict>(UserDictKind::WUBI);
    Config config;
    const std::string pinyin_path = join_path(options.data_directory, "pinyin.dict.bin");
    const std::string wubi_path = join_path(options.data_directory, "wubi86.dict.bin");
    if (!pinyin_dictionary->open_dict(pinyin_path) ||
        !wubi_dictionary->open_wubi_dict(wubi_path,
                                         join_path(options.data_directory, "wubi86.dict.idx")) ||
        !config.load(join_path(options.data_directory, "default.json"))) {
        *error = "Failed to initialize Engine benchmark resources";
        return false;
    }

    std::string disabled_path;
    if (!make_disabled_file(&disabled_path, error)) {
        return false;
    }
    DeleteFileA(disabled_path.c_str());
    if (!pinyin_dictionary->load_disabled_system_entries(disabled_path) ||
        !wubi_dictionary->load_disabled_system_entries(disabled_path)) {
        *error = "Failed to initialize the empty disabled-entry set";
        return false;
    }

    const auto& scheme = resolve_pinyin_scheme(config.pinyin_scheme);
    auto pinyin_resources = PinyinResourceSet::create(
        scheme.id, scheme.kind, join_path(options.data_directory, scheme.spelling_filename),
        PinyinSpellingRequirement::kRequired);
    Engine engine;
    if (!engine.initialize(EngineRuntimeState::create(
            config, pinyin_dictionary, wubi_dictionary, std::move(pinyin_resources)))) {
        *error = "Failed to initialize Engine benchmark";
        return false;
    }
    engine.set_trace_enabled(false);

    std::vector<LexiconBenchmarkRecord> empty_records;
    for (const EngineWorkload& workload : kEngineWorkloads) {
        engine.switch_mode(workload.mode);
        Measurement measurement =
            measure_engine_input(&engine, workload.input, options.repeat, options.warmup, error);
        if (!error->empty()) {
            DeleteFileA(disabled_path.c_str());
            return false;
        }
        empty_records.push_back({"engine", "disabled_empty", workload.mode_name, workload.input,
                                 measurement.timing, measurement.result_count,
                                 measurement.result_checksum});
    }
    report->records.insert(report->records.end(), empty_records.begin(), empty_records.end());

    if (!write_small_disabled_set(disabled_path, error) ||
        !pinyin_dictionary->load_disabled_system_entries(disabled_path) ||
        !wubi_dictionary->load_disabled_system_entries(disabled_path)) {
        if (error->empty()) {
            *error = "Failed to initialize the small disabled-entry set";
        }
        DeleteFileA(disabled_path.c_str());
        return false;
    }

    for (std::size_t index = 0; index < std::size(kEngineWorkloads); ++index) {
        const EngineWorkload& workload = kEngineWorkloads[index];
        engine.switch_mode(workload.mode);
        Measurement measurement =
            measure_engine_input(&engine, workload.input, options.repeat, options.warmup, error);
        if (!error->empty()) {
            DeleteFileA(disabled_path.c_str());
            return false;
        }
        const LexiconBenchmarkRecord small = {"engine",
                                              "disabled_small_16",
                                              workload.mode_name,
                                              workload.input,
                                              measurement.timing,
                                              measurement.result_count,
                                              measurement.result_checksum};
        const LexiconBenchmarkRecord& empty = empty_records[index];
        report->engine_comparisons.push_back(
            {workload.mode_name, workload.input, delta_ns(empty.timing.p50_ns, small.timing.p50_ns),
             delta_ns(empty.timing.p95_ns, small.timing.p95_ns),
             delta_ns(empty.timing.p99_ns, small.timing.p99_ns),
             delta_percent(empty.timing.p50_ns, small.timing.p50_ns),
             delta_percent(empty.timing.p95_ns, small.timing.p95_ns),
             delta_percent(empty.timing.p99_ns, small.timing.p99_ns),
             empty.result_count == small.result_count &&
                 empty.result_checksum == small.result_checksum});
        report->records.push_back(small);
    }
    DeleteFileA(disabled_path.c_str());
    engine.finalize();

    const bool semantics_equal =
        std::all_of(report->engine_comparisons.begin(), report->engine_comparisons.end(),
                    [](const EngineComparison& comparison) { return comparison.results_equal; });
    if (!semantics_equal) {
        *error = "The empty and small disabled sets changed benchmark candidate semantics";
        return false;
    }
    return true;
}

} // namespace

bool run_lexicon_benchmark(const LexiconBenchmarkOptions& options, LexiconBenchmarkReport* report,
                           std::string* error) {
    if (report == nullptr || error == nullptr || options.data_directory.empty() ||
        options.repeat == 0 || options.result_limit == 0) {
        return false;
    }
    report->records.clear();
    report->engine_comparisons.clear();
    error->clear();

    if (!add_inspector_queries(options, SystemLexiconType::kPinyin, "pinyin", "pinyin.dict.bin",
                               "pinyin.reverse.idx", u8"你好", u8"中", {"n", "ni", "nih", "nihao"},
                               report, error) ||
        !add_inspector_queries(options, SystemLexiconType::kWubi, "wubi", "wubi86.dict.bin",
                               "wubi86.reverse.idx", u8"你", u8"中", {"w", "wq", "wqa", "wqay"},
                               report, error) ||
        !add_engine_queries(options, report, error)) {
        return false;
    }
    return true;
}

void print_lexicon_benchmark_report(const LexiconBenchmarkOptions& options,
                                    const LexiconBenchmarkReport& report, std::ostream& output) {
    output << "lexicon_bench format=1 repeat=" << options.repeat << " warmup=" << options.warmup
           << " limit=" << options.result_limit << '\n';
    for (const auto& record : report.records) {
        output << "record category=" << record.category << " variant=" << record.variant
               << " operation=" << record.operation << " input=" << record.input
               << " samples=" << record.timing.samples << " p50_ns=" << record.timing.p50_ns
               << " p95_ns=" << record.timing.p95_ns << " p99_ns=" << record.timing.p99_ns
               << " max_ns=" << record.timing.max_ns << " results=" << record.result_count
               << " checksum=" << record.result_checksum << '\n';
    }
    output << std::fixed << std::setprecision(2);
    for (const auto& comparison : report.engine_comparisons) {
        output << "comparison mode=" << comparison.mode << " input=" << comparison.input
               << " small_vs_empty_p50_ns=" << comparison.p50_delta_ns
               << " small_vs_empty_p95_ns=" << comparison.p95_delta_ns
               << " small_vs_empty_p99_ns=" << comparison.p99_delta_ns
               << " small_vs_empty_p50_pct=" << comparison.p50_delta_percent
               << " small_vs_empty_p95_pct=" << comparison.p95_delta_percent
               << " small_vs_empty_p99_pct=" << comparison.p99_delta_percent
               << " results_equal=" << (comparison.results_equal ? "true" : "false") << '\n';
    }
}

} // namespace cxxime::benchmark
