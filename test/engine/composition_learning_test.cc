// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <cxxime/composition_learning.h>
#include <cxxime/composition_state.h>
#include <cxxime/dict.h>
#include <cxxime/translator.h>

#include "support/testutil.h"
#include "support/topn_test_data.h"

namespace {

std::string temp_path(const char* suffix) {
    char directory[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, directory);
    static std::atomic<unsigned long> sequence{0};
    const std::string path = std::string(directory) + "zhiyi-composition-learning-" +
                             std::to_string(GetCurrentProcessId()) + "-" +
                             std::to_string(sequence.fetch_add(1)) + suffix;
    DeleteFileA(path.c_str());
    return path;
}

bool write_file(const std::string& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(2500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

cxxime::CandidateCanonicalVariant
pinyin_variant(const std::string& code, const std::string& syllables,
               cxxime::CandidateOrigin origin = cxxime::CandidateOrigin::kSystem) {
    cxxime::CandidateCanonicalVariant variant;
    variant.provenance.source = cxxime::CandidateSource::kPinyin;
    variant.provenance.origin = origin;
    variant.code = code;
    variant.syllables = syllables;
    variant.frequency = 1000;
    variant.source_frequency = 1000;
    variant.learning_target = cxxime::LearningTarget::kPinyin;
    return variant;
}

cxxime::TextSelectionAction action(const std::string& text, std::size_t consumed,
                                   cxxime::CandidateCanonicalVariant variant) {
    cxxime::TextSelectionAction result;
    result.text = text;
    result.consumed_input_bytes = consumed;
    result.variants.push_back(std::move(variant));
    return result;
}

cxxime::CompositionLearningEvent learning_event(const std::string& text, const std::string& code) {
    cxxime::CompositionLearningEvent event;
    event.text = text;
    event.code = code;
    event.syllables = "hua:rui:ji:shu";
    return event;
}

} // namespace

TEST(CompositionLearningPlan, final_candidate_records_segments_and_whole_pinyin) {
    cxxime::CompositionState state;
    ASSERT_TRUE(state.set_scheme(cxxime::CompositionScheme::kPinyin));
    ASSERT_TRUE(state.set_active_input("huaruijishu", 11));
    ASSERT_TRUE(state.confirm_prefix(action("prefix", 6, pinyin_variant("huarui", "hua:rui"))));

    const auto final = action("suffix", 5, pinyin_variant("jishu", "ji:shu"));
    const cxxime::CommitLearningPlan plan = cxxime::make_candidate_learning_plan(state, final);
    ASSERT_EQ(plan.candidate_preferences.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(plan.candidate_preferences[0].typed_code, "huarui");
    ASSERT_EQ(plan.candidate_preferences[1].typed_code, "jishu");
    ASSERT_TRUE(plan.composition.has_value());
    ASSERT_EQ(plan.composition->text, "prefixsuffix");
    ASSERT_EQ(plan.composition->code, "huaruijishu");
    ASSERT_EQ(plan.composition->syllables, "hua:rui:ji:shu");
}

TEST(CompositionLearningPlan, raw_suffix_and_mixed_composition_do_not_learn_whole_phrase) {
    cxxime::CompositionState pinyin;
    ASSERT_TRUE(pinyin.set_scheme(cxxime::CompositionScheme::kPinyin));
    ASSERT_TRUE(pinyin.set_active_input("huaruijishu", 11));
    ASSERT_TRUE(pinyin.confirm_prefix(action("prefix", 6, pinyin_variant("huarui", "hua:rui"))));
    const cxxime::CommitLearningPlan raw = cxxime::make_raw_learning_plan(pinyin);
    ASSERT_EQ(raw.candidate_preferences.size(), static_cast<std::size_t>(1));
    ASSERT_TRUE(!raw.composition.has_value());

    cxxime::CompositionState mixed;
    ASSERT_TRUE(mixed.set_scheme(cxxime::CompositionScheme::kMixed));
    ASSERT_TRUE(mixed.set_active_input("huaruijishu", 11));
    ASSERT_TRUE(mixed.confirm_prefix(action("prefix", 6, pinyin_variant("huarui", "hua:rui"))));
    const auto final = action("suffix", 5, pinyin_variant("jishu", "ji:shu"));
    const cxxime::CommitLearningPlan mixed_plan =
        cxxime::make_candidate_learning_plan(mixed, final);
    ASSERT_EQ(mixed_plan.candidate_preferences.size(), static_cast<std::size_t>(2));
    ASSERT_TRUE(!mixed_plan.composition.has_value());
}

TEST(CompositionLearningService, persists_and_reloads_valid_records) {
    const std::string path = temp_path(".tsv");
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(path));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(service.flush());
    ASSERT_EQ(service.pending_count(), static_cast<std::size_t>(0));
    ASSERT_EQ(service.lookup_candidates("huaruijishu", 10).size(),
              static_cast<std::size_t>(1));
    ASSERT_TRUE(service.freeze_and_stop());

    cxxime::CompositionLearningService reloaded;
    ASSERT_TRUE(reloaded.load(path));
    const auto candidates = reloaded.lookup_candidates("huaruijishu", 10);
    ASSERT_EQ(candidates.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(candidates[0].text, "learned");
    ASSERT_EQ(candidates[0].origin, cxxime::CandidateOrigin::kComposed);
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, skips_malformed_records_without_losing_valid_data) {
    const std::string path = temp_path(".tsv");
    ASSERT_TRUE(write_file(path, "valid\thuaruijishu\thua:rui:ji:shu\t2\t3\n"
                                 "future\thuaruijishu\thua:rui:ji:shu\t1\t4\textra\n"
                                 "missing-fields\thuaruijishu\n"
                                 "bad-count\thuaruijishu\thua:rui:ji:shu\tbad\t4\n"));
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(path));
    ASSERT_EQ(service.entry_count(), static_cast<std::size_t>(2));
    ASSERT_EQ(service.lookup_candidates("huaruijishu", 10)[0].text, "valid");
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("future", "huaruijishu")));
    ASSERT_TRUE(service.freeze_and_stop());
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    ASSERT_TRUE(contents.find("\textra\n") != std::string::npos);
    input.close();
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, unreadable_or_oversized_file_degrades_to_empty_learning) {
    const std::string path = temp_path(".tsv");
    ASSERT_TRUE(
        write_file(path, std::string(cxxime::CompositionLearningService::kMaxFileSize + 1, 'x')));
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(path));
    ASSERT_EQ(service.entry_count(), static_cast<std::size_t>(0));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(service.freeze_and_stop());
    ASSERT_EQ(service.lookup_candidates("huaruijishu", 10).size(), static_cast<std::size_t>(1));
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, evicts_low_value_record_when_future_tail_exceeds_limit) {
    const std::string path = temp_path(".tsv");
    const std::size_t baseline_size = std::string("old\told\told\t1\t1\t\n").size();
    const std::string extension(cxxime::CompositionLearningService::kMaxFileSize - baseline_size,
                                'x');
    ASSERT_TRUE(write_file(path, "old\told\told\t1\t1\t" + extension + "\n"));

    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(path));
    ASSERT_EQ(service.entry_count(), static_cast<std::size_t>(1));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(service.freeze_and_stop());
    ASSERT_EQ(service.lookup_candidates("old", 10).size(), static_cast<std::size_t>(0));
    ASSERT_EQ(service.lookup_candidates("huaruijishu", 10).size(), static_cast<std::size_t>(1));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    ASSERT_LE(static_cast<std::uint64_t>(input.tellg()),
              cxxime::CompositionLearningService::kMaxFileSize);
    input.close();
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, slow_save_does_not_block_new_input_events) {
    const std::string path = temp_path(".tsv");
    std::mutex mutex;
    std::condition_variable condition;
    bool writer_entered = false;
    bool release_writer = false;
    cxxime::CompositionLearningService service(
        [&](const std::string& output_path, const std::string& contents) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                writer_entered = true;
                condition.notify_all();
                condition.wait(lock, [&]() { return release_writer; });
            }
            return write_file(output_path, contents);
        });
    ASSERT_TRUE(service.load(path));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("first", "huaruijishu")));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(
            condition.wait_for(lock, std::chrono::seconds(1), [&]() { return writer_entered; }));
    }
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(service.enqueue(learning_event("second", "huaruijishu")));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
    ASSERT_LT(elapsed_ms, 100);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_writer = true;
    }
    condition.notify_all();
    ASSERT_TRUE(service.freeze_and_stop());
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, failed_save_retries_without_losing_the_event) {
    const std::string path = temp_path(".tsv");
    std::atomic<int> attempts{0};
    cxxime::CompositionLearningService service(
        [&](const std::string& output_path, const std::string& contents) {
            if (attempts.fetch_add(1) == 0) {
                return false;
            }
            return write_file(output_path, contents);
        });
    ASSERT_TRUE(service.load(path));
    const std::uint64_t loaded_version = service.version();
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(wait_until([&]() { return service.version() > loaded_version; }));
    ASSERT_TRUE(service.freeze_and_stop());
    ASSERT_GE(attempts.load(), 2);

    cxxime::CompositionLearningService reloaded;
    ASSERT_TRUE(reloaded.load(path));
    ASSERT_EQ(reloaded.lookup_candidates("huaruijishu", 10).size(), static_cast<std::size_t>(1));
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, concurrent_events_merge_into_one_persisted_record) {
    const std::string path = temp_path(".tsv");
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(path));
    ASSERT_TRUE(service.start());
    std::atomic<bool> all_enqueued{true};
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        threads.emplace_back([&]() {
            for (int event_index = 0; event_index < 100; ++event_index) {
                if (!service.enqueue(learning_event("learned", "huaruijishu"))) {
                    all_enqueued.store(false);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    ASSERT_TRUE(all_enqueued.load());
    ASSERT_TRUE(service.freeze_and_stop());

    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    ASSERT_TRUE(contents.find("\t400\t") != std::string::npos);
    input.close();
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, permanent_failure_stops_once_and_preserves_dirty_state) {
    const std::string path = temp_path(".tsv");
    std::atomic<int> attempts{0};
    cxxime::CompositionLearningService service([&](const std::string&, const std::string&) {
        attempts.fetch_add(1);
        return false;
    });
    ASSERT_TRUE(service.load(path));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(!service.freeze_and_stop());
    ASSERT_GE(attempts.load(), 1);
    ASSERT_LE(attempts.load(), 2);
    ASSERT_EQ(service.pending_count(), static_cast<std::size_t>(1));
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningService, failed_writer_keeps_pending_records_bounded) {
    const std::string path = temp_path(".tsv");
    std::mutex mutex;
    std::condition_variable condition;
    bool writer_entered = false;
    bool release_writer = false;
    cxxime::CompositionLearningService service([&](const std::string&, const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        writer_entered = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return release_writer; });
        return false;
    });
    ASSERT_TRUE(service.load(path));
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(service.enqueue(learning_event("first", "a")));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(
            condition.wait_for(lock, std::chrono::seconds(1), [&]() { return writer_entered; }));
    }
    for (std::size_t index = 0; index < cxxime::CompositionLearningService::kMaxRecordCount + 100;
         ++index) {
        cxxime::CompositionLearningEvent event;
        event.text = "entry-" + std::to_string(index);
        event.code = "b" + std::string(index / 26, 'a') + static_cast<char>('a' + index % 26);
        event.syllables = event.code;
        ASSERT_TRUE(service.enqueue(event));
    }
    ASSERT_LE(service.pending_count(), cxxime::CompositionLearningService::kMaxRecordCount);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_writer = true;
    }
    condition.notify_all();
    ASSERT_TRUE(!service.freeze_and_stop());
    DeleteFileA(path.c_str());
}

