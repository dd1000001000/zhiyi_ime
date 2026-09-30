// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/composition_learning.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#include <cxxime/composition_state.h>
#include <cxxime/logging.h>
#include <cxxime/ordinary_candidate.h>
#include <cxxime/user_dict_validation.h>
#include <cxxime/user_data_merge.h>

#include "user_data_file.h"

namespace cxxime {
namespace {

constexpr int kCompositionLearningBaseScore = 220000000;

struct LearningRecord {
    CompositionLearningEvent event;
    std::uint32_t selection_count = 0;
    std::uint64_t sequence = 0;
    std::vector<std::string> extension_fields;
};

using RecordMap = std::unordered_map<std::string, LearningRecord>;

struct PublishedSnapshot {
    std::unordered_map<std::string, std::vector<LearningRecord>> by_code;
    std::uint64_t sequence = 0;
};

std::string record_key(const CompositionLearningEvent& event) {
    std::string key;
    key.reserve(event.code.size() + event.text.size() + 1);
    key.append(event.code);
    key.push_back('\x1f');
    key.append(event.text);
    return key;
}

bool valid_event(const CompositionLearningEvent& event) {
    return is_valid_user_dict_text(event.text) && is_valid_user_dict_code(event.code) &&
           !event.syllables.empty() && is_valid_user_dict_syllables(event.syllables);
}

bool parse_unsigned(const std::string& value, std::uint64_t* parsed) {
    if (!parsed || value.empty() || value.front() == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long result = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *parsed = static_cast<std::uint64_t>(result);
    return true;
}

std::vector<std::string> split_tsv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string::npos) {
            return fields;
        }
        start = separator + 1;
    }
}

bool record_less_valuable(const LearningRecord& left, const LearningRecord& right) {
    if (left.selection_count != right.selection_count) {
        return left.selection_count < right.selection_count;
    }
    if (left.sequence != right.sequence) {
        return left.sequence < right.sequence;
    }
    return record_key(left.event) < record_key(right.event);
}

void enforce_record_limit(RecordMap& records) {
    while (records.size() > CompositionLearningService::kMaxRecordCount) {
        auto victim = records.begin();
        for (auto current = std::next(records.begin()); current != records.end(); ++current) {
            if (record_less_valuable(current->second, victim->second)) {
                victim = current;
            }
        }
        records.erase(victim);
    }
}

std::uint64_t serialized_record_size(const LearningRecord& record) {
    std::uint64_t size =
        record.event.text.size() + record.event.code.size() + record.event.syllables.size() +
        std::to_string(record.selection_count).size() + std::to_string(record.sequence).size() + 5;
    for (const auto& field : record.extension_fields) {
        size += field.size() + 1;
    }
    return size;
}

void enforce_file_size_limit(RecordMap& records) {
    std::uint64_t size = 0;
    for (const auto& item : records) {
        size += serialized_record_size(item.second);
    }
    while (size > CompositionLearningService::kMaxFileSize && !records.empty()) {
        auto victim = records.begin();
        for (auto current = std::next(records.begin()); current != records.end(); ++current) {
            if (record_less_valuable(current->second, victim->second)) {
                victim = current;
            }
        }
        size -= serialized_record_size(victim->second);
        records.erase(victim);
        CXXIME_LOG(L"%s", L"composition_learning event=evict_file_size result=1");
    }
}

std::string serialize_records(const RecordMap& records) {
    std::vector<const LearningRecord*> sorted;
    sorted.reserve(records.size());
    for (const auto& item : records) {
        sorted.push_back(&item.second);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return record_key(left->event) < record_key(right->event);
    });

    std::ostringstream output;
    for (const LearningRecord* record : sorted) {
        output << record->event.text << '\t' << record->event.code << '\t'
               << record->event.syllables << '\t' << record->selection_count << '\t'
               << record->sequence;
        for (const auto& field : record->extension_fields) {
            output << '\t' << field;
        }
        output << '\n';
    }
    return output.str();
}

