// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/pinyin_composer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

#include <cxxime/dict.h>
#include <cxxime/query_budget.h>

namespace cxxime {

namespace {

struct SpanMemoEntry {
    std::vector<uint32_t> ids;
    std::vector<Candidate> candidates;
    bool has_prefix = false;
};

struct SpanEdge {
    uint16_t end = 0;
    int64_t score = 0;
    Candidate candidate;
};

struct CompositionNode {
    uint16_t end = 0;
    uint16_t segment_count = 0;
    int32_t weakest_frequency = 0;
    int64_t aggregate_score = 0;
    uint32_t parent = UINT32_MAX;
    uint32_t edge_index = UINT32_MAX;
    uint32_t sequence = 0;
};

struct ComposedResult {
    Candidate candidate;
    CompositionPathKind kind = CompositionPathKind::kNormal;
    uint16_t path_rank = 0;
    uint16_t segment_count = 0;
    int32_t weakest_frequency = 0;
    int64_t aggregate_score = 0;
    uint32_t sequence = 0;
};

constexpr int64_t kAmbiguousJointPenalty = -2303;       // ln(0.1)

int64_t word_score(int frequency) { return composed_word_score(frequency); }

bool node_better(const CompositionNode& left, const CompositionNode& right) {
    if (left.aggregate_score != right.aggregate_score) {
        return left.aggregate_score > right.aggregate_score;
    }
    if (left.segment_count != right.segment_count) {
        return left.segment_count < right.segment_count;
    }
    return left.sequence < right.sequence;
}

bool offer_node(std::vector<CompositionNode>& nodes, std::vector<uint32_t>& beam,
                CompositionNode node, size_t beam_width, size_t node_capacity) {
    if (beam_width == 0) {
        return true;
    }
    if (beam.size() < beam_width) {
        if (nodes.size() >= node_capacity) {
            return false;
        }
        beam.push_back(static_cast<uint32_t>(nodes.size()));
        nodes.push_back(node);
        return true;
    }

    size_t worst = 0;
    for (size_t i = 1; i < beam.size(); ++i) {
        if (node_better(nodes[beam[worst]], nodes[beam[i]])) {
            worst = i;
        }
    }
    if (node_better(node, nodes[beam[worst]])) {
        nodes[beam[worst]] = node;
    }
    return true;
}

bool result_better(const ComposedResult& left, const ComposedResult& right) {
    if (left.kind != right.kind) {
        return left.kind == CompositionPathKind::kNormal;
    }
    if (left.aggregate_score != right.aggregate_score) {
        return left.aggregate_score > right.aggregate_score;
    }
    if (left.segment_count != right.segment_count) {
        return left.segment_count < right.segment_count;
    }
    if (left.path_rank != right.path_rank) {
        return left.path_rank < right.path_rank;
    }
    return left.sequence < right.sequence;
}

SpanMemoEntry* find_memo(std::vector<SpanMemoEntry>& memo, const std::vector<uint32_t>& ids,
                         size_t start, size_t end) {
    const size_t count = end - start;
    for (auto& entry : memo) {
        if (entry.ids.size() == count &&
            std::equal(entry.ids.begin(), entry.ids.end(), ids.begin() + start)) {
            return &entry;
        }
    }
    return nullptr;
}

std::string join_syllables(const std::vector<std::string>& syllables) {
    size_t size = syllables.empty() ? 0 : syllables.size() - 1;
    for (const auto& syllable : syllables) {
        size += syllable.size();
    }

    std::string joined;
    joined.reserve(size);
    for (size_t i = 0; i < syllables.size(); ++i) {
        if (i > 0) {
            joined.push_back(':');
        }
        joined.append(syllables[i]);
    }
    return joined;
}

Candidate rebuild_candidate(const std::string& input, const CompositionPath& path,
                            const std::vector<CompositionNode>& nodes,
                            const std::vector<SpanEdge>& edges, uint32_t node_index) {
    std::vector<uint32_t> chain;
    for (uint32_t current = node_index; nodes[current].parent != UINT32_MAX;
         current = nodes[current].parent) {
        chain.push_back(nodes[current].edge_index);
    }

    size_t text_size = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        text_size += edges[*it].candidate.text.size();
    }

