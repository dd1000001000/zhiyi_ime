// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/syllabifier.h>

#include <algorithm>
#include <queue>
#include <string_view>

#include <cxxime/query_budget.h>

#include "syllable_frequency.h"

namespace cxxime {

namespace {

// ln of the syllable's share of all syllable occurrences in the dictionary; a syllable the
// dictionary never uses counts as rarer than any it does.
float syllable_log_share(std::string_view syllable) {
    const auto* begin = std::begin(kSyllableFrequencies);
    const auto* end = std::end(kSyllableFrequencies);
    const auto* it = std::lower_bound(
        begin, end, syllable,
        [](const SyllableFrequency& entry, std::string_view key) { return entry.syllable < key; });
    if (it != end && it->syllable == syllable) {
        return it->log_share;
    }
    return -25.0f;
}

// Initials mode: a syllable span is one letter, or one of the two-letter initials zh/ch/sh.
bool is_initial_span(std::string_view input, size_t length) {
    if (length == 1) {
        return true;
    }
    return length == 2 && input[1] == 'h' &&
           (input[0] == 'z' || input[0] == 'c' || input[0] == 's');
}

bool match_initials(std::string_view input, const std::vector<std::string_view>& syllables,
                    size_t input_pos, size_t syllable_index) {
    if (syllable_index == syllables.size()) {
        return input_pos == input.size();
    }
    const std::string_view syllable = syllables[syllable_index];
    if (input_pos >= input.size() || syllable.empty() || input[input_pos] != syllable[0]) {
        return false;
    }
    if (match_initials(input, syllables, input_pos + 1, syllable_index + 1)) {
        return true;
    }
    return syllable.size() >= 2 && syllable[1] == 'h' && input_pos + 1 < input.size() &&
           input[input_pos + 1] == 'h' && is_initial_span(syllable, 2) &&
           match_initials(input, syllables, input_pos + 2, syllable_index + 1);
}

bool match_mixed(std::string_view input, const std::vector<std::string_view>& syllables,
                 size_t input_pos, size_t syllable_index) {
    if (syllable_index == syllables.size()) {
        return input_pos == input.size();
    }
    const std::string_view syllable = syllables[syllable_index];
    if (input_pos >= input.size() || syllable.empty() || input[input_pos] != syllable[0]) {
        return false;
    }
    // The whole syllable typed.
    if (input.substr(input_pos, syllable.size()) == syllable &&
        match_mixed(input, syllables, input_pos + syllable.size(), syllable_index + 1)) {
        return true;
    }
    // Its initial: one letter, or zh/ch/sh.
    if (match_mixed(input, syllables, input_pos + 1, syllable_index + 1)) {
        return true;
    }
    return syllable.size() >= 2 && syllable[1] == 'h' && input_pos + 1 < input.size() &&
           input[input_pos + 1] == 'h' && is_initial_span(syllable, 2) &&
           match_mixed(input, syllables, input_pos + 2, syllable_index + 1);
}

std::vector<std::string_view> split_syllables(std::string_view syllables) {
    std::vector<std::string_view> parts;
    size_t begin = 0;
    while (begin <= syllables.size()) {
        size_t end = syllables.find(':', begin);
        if (end == std::string_view::npos) {
            end = syllables.size();
        }
        parts.push_back(syllables.substr(begin, end - begin));
        begin = end + 1;
    }
    return parts;
}

}  // namespace

bool pinyin_matches_mixed(std::string_view input, std::string_view syllables) {
    if (input.empty() || syllables.empty()) {
        return false;
    }
    const std::vector<std::string_view> parts = split_syllables(syllables);
    return parts.size() <= input.size() && match_mixed(input, parts, 0, 0);
}

bool pinyin_matches_initials(std::string_view input, std::string_view syllables) {
    if (input.empty() || syllables.empty()) {
        return false;
    }
    const std::vector<std::string_view> parts = split_syllables(syllables);
    return parts.size() <= input.size() && match_initials(input, parts, 0, 0);
}

constexpr float kErhuaCredibility = 0.0f;

Syllabifier::Syllabifier(const SpellingsIndex& spellings)
    : spellings_(spellings) {}

SyllableGraph Syllabifier::build_graph(const std::string& input,
                                       const SyllabifierOptions& options) const {
    SyllableGraph graph;
    if (input.empty() || !spellings_.has_spellings())
        return graph;

    // BFS with priority queue (pos, spelling_type)
    // Corresponds to librime syllabifier.cc BuildSyllableGraph
    using Vertex = std::pair<size_t, int>;  // (pos, worst_type)
    std::priority_queue<Vertex, std::vector<Vertex>, std::greater<Vertex>> queue;
    std::vector<uint8_t> visited(input.size() + 1, 0);

    queue.push({0, kNormalSpelling});

    while (!queue.empty()) {
        auto [pos, vertex_type] = queue.top();
        queue.pop();

        if (visited[pos])
            continue;
        visited[pos] = 1;

        std::string_view remaining(input.data() + pos, input.size() - pos);
        auto matches =
            spellings_.prefix_search(remaining, options.enable_fuzzy, options.fuzzy_groups);

        for (auto& m : matches) {
            // The trie key is the raw input span. The canonical syllable may have a
            // different length for fuzzy spellings and Shuangpin zero initials.
            if (m.input_key_len == 0 || m.input_key_len > remaining.size()) {
                continue;
            }
            if (options.initials_only && !is_initial_span(remaining, m.input_key_len)) {
                continue;
            }
            const size_t end_pos = pos + m.input_key_len;
            if (end_pos > input.size())
                continue;

            // Add edge
            graph[pos][end_pos].push_back(
                {m.syllable, m.type, m.credibility, syllable_log_share(m.syllable)});

            // Enqueue end vertex with worst type along path
            int worst_type = std::max(vertex_type, m.type);
            if (end_pos < visited.size() && !visited[end_pos]) {
                queue.push({end_pos, worst_type});
            }
        }

        // Erhua: an r after a syllable spells 儿 in full (花儿 "huar", 哪儿 "nar", 一会儿
        // "yihuir"), a whole syllable like "er" typed out; the r-initial readings (华人, 纳入)
        // stay as abbreviations.
        if (pos > 0 && !remaining.empty() && remaining[0] == 'r' && !options.initials_only) {
            graph[pos][pos + 1].push_back(
                {"er", kNormalSpelling, kErhuaCredibility, syllable_log_share("er")});
            if (pos + 1 < visited.size() && !visited[pos + 1]) {
                queue.push({pos + 1, vertex_type});
            }
        }
    }

    if (options.enable_terminal_completion && !options.initials_only) {
        static constexpr float kCompletionPenalty = -0.69314718f;
        const size_t end_position = input.size();
        for (size_t position = 0; position < end_position; ++position) {
            if (!visited[position]) {
                continue;
            }
            const std::string_view remaining(input.data() + position,
                                             end_position - position);
            auto completions = spellings_.completion_search(remaining, options.enable_fuzzy,
                                                            options.fuzzy_groups);
            auto& edges = graph[position][end_position];
            for (const auto& completion : completions) {
                if (completion.type >= kAbbreviation) {
                    continue;
                }
                const bool duplicate = std::any_of(
                    edges.begin(), edges.end(), [&completion](const SyllableEdge& edge) {
                        return edge.syllable == completion.syllable;
                    });
                if (!duplicate) {
                    edges.push_back({completion.syllable, kCompletionSpelling,
                                     completion.credibility + kCompletionPenalty,
                                     syllable_log_share(completion.syllable)});
                }
            }
            if (edges.empty()) {
                graph[position].erase(end_position);
            }
        }
    }

    return graph;
}

// Best paths first, from position 0 to end_pos: a path's score is its credibility (every
// abbreviation or fuzzy spelling costs the same as in the final ranking) plus the weight of its
// syllables, so the plausible readings come out first. An initial after full pinyin expands to
// every syllable it starts ("womsh": m, sh); the depth-first walk this replaces took them in
// alphabetical order and spent the cap on wo:ma:sa:ha... before reaching wo:men:shi.
// A* over partial paths: the priority adds the best score any continuation can still reach
// (exact, from a backward pass over the graph), so only prefixes of the paths about to be
// reported are expanded and dead ends are never entered.
bool Syllabifier::enumerate_paths(const SyllableGraph& graph, size_t end_pos,
                                  std::vector<SegmentedPath>& results,
                                  const QueryDeadline* deadline, bool collect_path_metadata,
                                  uint32_t& call_count) const {
    // Cap at 256 paths — the translator keeps the first kMaxPaths, and dense abbreviation
    // graphs (150+ edges) can produce 10K+ paths which wastes CPU on sorting/dedup.
    static constexpr size_t kMaxPaths = 256;
    // Positions expanded: bounds the work (each reported path expands one node per syllable).
    static constexpr uint32_t kMaxExpansions = 8192;
    static constexpr float kUnreachable = -1e30f;
    if (results.size() >= kMaxPaths) {
        return false;
    }

    // Best score of any continuation from each position to end_pos.
    std::vector<float> to_end(end_pos + 1, kUnreachable);
    to_end[end_pos] = 0.0f;
    for (size_t pos = end_pos; pos-- > 0;) {
        const auto it = graph.find(pos);
        if (it == graph.end()) {
            continue;
        }
        for (const auto& [end, edges] : it->second) {
            if (end > end_pos || to_end[end] == kUnreachable) {
                continue;
            }
            for (const SyllableEdge& edge : edges) {
                to_end[pos] = (std::max)(to_end[pos], edge.credibility + edge.weight + to_end[end]);
            }
        }
    }
    if (to_end[0] == kUnreachable) {
        return false;
    }

    struct Node {
        size_t pos;
        int parent;
        const SyllableEdge* edge;  // the edge that led here (null at the root)
        uint16_t input_length;
        float score;  // credibility + weight so far, plus the best continuation (to_end)
        float credibility;
    };
    std::vector<Node> nodes;
    nodes.push_back({0, -1, nullptr, 0, to_end[0], 0.0f});
    auto later = [&nodes](int a, int b) { return nodes[a].score < nodes[b].score; };
    std::priority_queue<int, std::vector<int>, decltype(later)> queue(later);
    queue.push(0);
    uint32_t expansions = 0;

    while (!queue.empty()) {
        // Check the deadline every 32 steps (Clock::now() is not free), and on entry to catch
        // an already expired one.
        ++call_count;
        if (deadline && deadline->enabled) {
            if ((call_count <= 1 || (call_count & 31) == 0) && deadline->expired()) {
                return true;
            }
        }
        const int index = queue.top();
        queue.pop();
        const Node node = nodes[index];  // a copy: expanding reallocates `nodes`

        if (node.pos >= end_pos) {
            SegmentedPath path;
            path.credibility = node.credibility;
            for (int i = index; nodes[i].parent >= 0; i = nodes[i].parent) {
                path.syllables.push_back(nodes[i].edge->syllable);
                if (collect_path_metadata) {
                    path.spelling_types.push_back(static_cast<uint8_t>(nodes[i].edge->type));
                    path.input_lengths.push_back(nodes[i].input_length);
                }
            }
            std::reverse(path.syllables.begin(), path.syllables.end());
            std::reverse(path.spelling_types.begin(), path.spelling_types.end());
            std::reverse(path.input_lengths.begin(), path.input_lengths.end());
            results.push_back(std::move(path));
            if (results.size() >= kMaxPaths) {
                return false;
            }
            continue;
        }

        if (++expansions > kMaxExpansions) {
            return false;
        }
        const auto it = graph.find(node.pos);
        if (it == graph.end()) {
            continue;
        }
        for (const auto& [end, edges] : it->second) {
            const size_t input_length = end - node.pos;
            if (input_length > UINT16_MAX || end > end_pos || to_end[end] == kUnreachable) {
                continue;
            }
            // node.score includes to_end[node.pos]; the child's includes to_end[end] instead.
            const float traveled = node.score - to_end[node.pos];
            for (const SyllableEdge& edge : edges) {
                nodes.push_back({end, index, &edge, static_cast<uint16_t>(input_length),
                                 traveled + edge.credibility + edge.weight + to_end[end],
                                 node.credibility + edge.credibility});
                queue.push(static_cast<int>(nodes.size()) - 1);
            }
        }
    }
    return false;
}

SegmentResult Syllabifier::segment(const std::string& input, const QueryDeadline* deadline,
                                   const SyllabifierOptions& options) const {
    SegmentResult result;
    if (input.empty())
        return result;

    auto graph = build_graph(input, options);
    if (graph.empty())
        return result;

    // Find the farthest reachable position
    size_t farthest = 0;
    for (auto& [start, edges] : graph) {
        for (auto& [end, _] : edges) {
            if (end > farthest)
                farthest = end;
        }
    }

    if (farthest == 0)
        return result;

    // Enumerate paths from 0 to farthest.
    // enumerate_paths bails out at kMaxPaths to prevent exponential blowup
    // from dense abbreviation graphs.
    // Pass the deadline for internal checks during the search.
    std::vector<SegmentedPath> scored;
    uint32_t call_count = 0;
    bool deadline_expired = false;

    // Pass 1: paths made of whole syllables only (normal and fuzzy spellings). Dense
    // abbreviation graphs can produce thousands of paths, which would fill kMaxPaths before
    // the search reaches a fuzzy branch (cen -> cheng); these few paths always fit.
    SyllableGraph whole_syllables;
    bool has_abbreviation = false;
    for (const auto& [start, groups] : graph) {
        for (const auto& [end, edges] : groups) {
            for (const SyllableEdge& edge : edges) {
                if (edge.type == kAbbreviation) {
                    has_abbreviation = true;
                } else {
                    whole_syllables[start][end].push_back(edge);
                }
            }
        }
    }
    if (has_abbreviation && !whole_syllables.empty()) {
        deadline_expired = enumerate_paths(whole_syllables, farthest, scored, deadline,
                                           options.collect_path_metadata, call_count);
    }
    // Pass 2: every path (duplicates of pass 1 are removed below), up to kMaxPaths in total.
    if (!deadline_expired) {
        deadline_expired = enumerate_paths(graph, farthest, scored, deadline,
                                           options.collect_path_metadata, call_count);
    }

    if (deadline_expired) {
        result.deadline_exceeded = true;
        result.truncated = true;
    }

    // Sort by quality: all-normal paths, then paths with fuzzy spellings, then paths with
    // abbreviations; higher credibility first within each class.
    auto path_class = [](const SegmentedPath& path) {
        int worst = 0;
        for (const uint8_t type : path.spelling_types) {
            if (type == kAbbreviation || type == kCompletionSpelling) {
                return 2;
            }
            if (type == kFuzzySpelling) {
                worst = 1;
            }
        }
        return worst;
    };
    std::stable_sort(scored.begin(), scored.end(),
        [&](const auto& a, const auto& b) {
            const int class_a = path_class(a);
            const int class_b = path_class(b);
            if (class_a != class_b) {
                return class_a < class_b;
            }
            return a.credibility > b.credibility;
        });

    // Deduplicate and collect (linear scan — path count bounded by kMaxPaths)
    std::vector<std::string> seen_keys;
    for (auto& path : scored) {
        std::string key;
        for (auto& s : path.syllables) key += s + ":";
        bool dup = false;
        for (auto& k : seen_keys) {
            if (k == key) { dup = true; break; }
        }
        if (!dup) {
            seen_keys.push_back(key);
            result.paths.push_back(std::move(path));
        }
    }

    return result;
}

bool Syllabifier::matches_without_fuzzy(const std::string& input,
                                        const std::string& syllables) const {
    if (input.empty() || syllables.empty()) {
        return true;
    }
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= syllables.size()) {
        std::size_t end = syllables.find(':', begin);
        if (end == std::string::npos) {
            end = syllables.size();
        }
        parts.push_back(syllables.substr(begin, end - begin));
        begin = end + 1;
    }