RecordMap merge_records(const RecordMap& persisted, const RecordMap& pending) {
    RecordMap merged = persisted;
    for (const auto& item : pending) {
        const LearningRecord& update = item.second;
        auto found = merged.find(item.first);
        if (found == merged.end()) {
            merged.emplace(item.first, update);
            continue;
        }
        LearningRecord& record = found->second;
        const std::uint64_t count =
            static_cast<std::uint64_t>(record.selection_count) + update.selection_count;
        record.selection_count =
            static_cast<std::uint32_t>((std::min)(count, static_cast<std::uint64_t>(UINT_MAX)));
        record.sequence = (std::max)(record.sequence, update.sequence);
        record.event = update.event;
    }
    enforce_record_limit(merged);
    enforce_file_size_limit(merged);
    return merged;
}

std::shared_ptr<const PublishedSnapshot> make_snapshot(const RecordMap& records) {
    auto snapshot = std::make_shared<PublishedSnapshot>();
    for (const auto& item : records) {
        snapshot->sequence = (std::max)(snapshot->sequence, item.second.sequence);
        snapshot->by_code[item.second.event.code].push_back(item.second);
    }
    for (auto& item : snapshot->by_code) {
        std::sort(item.second.begin(), item.second.end(), [](const auto& left, const auto& right) {
            return record_less_valuable(right, left);
        });
    }
    return snapshot;
}

Candidate candidate_from_record(const LearningRecord& record, std::uint64_t current_sequence) {
    const std::uint64_t delta =
        current_sequence >= record.sequence ? current_sequence - record.sequence : 0;
    const int recency = delta <= 1000 ? static_cast<int>(1000 - delta) : 0;
    Candidate candidate;
    candidate.text = record.event.text;
    candidate.code = record.event.code;
    candidate.syllables = record.event.syllables;
    candidate.frequency = kCompositionLearningBaseScore +
                          (std::min)(static_cast<int>(record.selection_count), 50000) + recency;
    candidate.source_frequency = candidate.frequency;
    candidate.source = CandidateSource::kPinyin;
    candidate.origin = CandidateOrigin::kComposed;
    return candidate;
}

const CandidateCanonicalVariant* primary_variant(const ConvertedSegment& segment) {
    return segment.primary_variant < segment.variants.size()
               ? &segment.variants[segment.primary_variant]
               : nullptr;
}

const CandidateCanonicalVariant* primary_variant(const TextSelectionAction& action) {
    return action.primary_variant < action.variants.size()
               ? &action.variants[action.primary_variant]
               : nullptr;
}

void append_preference(CommitLearningPlan& plan, const std::string& text,
                       const std::string& typed_code, const CandidateCanonicalVariant* variant) {
    if (!variant || variant->learning_target == LearningTarget::kNone ||
        variant->provenance.origin == CandidateOrigin::kComposed) {
        return;
    }
    CandidatePreferenceLearningEvent event;
    event.target = variant->learning_target;
    event.typed_code = variant->input_code.empty() ? typed_code : variant->input_code;
    event.candidate.text = text;
    event.candidate.code = variant->code;
    event.candidate.syllables = variant->syllables;
    event.candidate.frequency = variant->frequency;
    event.candidate.source_frequency = variant->source_frequency;
    event.candidate.source = variant->provenance.source;
    event.candidate.origin = variant->provenance.origin;
    plan.candidate_preferences.push_back(std::move(event));
}

bool append_composition_part(CompositionLearningEvent& event, const std::string& text,
                             const std::string& raw_input,
                             const CandidateCanonicalVariant* variant) {
    if (!variant || variant->learning_target != LearningTarget::kPinyin ||
        variant->provenance.source != CandidateSource::kPinyin || variant->syllables.empty()) {
        return false;
    }
    event.text += text;
    event.code += variant->input_code.empty() ? raw_input : variant->input_code;
    if (!event.syllables.empty()) {
        event.syllables.push_back(':');
    }
    event.syllables += variant->syllables;
    return true;
}