    Candidate candidate;
    candidate.text.reserve(text_size);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        candidate.text.append(edges[*it].candidate.text);
    }
    candidate.code = path.input_code ? *path.input_code : input;
    if (path.input_code) {
        candidate.input_code = *path.input_code;
    }
    candidate.syllables = join_syllables(*path.syllables);
    candidate.origin = CandidateOrigin::kComposed;
    candidate.source = CandidateSource::kPinyin;
    candidate.source_frequency = nodes[node_index].weakest_frequency;
    candidate.frequency = sentence_frequency(nodes[node_index].aggregate_score);
    candidate.composed_score = nodes[node_index].aggregate_score;
    return candidate;
}

void offer_result(std::vector<ComposedResult>& results, ComposedResult result, size_t capacity) {
    for (auto& existing : results) {
        if (existing.candidate.text == result.candidate.text) {
            if (result_better(result, existing)) {
                existing = std::move(result);
            }
            return;
        }
    }
    if (results.size() < capacity) {
        results.push_back(std::move(result));
        return;
    }

    size_t worst = 0;
    for (size_t i = 1; i < results.size(); ++i) {
        if (result_better(results[worst], results[i])) {
            worst = i;
        }
    }
    if (result_better(result, results[worst])) {
        results[worst] = std::move(result);
    }
}

std::vector<std::string> utf8_characters(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size();) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        out.push_back(text.substr(i, n));
        i += n;
    }
    return out;
}

struct SequentialHypothesis {
    std::string text;
    std::vector<std::pair<uint16_t, uint32_t>> edges;
    double ln_p = 0.0;
    int64_t word_score = 0;
    int64_t score = 0;
    uint16_t word_start = 0;             // the current word's start slot
    uint32_t word_index = UINT32_MAX;    // in edges_by_start[word_start]; UINT32_MAX = at a boundary
    size_t offset = 0;                   // characters of the current word taken
    uint32_t sequence = 0;
};

} // namespace

