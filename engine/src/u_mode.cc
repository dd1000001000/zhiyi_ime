// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// U mode (cxxime/uv_mode.h): characters by their strokes and parts.

#include <cxxime/uv_mode.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

#include <cxxime/data_path.h>

namespace cxxime {
namespace {

constexpr std::size_t kMaxTokens = 5;
constexpr std::size_t kMaxSegmentations = 64;

bool is_stroke(char c) { return c == 'h' || c == 's' || c == 'p' || c == 'n' || c == 'z'; }

std::vector<std::string_view> split_view(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    if (s.empty()) return parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = s.find(separator, start);
        parts.push_back(s.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

// Calls `f` with each item of a comma-separated list until it returns true (no allocation: the
// stroke lookup runs over every character a key).
template <typename F>
bool any_item(std::string_view list, F f) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        if (f(list.substr(0, comma))) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool any_starts_with(std::string_view list, std::string_view prefix, bool* exact = nullptr) {
    bool found = false;
    any_item(list, [&](std::string_view item) {
        if (item.substr(0, prefix.size()) != prefix) return false;
        found = true;
        if (item.size() != prefix.size()) return false;
        if (exact) *exact = true;
        return true;
    });
    return found;
}

bool contains(std::string_view list, std::string_view value) {
    return any_item(list, [&](std::string_view item) { return item == value; });
}

struct SharedTable {
    std::mutex mutex;
    std::once_flag once;
    std::shared_ptr<const UModeTable> table;
};

SharedTable& shared_table() {
    static SharedTable state;
    return state;
}

}  // namespace

bool UModeTable::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str());
}

bool UModeTable::parse(std::string text) {
    text_ = std::move(text);
    entries_.clear();
    splits_.clear();
    by_first_part_.clear();
    parts_.clear();
    syllables_.clear();
    std::unordered_map<std::string_view, int> index;
    std::vector<std::pair<int, std::string_view>> split_texts;
    for (std::string_view line : split_view(text_, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.remove_suffix(1);
        const std::vector<std::string_view> fields = split_view(line, '\t');
        if (fields.size() < 6 || fields[0].empty()) continue;
        Entry entry;
        entry.text = fields[0];
        entry.strokes = fields[1];
        entry.readings = fields[2];
        entry.plain = fields[3];
        entry.frequency = std::atoi(std::string(fields[4]).c_str());
        const int at = static_cast<int>(entries_.size());
        index.emplace(entry.text, at);
        entries_.push_back(entry);
        if (!fields[5].empty()) split_texts.emplace_back(at, fields[5]);
    }
    std::unordered_set<int> parts;
    for (const auto& [at, splits] : split_texts) {
        auto& resolved = splits_[at];
        for (std::string_view split : split_view(splits, ';')) {
            std::vector<int> components;
            for (std::string_view name : split_view(split, ' ')) {
                const auto found = index.find(name);
                if (found == index.end()) {
                    components.clear();
                    break;
                }
                components.push_back(found->second);
            }
            if (components.size() < 2) continue;
            by_first_part_[components[0]].emplace_back(at, static_cast<int>(resolved.size()));
            parts.insert(components.begin(), components.end());
            resolved.push_back(std::move(components));
        }
    }
    parts_.assign(parts.begin(), parts.end());
    std::sort(parts_.begin(), parts_.end());
    std::set<std::string> syllables;
    for (int part : parts_) {
        for (std::string_view reading : split_view(entries_[static_cast<std::size_t>(part)].plain, ','))
            syllables.emplace(reading);
    }
    syllables_.assign(syllables.begin(), syllables.end());
    return !entries_.empty();
}

std::shared_ptr<const UModeTable> UModeTable::shared() {
    SharedTable& state = shared_table();
    std::call_once(state.once, [&state] {
        auto table = std::make_shared<UModeTable>();
        if (table->load(data_path("uv_chars.tsv"))) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.table) state.table = std::move(table);
        }
    });
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.table;
}

void UModeTable::set_shared(std::shared_ptr<const UModeTable> table) {
    SharedTable& state = shared_table();
    std::call_once(state.once, [] {});  // a table set is not replaced by the file
    std::lock_guard<std::mutex> lock(state.mutex);
    state.table = std::move(table);
}

std::vector<int> UModeTable::lookup(std::string_view input, std::size_t limit) const {
    if (input.empty()) return {};
    const bool strokes_only =
        std::all_of(input.begin(), input.end(), [](char c) { return is_stroke(c); });
    return strokes_only ? lookup_strokes(input, limit) : lookup_parts(input, limit);
}

// Characters whose strokes start so: the ones written with exactly these first, then by
// frequency (as Microsoft Pinyin: uh gives 一 再 开 天 ...).
std::vector<int> UModeTable::lookup_strokes(std::string_view strokes, std::size_t limit) const {
    std::vector<std::tuple<bool, int, int>> found;  // (not exact, -frequency, index)
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        bool exact = false;
        if (any_starts_with(entries_[i].strokes, strokes, &exact))
            found.emplace_back(!exact, -entries_[i].frequency, static_cast<int>(i));
    }
    const std::size_t kept = (std::min)(limit, found.size());
    std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(kept), found.end());
    std::vector<int> result;
    result.reserve(kept);
    for (std::size_t i = 0; i < kept; ++i) result.push_back(std::get<2>(found[i]));
    return result;
}