CommitLearningPlan make_learning_plan(const CompositionState& state,
                                      const TextSelectionAction* final_action,
                                      bool include_composition) {
    CommitLearningPlan plan;
    CompositionLearningEvent composition;
    const CandidateCanonicalVariant* final_variant =
        final_action ? primary_variant(*final_action) : nullptr;
    bool composition_is_valid =
        include_composition && state.active().scheme == CompositionScheme::kPinyin &&
        (!state.converted_segments().empty() ||
         (final_variant && final_variant->provenance.origin == CandidateOrigin::kComposed));
    for (const ConvertedSegment& segment : state.converted_segments()) {
        const CandidateCanonicalVariant* variant = primary_variant(segment);
        append_preference(plan, segment.text, segment.raw_input, variant);
        composition_is_valid =
            append_composition_part(composition, segment.text, segment.raw_input, variant) &&
            composition_is_valid;
    }
    if (final_action) {
        const CandidateCanonicalVariant* variant = primary_variant(*final_action);
        append_preference(plan, final_action->text, state.active().input, variant);
        composition_is_valid = append_composition_part(composition, final_action->text,
                                                       state.active().input, variant) &&
            composition_is_valid;
    }
    if (composition_is_valid && valid_event(composition)) {
        plan.composition = std::move(composition);
    }
    return plan;
}

} // namespace

struct CompositionLearningService::Impl {
    explicit Impl(WriteCallback callback)
        : write_callback(callback ? std::move(callback) : write_user_data_file_atomically)
        , published(make_snapshot({})) {}

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            condition.wait(lock, [this]() { return stopping || !pending.empty(); });
            if (pending.empty()) {
                break;
            }
            if (!stopping) {
                condition.wait_for(lock, std::chrono::milliseconds(50),
                                   [this]() { return stopping; });
            }

            const RecordMap batch = pending;
            const std::uint64_t batch_generation = enqueued_generation;
            RecordMap merged = merge_records(persisted, batch);
            const std::string contents = serialize_records(merged);
            const std::string output_path = path;
            lock.unlock();
            bool saved = false;
            try {
                saved = contents.size() <= CompositionLearningService::kMaxFileSize &&
                        write_callback(output_path, contents);
            } catch (...) {
                saved = false;
            }
            lock.lock();
            ++save_attempt;
            last_save_succeeded = saved;

            if (saved) {
                persisted = std::move(merged);
                persisted_generation = batch_generation;
                for (const auto& item : batch) {
                    auto current = pending.find(item.first);
                    if (current == pending.end()) {
                        continue;
                    }
                    if (current->second.selection_count <= item.second.selection_count) {
                        pending.erase(current);
                    } else {
                        current->second.selection_count -= item.second.selection_count;
                    }
                }
                published = make_snapshot(persisted);
                version.fetch_add(1, std::memory_order_acq_rel);
            } else {
                CXXIME_LOG(L"%s", L"composition_learning event=save result=0");
            }
            condition.notify_all();

            if (stopping) {
                if (!saved || pending.empty()) {
                    break;
                }
                continue;
            }
            if (!saved) {
                condition.wait_for(lock, std::chrono::seconds(1), [this]() { return stopping; });
            }
        }
    }

    WriteCallback write_callback;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    std::string path;
    RecordMap persisted;
    RecordMap pending;
    std::shared_ptr<const PublishedSnapshot> published;
    std::atomic<std::uint64_t> version{0};
    std::uint64_t sequence = 0;
    std::uint64_t enqueued_generation = 0;
    std::uint64_t persisted_generation = 0;
    std::uint64_t save_attempt = 0;
    bool loaded = false;
    bool accepting = false;
    bool stopping = false;
    bool last_save_succeeded = true;
};