    SyllabifierOptions options;
    options.enable_fuzzy = false;
    options.fuzzy_groups = 0;
    options.enable_terminal_completion = true;
    const SyllableGraph graph = build_graph(input, options);

    // Input positions reachable after matching the first k syllables.
    std::vector<std::size_t> positions = {0};
    for (const std::string& syllable : parts) {
        std::vector<std::size_t> next;
        for (const std::size_t position : positions) {
            if (position == input.size()) {
                return true;  // the rest of the word continues past the typed input
            }
            const auto edges = graph.find(position);
            if (edges == graph.end()) {
                continue;
            }
            for (const auto& [end, group] : edges->second) {
                for (const SyllableEdge& edge : group) {
                    if (edge.syllable == syllable &&
                        std::find(next.begin(), next.end(), end) == next.end()) {
                        next.push_back(end);
                    }
                }
            }
        }
        if (next.empty()) {
            return false;
        }
        positions = std::move(next);
    }
    return std::find(positions.begin(), positions.end(), input.size()) != positions.end();
}

bool Syllabifier::has_fuzzy_path(const std::string& input,
                                 const SyllabifierOptions& options) const {
    const SyllableGraph graph = build_graph(input, options);
    std::vector<uint8_t> reachable_without_fuzzy(input.size() + 1, 0);
    std::vector<uint8_t> reachable_with_fuzzy(input.size() + 1, 0);
    reachable_without_fuzzy[0] = 1;

    for (std::size_t position = 0; position < input.size(); ++position) {
        if (!reachable_without_fuzzy[position] && !reachable_with_fuzzy[position]) {
            continue;
        }
        const auto edge_groups = graph.find(position);
        if (edge_groups == graph.end()) {
            continue;
        }
        for (const auto& group : edge_groups->second) {
            for (const SyllableEdge& edge : group.second) {
                if (edge.type == kFuzzySpelling) {
                    reachable_with_fuzzy[group.first] = 1;
                } else {
                    reachable_without_fuzzy[group.first] =
                        reachable_without_fuzzy[group.first] || reachable_without_fuzzy[position];
                    reachable_with_fuzzy[group.first] =
                        reachable_with_fuzzy[group.first] || reachable_with_fuzzy[position];
                }
            }
        }
    }
    return !input.empty() && reachable_with_fuzzy[input.size()] != 0;
}

} // namespace cxxime