bool UModeTable::part_matches(int part, const Token& token) const {
    const Entry& entry = entries_[static_cast<std::size_t>(part)];
    if (token.strokes) return any_starts_with(entry.strokes, token.text);
    return token.last ? any_starts_with(entry.plain, token.text) : contains(entry.plain, token.text);
}

// Characters by their parts, each typed as its reading (mu for 木) or its first strokes (ps for
// 川), in order: umumu gives 林 (木 木), uqips gives 氚 (气 川).
std::vector<int> UModeTable::lookup_parts(std::string_view input, std::size_t limit) const {
    // The ways to cut the letters into parts: a reading, or strokes (h s p n z); ' cuts.
    std::vector<std::vector<Token>> segmentations;
    std::vector<Token> current;
    auto is_syllable = [&](std::string_view s) {
        return std::binary_search(syllables_.begin(), syllables_.end(), s,
                                  [](const auto& a, const auto& b) { return std::string_view(a) < std::string_view(b); });
    };
    auto starts_syllable = [&](std::string_view s) {
        const auto it = std::lower_bound(syllables_.begin(), syllables_.end(), s,
                                         [](const std::string& a, std::string_view b) { return std::string_view(a) < b; });
        return it != syllables_.end() && std::string_view(*it).substr(0, s.size()) == s;
    };
    auto walk = [&](auto&& self, std::size_t pos) -> void {
        if (segmentations.size() >= kMaxSegmentations) return;
        while (pos < input.size() && input[pos] == '\'') ++pos;
        if (pos == input.size()) {
            if (!current.empty()) segmentations.push_back(current);
            return;
        }
        if (current.size() == kMaxTokens) return;
        std::size_t end = pos;
        while (end < input.size() && input[end] != '\'') ++end;
        // Readings, longest first.
        for (std::size_t len = end - pos; len >= 1; --len) {
            const std::string_view piece = input.substr(pos, len);
            const bool last = pos + len == input.size();
            if (is_syllable(piece) || (last && starts_syllable(piece))) {
                current.push_back({piece, false, last});
                self(self, pos + len);
                current.pop_back();
            }
        }
        // Strokes.
        std::size_t run = pos;
        while (run < end && is_stroke(input[run])) ++run;
        for (std::size_t stop = run; stop > pos; --stop) {
            current.push_back({input.substr(pos, stop - pos), true, stop == input.size()});
            self(self, stop);
            current.pop_back();
        }
    };
    walk(walk, 0);

    // The best match of each character: all its parts typed, then readings typed in full, then
    // frequency.
    std::unordered_map<int, std::tuple<int, int, int>> best;  // index -> (-whole, -quality, -frequency)
    for (const std::vector<Token>& tokens : segmentations) {
        int quality = 0;
        for (const Token& token : tokens) quality += token.strokes ? 1 : 2;
        // The parts that the first token can be.
        for (int first : parts_) {
            if (!part_matches(first, tokens[0])) continue;
            const auto users = by_first_part_.find(first);
            if (users == by_first_part_.end()) continue;
            for (const auto& [character, split_index] : users->second) {
                const std::vector<int>& split =
                    splits_.at(character)[static_cast<std::size_t>(split_index)];
                if (split.size() < tokens.size()) continue;
                bool all = true;
                for (std::size_t i = 1; i < tokens.size() && all; ++i)
                    all = part_matches(split[i], tokens[i]);
                if (!all) continue;
                const std::tuple<int, int, int> key{split.size() == tokens.size() ? -1 : 0, -quality,
                                                    -entries_[static_cast<std::size_t>(character)].frequency};
                const auto [it, inserted] = best.emplace(character, key);
                if (!inserted && key < it->second) it->second = key;
            }
        }
    }
    std::vector<std::pair<std::tuple<int, int, int>, int>> ranked;
    ranked.reserve(best.size());
    for (const auto& [index, key] : best) ranked.emplace_back(key, index);
    const std::size_t kept = (std::min)(limit, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(kept), ranked.end());
    std::vector<int> result;
    result.reserve(kept);
    for (std::size_t i = 0; i < kept; ++i) result.push_back(ranked[i].second);
    return result;
}

std::vector<UvCandidate> u_mode_candidates(const UModeTable& table, std::string_view input,
                                           std::size_t limit) {
    std::vector<UvCandidate> out;
    if (input.empty()) {
        out.push_back({"u", "笔画 hspnz 拆字 uu符号"});
        return out;
    }
    for (int index : table.lookup(input, limit)) {
        const UModeTable::Entry& entry = table.entry(index);
        // Up to three readings, as Microsoft Pinyin shows them.
        std::string comment;
        int shown = 0;
        for (std::string_view reading : split_view(entry.readings, ',')) {
            if (shown++ == 3) break;
            if (!comment.empty()) comment += ',';
            comment += reading;
        }
        out.push_back({std::string(entry.text), std::move(comment)});
    }
    return out;
}

}  // namespace cxxime