CommitLearningPlan make_candidate_learning_plan(const CompositionState& state,
                                                const TextSelectionAction& final_action) {
    return make_learning_plan(state, &final_action, true);
}

CommitLearningPlan make_raw_learning_plan(const CompositionState& state) {
    return make_learning_plan(state, nullptr, false);
}

CommitLearningPlan make_partial_raw_learning_plan(const CompositionState& state,
                                                  const TextSelectionAction& partial_action) {
    return make_learning_plan(state, &partial_action, false);
}

CompositionLearningService::CompositionLearningService(WriteCallback write_callback)
    : impl_(std::make_unique<Impl>(std::move(write_callback))) {}

CompositionLearningService::~CompositionLearningService() { freeze_and_stop(); }

bool CompositionLearningService::load(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string contents;
    if (!read_user_data_file(path, kMaxFileSize, &contents)) {
        CXXIME_LOG(L"%s", L"composition_learning event=load result=degraded_empty");
        contents.clear();
    }

    RecordMap records;
    std::uint64_t sequence = 0;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> fields = split_tsv_line(line);
        std::uint64_t selection_count = 0;
        std::uint64_t entry_sequence = 0;
        CompositionLearningEvent event;
        // The first five columns are the 0.5 baseline. Future formats may only append columns;
        // preserve unknown tail fields when this version rewrites the record.
        if (fields.size() < 5 || !parse_unsigned(fields[3], &selection_count) ||
            selection_count == 0 || selection_count > UINT_MAX ||
            !parse_unsigned(fields[4], &entry_sequence) || entry_sequence == 0) {
            continue;
        }
        event.text = fields[0];
        event.code = fields[1];
        event.syllables = fields[2];
        if (!valid_event(event) || !is_ordinary_candidate_text(event.text)) {
            continue;
        }
        LearningRecord record;
        record.event = std::move(event);
        record.selection_count = static_cast<std::uint32_t>(selection_count);
        record.sequence = entry_sequence;
        record.extension_fields.assign(fields.begin() + 5, fields.end());
        sequence = (std::max)(sequence, entry_sequence);
        records[record_key(record.event)] = std::move(record);
    }
    enforce_record_limit(records);
    enforce_file_size_limit(records);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->worker.joinable()) {
        return false;
    }
    impl_->path = path;
    impl_->persisted = std::move(records);
    impl_->pending.clear();
    impl_->published = make_snapshot(impl_->persisted);
    impl_->sequence = sequence;
    impl_->enqueued_generation = 0;
    impl_->persisted_generation = 0;
    impl_->save_attempt = 0;
    impl_->loaded = true;
    impl_->accepting = false;
    impl_->stopping = false;
    impl_->last_save_succeeded = true;
    impl_->version.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool CompositionLearningService::validate_contents(const std::string& contents) {
    if (contents.size() > kMaxFileSize) {
        return false;
    }
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_tsv_line(line);
        std::uint64_t selection_count = 0;
        std::uint64_t sequence = 0;
        if (fields.size() < 5 || !parse_unsigned(fields[3], &selection_count) ||
            selection_count == 0 || selection_count > UINT_MAX ||
            !parse_unsigned(fields[4], &sequence) || sequence == 0 ||
            !valid_event({fields[0], fields[1], fields[2]})) {
            return false;
        }
    }
    return true;
}

bool CompositionLearningService::start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->loaded || impl_->worker.joinable()) {
        return false;
    }
    impl_->accepting = true;
    impl_->stopping = false;
    try {
        impl_->worker = std::thread([this]() { impl_->run(); });
    } catch (...) {
        impl_->accepting = false;
        return false;
    }
    return true;
}