TEST(CompositionLearningTranslator, successful_save_invalidates_topn_query_cache) {
    const std::string dict_path = temp_path(".dict.bin");
    std::string topn_path = dict_path;
    topn_path.replace(topn_path.size() - std::string(".dict.bin").size(),
                      std::string(".dict.bin").size(), ".topn.bin");
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, {{"hua:rui:ji:shu", "baseline", 1000}}));
    cxxime::Candidate baseline;
    baseline.text = "baseline";
    baseline.code = "huaruijishu";
    baseline.syllables = "hua:rui:ji:shu";
    baseline.frequency = 1000;
    ASSERT_TRUE(cxxime::test::create_test_topn(
        topn_path, dict_path, {{"huaruijishu", {baseline}}}));

    cxxime::Dict dict{cxxime::UserDictKind::PINYIN};
    ASSERT_TRUE(dict.open(dict_path));
    const std::string disabled_path = temp_path(".tsv");
    ASSERT_TRUE(dict.load_disabled_system_entries(disabled_path));
    const std::string learning_path = temp_path(".tsv");
    cxxime::CompositionLearningService service;
    ASSERT_TRUE(service.load(learning_path));
    ASSERT_TRUE(service.start());

    cxxime::PinyinTranslator translator;
    translator.set_dict(&dict);
    translator.set_short_cache(&dict.short_cache());
    translator.set_candidate_learning_enabled(true);
    translator.set_composition_learning_service(&service);
    ASSERT_EQ(translator.translate_page("huaruijishu", 0, 5).candidates[0].text, "baseline");

    ASSERT_TRUE(service.enqueue(learning_event("learned", "huaruijishu")));
    ASSERT_TRUE(service.freeze_and_stop());
    const auto learned = translator.translate_page("huaruijishu", 0, 5);
    ASSERT_EQ(learned.candidates[0].text, "learned");

    cxxime::TranslationRequest request;
    request.input = "huaruijishu";
    request.page_size = 10;
    request.policy.allow_partial_selection = true;
    const cxxime::TranslationResult partial = translator.translate(request);
    for (const auto& entry : partial.entries) {
        if (entry.candidate.origin != cxxime::CandidateOrigin::kComposed) {
            continue;
        }
        const auto* selection = std::get_if<cxxime::TextSelectionAction>(&entry.selection);
        ASSERT_TRUE(selection != nullptr);
        ASSERT_EQ(selection->consumed_input_bytes, request.input.size());
    }

    ASSERT_TRUE(dict.disable_system_entry("learned"));
    const auto hidden = translator.translate_page("huaruijishu", 0, 5);
    ASSERT_TRUE(std::none_of(hidden.candidates.begin(), hidden.candidates.end(),
                             [](const auto& candidate) { return candidate.text == "learned"; }));

    dict.close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(topn_path.c_str());
    DeleteFileA(learning_path.c_str());
    DeleteFileA(disabled_path.c_str());
}

RUN_ALL_TESTS()