std::vector<SequentialPath> sequential_compose(size_t slots,
                                               const std::vector<std::vector<SequentialEdge>>& edges_by_start,
                                               const std::vector<uint8_t>* ambiguous_joint,
                                               SpanScorer& scorer, size_t max_results,
                                               QueryDeadline& deadline, bool& deadline_hit) {
    deadline_hit = false;
    std::vector<SequentialPath> out;
    if (slots == 0 || max_results == 0) return out;
    // Every word's characters, split once.
    std::vector<std::vector<std::vector<std::string>>> chars(edges_by_start.size());
    for (size_t start = 0; start < edges_by_start.size(); ++start) {
        for (const SequentialEdge& edge : edges_by_start[start]) {
            chars[start].push_back(edge.text ? utf8_characters(*edge.text) : std::vector<std::string>{});
        }
    }
    const double weight = scorer.word_weight();
    const size_t width = (std::max)(size_t{1}, scorer.beam_width());
    std::vector<SequentialHypothesis> live(1);  // the empty prefix, at slot 0
    uint32_t sequence = 0;
    std::vector<std::string> prefixes;
    std::vector<SequentialHypothesis> next;
    std::unordered_set<std::string> seen;
    for (size_t slot = 0; slot < slots; ++slot) {
        if (deadline.enabled && deadline.expired()) {
            deadline_hit = true;
            return out;
        }
        prefixes.clear();
        for (const auto& h : live) prefixes.push_back(h.text);
        const auto model_start = QueryDeadline::Clock::now();
        const bool stepped = scorer.step(prefixes, slot);
        if (deadline.enabled) deadline.expires_at += QueryDeadline::Clock::now() - model_start;
        if (!stepped) return out;
        next.clear();
        for (size_t i = 0; i < live.size(); ++i) {
            const SequentialHypothesis& h = live[i];
            const auto extend = [&](uint16_t start, uint32_t index, size_t offset) {
                const std::vector<std::string>& word = chars[start][index];
                double ln_p = 0.0;
                if (!scorer.char_ln_p(i, word[offset], ln_p)) return;
                SequentialHypothesis n = h;
                n.text += word[offset];
                n.ln_p += ln_p;
                n.word_start = start;
                n.word_index = index;
                n.offset = offset + 1;
                if (offset == 0) {
                    n.edges.emplace_back(start, index);
                    n.word_score += edges_by_start[start][index].score;
                }
                if (n.offset == word.size()) n.word_index = UINT32_MAX;  // the word is complete
                n.score = static_cast<int64_t>(std::llround(n.ln_p * kComposedScoreScale +
                                                            weight * static_cast<double>(n.word_score)));
                n.sequence = ++sequence;
                next.push_back(std::move(n));
            };
            if (h.word_index != UINT32_MAX) {
                extend(h.word_start, h.word_index, h.offset);
                continue;
            }
            if (ambiguous_joint && slot < ambiguous_joint->size() && (*ambiguous_joint)[slot]) continue;
            if (slot >= edges_by_start.size()) continue;
            for (uint32_t e = 0; e < edges_by_start[slot].size(); ++e) {
                const SequentialEdge& edge = edges_by_start[slot][e];
                if (edge.end <= slot || edge.end > slots || chars[slot][e].size() != edge.end - slot) continue;
                extend(static_cast<uint16_t>(slot), e, 0);
            }
        }
        if (next.empty()) return out;
        std::sort(next.begin(), next.end(), [](const SequentialHypothesis& a, const SequentialHypothesis& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.edges.size() != b.edges.size()) return a.edges.size() < b.edges.size();
            return a.sequence < b.sequence;
        });
        // The same characters in the same word state: one path.
        live.clear();
        seen.clear();
        for (auto& n : next) {
            std::string key = n.text;
            key += '\x1f';
            if (n.word_index != UINT32_MAX) {
                key += std::to_string(n.word_start) + ':' + std::to_string(n.word_index) + ':' +
                       std::to_string(n.offset);
            }
            if (!seen.insert(key).second) continue;
            live.push_back(std::move(n));
            if (live.size() >= width) break;
        }
    }
    for (const auto& h : live) {
        if (h.word_index != UINT32_MAX) continue;
        SequentialPath path;
        path.edges = h.edges;
        path.ln_p = h.ln_p;
        path.word_score = h.word_score;
        path.score = h.score;
        out.push_back(std::move(path));
        if (out.size() >= max_results) break;
    }
    return out;
}