bool CompositionLearningService::enqueue(const CompositionLearningEvent& event) {
    if (!valid_event(event)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->accepting || impl_->stopping) {
        return false;
    }
    const std::string key = record_key(event);
    if (impl_->enqueued_generation !=
        (std::numeric_limits<std::uint64_t>::max)()) {
        ++impl_->enqueued_generation;
    }
    auto found = impl_->pending.find(key);
    if (found == impl_->pending.end()) {
        LearningRecord record;
        record.event = event;
        record.selection_count = 1;
        if (impl_->sequence != (std::numeric_limits<std::uint64_t>::max)()) {
            ++impl_->sequence;
        }
        record.sequence = impl_->sequence;
        impl_->pending.emplace(key, std::move(record));
        if (impl_->pending.size() > kMaxRecordCount) {
            auto victim = impl_->pending.begin();
            for (auto current = std::next(impl_->pending.begin()); current != impl_->pending.end();
                 ++current) {
                if (record_less_valuable(current->second, victim->second)) {
                    victim = current;
                }
            }
            impl_->pending.erase(victim);
            CXXIME_LOG(L"%s", L"composition_learning event=evict_pending result=1");
        }
    } else {
        if (found->second.selection_count < UINT_MAX) {
            ++found->second.selection_count;
        }
        if (impl_->sequence != (std::numeric_limits<std::uint64_t>::max)()) {
            ++impl_->sequence;
        }
        found->second.sequence = impl_->sequence;
        found->second.event = event;
    }
    impl_->condition.notify_one();
    return true;
}

bool CompositionLearningService::flush() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const std::uint64_t target_generation = impl_->enqueued_generation;
    if (impl_->persisted_generation >= target_generation) {
        return true;
    }
    if (!impl_->worker.joinable()) {
        return false;
    }
    const std::uint64_t initial_attempt = impl_->save_attempt;
    impl_->condition.notify_one();
    impl_->condition.wait(lock, [&]() {
        return impl_->persisted_generation >= target_generation ||
               (impl_->save_attempt > initial_attempt && !impl_->last_save_succeeded);
    });
    return impl_->persisted_generation >= target_generation;
}

bool CompositionLearningService::freeze_and_stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->accepting = false;
        if (!impl_->worker.joinable()) {
            return impl_->pending.empty();
        }
        impl_->stopping = true;
    }
    impl_->condition.notify_one();
    impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = false;
    return impl_->pending.empty();
}

bool CompositionLearningService::merge_contents_and_save(const std::string& imported,
                                                         UserDataMergeResult* result) {
    if (!result || !freeze_and_stop()) {
        start();
        return false;
    }
    std::string current;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        current = serialize_records(impl_->persisted);
        path = impl_->path;
    }
    UserDataMergeResult merged;
    const bool merged_ok =
        !path.empty() && merge_composition_learning_contents(current, imported, &merged);
    bool saved = false;
    if (merged_ok) {
        try {
            saved = impl_->write_callback(path, merged.contents);
        } catch (...) {
            saved = false;
        }
    }
    const bool loaded = saved && load(path);
    const bool started = start();
    if (!loaded || !started) {
        return false;
    }
    *result = std::move(merged);
    return true;
}

std::vector<Candidate> CompositionLearningService::lookup_candidates(const std::string& code,
                                                                     std::size_t limit) const {
    std::shared_ptr<const PublishedSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        snapshot = impl_->published;
    }
    std::vector<Candidate> candidates;
    if (!snapshot || code.empty() || limit == 0) {
        return candidates;
    }
    const auto found = snapshot->by_code.find(code);
    if (found == snapshot->by_code.end()) {
        return candidates;
    }
    const std::size_t count = (std::min)(limit, found->second.size());
    candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        candidates.push_back(candidate_from_record(found->second[index], snapshot->sequence));
    }
    return candidates;
}

std::uint64_t CompositionLearningService::version() const {
    return impl_->version.load(std::memory_order_acquire);
}

std::size_t CompositionLearningService::entry_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->persisted.size();
}

std::size_t CompositionLearningService::pending_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pending.size();
}

} // namespace cxxime