std::vector<Candidate>
PinyinComposer::compose(const std::string& input, const std::vector<CompositionPath>& paths,
                        size_t requested_candidates, const QueryDeadline& query_deadline,
                        const CompositionLimits& limits, CompositionStats& stats,
                        SpanScorer* scorer) const {
    stats = CompositionStats{};
    // The scorer's model calls (prepare) move the deadline back by the time they take: the
    // search keeps the budget a composition without the model has (a key's 30 ms would
    // otherwise go to the model on the CPU, and the beam stop before any sentence).
    QueryDeadline deadline = query_deadline;
    // Rank every path's sentences before choosing: the requested few would otherwise all be
    // homophones of the best path.
    const size_t result_capacity = static_cast<size_t>(limits.max_final_candidates);
    const size_t sentence_limit =
        (std::min)(requested_candidates, static_cast<size_t>(limits.max_sentences));
    if (sentence_limit == 0 || result_capacity == 0 || paths.empty()) {
        return {};
    }

    SpanLookupLimits lookup_limits;
    lookup_limits.max_range_queries = limits.max_range_queries;
    lookup_limits.max_entry_scans = limits.max_entry_scans;
    lookup_limits.max_results = limits.max_span_candidates;
    lookup_limits.max_candidates_per_range = limits.max_candidates_per_range;
    SpanLookupStats lookup_stats;
    std::vector<SpanMemoEntry> memo;
    memo.reserve(limits.max_range_queries);
    std::vector<ComposedResult> results;
    results.reserve(result_capacity);
    uint32_t result_sequence = 0;

    for (const auto& path : paths) {
        if (!path.ids || !path.syllables || path.ids->empty() ||
            path.ids->size() != path.syllables->size()) {
            continue;
        }
        if (deadline.enabled && deadline.expired()) {
            stats.deadline_exceeded = true;
            stats.truncated = true;
            break;
        }

        if (path.kind == CompositionPathKind::kNormal) {
            if (stats.normal_path_count >= limits.max_normal_paths) {
                continue;
            }
            ++stats.normal_path_count;
        } else {
            if (stats.repeated_short_path_count >= limits.max_repeated_short_paths) {
                continue;
            }
            ++stats.repeated_short_path_count;
        }

        const auto& ids = *path.ids;
        if (ids.size() > UINT16_MAX) {
            stats.truncated = true;
            continue;
        }
        if (scorer) {
            const auto model_start = QueryDeadline::Clock::now();
            const bool prepared = scorer->prepare(path, input);
            if (deadline.enabled) deadline.expires_at += QueryDeadline::Clock::now() - model_start;
            if (!prepared) continue;  // the model cannot read this path: not composed at all
        }

        const uint32_t span_capacity = limits.max_span_candidates > stats.span_candidate_count
                                        ? limits.max_span_candidates - stats.span_candidate_count
                                        : 0;
        if (span_capacity == 0) {
            stats.truncated = true;
            break;
        }
        // An ambiguous joint splits what also reads as one syllable (xi|a of "xia", xi|an of
        // "xian"). A word boundary may not fall there ("nikanxia" would also give 你看洗啊); a word
        // spanning it (西安) costs kAmbiguousJointPenalty, so the plain reading leads.
        const auto& syllables = *path.syllables;
        std::vector<uint8_t> ambiguous_joint(ids.size() + 1, 0);
        if (path.kind == CompositionPathKind::kNormal) {
            for (size_t joint = 1; joint < ids.size(); ++joint) {
                ambiguous_joint[joint] =
                    dict_.syllable_to_id(syllables[joint - 1] + syllables[joint]) != UINT32_MAX;
            }
        }

        std::vector<SpanEdge> edges;
        edges.reserve(span_capacity);
        std::vector<std::vector<uint32_t>> edges_by_start(ids.size());
        bool stop_queries = false;
        for (size_t start = 0; start < ids.size() && !stop_queries; ++start) {
            const size_t last_end = path.kind == CompositionPathKind::kRepeatedShortCode
                                        ? start + 1
                                        : ids.size();
            for (size_t end = start + 1; end <= last_end; ++end) {
                SpanMemoEntry* memo_entry = find_memo(memo, ids, start, end);
                if (!memo_entry) {
                    SpanMemoEntry entry;
                    entry.ids.assign(ids.begin() + start, ids.begin() + end);
                    entry.has_prefix = dict_.lookup_exact_span(
                        ids, start, end, lookup_limits, deadline, entry.candidates, lookup_stats);
                    memo.push_back(std::move(entry));
                    memo_entry = &memo.back();
                }
                if (!memo_entry->has_prefix) {
                    break;
                }

                // Homophones within a tenth of the span's most common word (改 / 该, not 瓦 / 哇;
                // limits.homophone_ratio); a repeated short code takes its most common word only.
                const int top_frequency =
                    memo_entry->candidates.empty() ? 0 : memo_entry->candidates.front().source_frequency;
                size_t taken = 0;
                for (const auto& candidate : memo_entry->candidates) {
                    if (taken > 0 && (path.kind == CompositionPathKind::kRepeatedShortCode ||
                                      (limits.homophone_ratio > 0 &&
                                       static_cast<int64_t>(candidate.source_frequency) *
                                               limits.homophone_ratio <
                                           top_frequency))) {
                        break;
                    }
                    ++taken;
                    if (edges.size() >= span_capacity) {
                        stats.truncated = true;
                        stop_queries = true;
                        break;
                    }
                    SpanEdge edge;
                    edge.end = static_cast<uint16_t>(end);
                    edge.score = word_score(candidate.source_frequency);
                    for (size_t joint = start + 1; joint < end; ++joint) {
                        if (ambiguous_joint[joint]) {
                            edge.score += kAmbiguousJointPenalty;
                        }
                    }
                    if (scorer && !scorer->sequential()) {
                        double ln_p = 0.0;
                        if (!scorer->score(candidate.text, start, end, ln_p)) {
                            continue;
                        }
                        edge.score = static_cast<int64_t>(std::llround(
                            ln_p * kComposedScoreScale +
                            scorer->word_weight() * static_cast<double>(edge.score)));
                    }
                    edge.candidate = candidate;
                    edges_by_start[start].push_back(static_cast<uint32_t>(edges.size()));
                    edges.push_back(std::move(edge));
                }
                if (lookup_stats.truncated) {
                    stop_queries = true;
                    break;
                }
            }
        }
        stats.span_candidate_count += static_cast<uint32_t>(edges.size());

        if (scorer && scorer->sequential()) {
            // A decoder model writes the sentence a character at a time over the same words
            // (sequential_compose); its paths are the composed sentences of this syllable path.
            std::vector<std::vector<SequentialEdge>> by_start(ids.size());
            for (size_t start = 0; start < ids.size(); ++start) {
                for (uint32_t edge_index : edges_by_start[start]) {
                    const SpanEdge& edge = edges[edge_index];
                    by_start[start].push_back({edge.end, edge.score, &edge.candidate.text});
                }
            }
            bool hit = false;
            const auto found = sequential_compose(ids.size(), by_start, &ambiguous_joint, *scorer,
                                                  result_capacity, deadline, hit);
            if (hit) {
                stats.deadline_exceeded = true;
                stats.truncated = true;
            }
            for (const auto& found_path : found) {
                if (found_path.edges.size() < 2) {
                    continue;  // a single word is already a dictionary candidate
                }
                ComposedResult result;
                Candidate& candidate = result.candidate;
                int32_t weakest = (std::numeric_limits<int32_t>::max)();
                for (const auto& [start, index] : found_path.edges) {
                    const SpanEdge& edge = edges[edges_by_start[start][index]];
                    candidate.text += edge.candidate.text;
                    weakest = (std::min)(weakest, edge.candidate.source_frequency);
                }
                candidate.code = path.input_code ? *path.input_code : input;
                if (path.input_code) {
                    candidate.input_code = *path.input_code;
                }
                candidate.syllables = join_syllables(*path.syllables);
                candidate.origin = CandidateOrigin::kComposed;
                candidate.source = CandidateSource::kPinyin;
                candidate.source_frequency = weakest;
                candidate.frequency = sentence_frequency(found_path.score);
                candidate.composed_score = (std::min)(found_path.score, int64_t{-1});
                result.kind = path.kind;
                result.path_rank = path.rank;
                result.segment_count = static_cast<uint16_t>(found_path.edges.size());
                result.weakest_frequency = weakest;
                result.aggregate_score = found_path.score;
                result.sequence = ++result_sequence;
                offer_result(results, std::move(result), result_capacity);
            }
            if (lookup_stats.truncated || hit) {
                break;
            }
            continue;
        }

        const uint32_t node_capacity =
            limits.max_nodes > stats.state_count ? limits.max_nodes - stats.state_count : 0;
        if (node_capacity == 0) {
            stats.truncated = true;
            break;
        }
        std::vector<CompositionNode> nodes;
        nodes.reserve(node_capacity);
        CompositionNode root;
        root.weakest_frequency = (std::numeric_limits<int32_t>::max)();
        nodes.push_back(root);
        std::vector<std::vector<uint32_t>> beams(ids.size() + 1);
        beams[0].push_back(0);
        uint32_t node_sequence = 0;
        bool node_limit_hit = false;

        for (size_t start = 0; start < ids.size() && !node_limit_hit; ++start) {
            if (beams[start].empty() || ambiguous_joint[start]) {
                continue;
            }
            for (uint32_t parent_index : beams[start]) {
                for (uint32_t edge_index : edges_by_start[start]) {
                    if (deadline.enabled && node_sequence > 0 && (node_sequence & 63) == 0 &&
                        deadline.expired()) {
                        stats.deadline_exceeded = true;
                        stats.truncated = true;
                        node_limit_hit = true;
                        break;
                    }
                    const auto& parent = nodes[parent_index];
                    const auto& edge = edges[edge_index];
                    CompositionNode node;
                    node.end = edge.end;
                    node.segment_count = static_cast<uint16_t>(parent.segment_count + 1);
                    node.weakest_frequency =
                        parent.segment_count == 0
                            ? edge.candidate.source_frequency
                            : (std::min)(parent.weakest_frequency, edge.candidate.source_frequency);
                    node.aggregate_score = parent.aggregate_score + edge.score;
                    node.parent = parent_index;
                    node.edge_index = edge_index;
                    node.sequence = ++node_sequence;
                    if (!offer_node(nodes, beams[node.end], node, limits.max_beam_width,
                                    node_capacity)) {
                        stats.truncated = true;
                        node_limit_hit = true;
                        break;
                    }
                }
                if (node_limit_hit) {
                    break;
                }
            }
        }
        stats.state_count += static_cast<uint32_t>(nodes.size());

        auto& completed = beams.back();
        std::sort(completed.begin(), completed.end(), [&](uint32_t left, uint32_t right) {
            return node_better(nodes[left], nodes[right]);
        });
        for (uint32_t node_index : completed) {
            const auto& node = nodes[node_index];
            if (node.segment_count < 2) {
                continue;  // a single word is already a dictionary candidate (as Rime's Poet)
            }
            ComposedResult result;
            result.candidate = rebuild_candidate(input, path, nodes, edges, node_index);
            result.kind = path.kind;
            result.path_rank = path.rank;
            result.segment_count = node.segment_count;
            result.weakest_frequency = node.weakest_frequency;
            result.aggregate_score = node.aggregate_score;
            result.sequence = ++result_sequence;
            offer_result(results, std::move(result), result_capacity);
        }

        if (lookup_stats.truncated || node_limit_hit) {
            break;
        }
    }

    stats.span_query_count = lookup_stats.range_queries;
    stats.span_entry_scan_count = lookup_stats.entry_scans;
    stats.deadline_exceeded = stats.deadline_exceeded || lookup_stats.deadline_exceeded;
    stats.truncated = stats.truncated || lookup_stats.truncated;
    std::sort(results.begin(), results.end(), result_better);

    // Keep the leading sentences while each stays close to the previous one (Rime's
    // MakeSentences), plus the best sentence of each other syllable path not far from the best
    // one ("nikanxiane": 你看限额 and 你看下呢); the model picks among them.
    std::vector<Candidate> candidates;
    candidates.reserve((std::min)(results.size(), sentence_limit));
    std::vector<uint16_t> paths_shown;
    double cutoff = limits.sentence_cutoff;
    const double acceleration =
        limits.max_sentences > 0 ? 1.0 - 1.0 / static_cast<double>(limits.max_sentences) : 1.0;
    const auto distance = [](const ComposedResult& from, const ComposedResult& to) {
        const double base = static_cast<double>(from.aggregate_score);
        return base == 0.0 ? 0.0
                           : std::fabs(static_cast<double>(to.aggregate_score) - base) /
                                 std::fabs(base);
    };
    bool chain_open = true;
    size_t last = 0;
    for (size_t i = 0; i < results.size() && candidates.size() < sentence_limit; ++i) {
        if (results[i].kind != results[0].kind) {
            break;
        }
        bool take = i == 0;
        if (!take && chain_open) {
            take = distance(results[last], results[i]) <= cutoff;
            if (take) {
                cutoff *= acceleration;
            } else {
                chain_open = false;
            }
        }
        if (!take && std::find(paths_shown.begin(), paths_shown.end(), results[i].path_rank) ==
                         paths_shown.end()) {
            take = distance(results[0], results[i]) <= limits.path_cutoff;
        }
        if (take) {
            last = i;
            if (std::find(paths_shown.begin(), paths_shown.end(), results[i].path_rank) ==
                paths_shown.end()) {
                paths_shown.push_back(results[i].path_rank);
            }
            candidates.push_back(std::move(results[i].candidate));
        }
    }
    stats.candidate_count = static_cast<uint32_t>(candidates.size());
    return candidates;
}

} // namespace cxxime
