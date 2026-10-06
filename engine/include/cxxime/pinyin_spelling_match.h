// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// How a typed pinyin string reads a word's syllables, the way the dictionary ranking reads an
// input (candidate-selection.md): every syllable typed in full, as its initial (z / zh), a
// final 儿 typed as r, and the last typed syllable may be unfinished. Used to find learned
// words for any way of typing them: 现在 (xian:zai) learned once answers "xz", "xianz" and
// "xianzai".
#ifndef CXXIME_PINYIN_SPELLING_MATCH_H_
#define CXXIME_PINYIN_SPELLING_MATCH_H_

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace cxxime {

struct SpellingMatch {
    float credibility = 0.0f;  // ln scale, <= 0: ln 0.5 per initial, ln 0.05 for an unfinished
                               // last syllable (as the ranking of completions)
    int initials = 0;          // syllables typed by their initial only
    bool unfinished = false;   // the last typed syllable is cut short
};

constexpr float kSpellingInitialCredibility = -0.6931472f;     // ln(0.5)
constexpr float kSpellingUnfinishedCredibility = -2.9957323f;  // ln(0.05)

// The best reading of `syllables` ("xian:zai") by `typed` ("xianz"), or nullopt when the
// letters do not spell exactly these syllables (a longer word is not matched: no prediction).
inline std::optional<SpellingMatch> match_typed_spelling(std::string_view typed,
                                                          std::string_view syllables) {
    if (typed.empty() || syllables.empty()) {
        return std::nullopt;
    }
    std::vector<std::string_view> parts;
    while (!syllables.empty()) {
        const std::size_t colon = syllables.find(':');
        parts.push_back(syllables.substr(0, colon));
        if (colon == std::string_view::npos) break;
        syllables.remove_prefix(colon + 1);
    }
    if (parts.size() > typed.size()) {
        return std::nullopt;  // at least one letter per syllable
    }
    std::optional<SpellingMatch> best;
    auto offer = [&](const SpellingMatch& match) {
        if (!best || match.credibility > best->credibility) best = match;
    };
    // Depth-first over the few readings: position in `typed`, syllable index, running match.
    struct Frame {
        std::size_t p;
        std::size_t i;
        SpellingMatch match;
    };
    std::vector<Frame> stack;
    stack.push_back({0, 0, {}});
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.i == parts.size()) {
            if (frame.p == typed.size()) offer(frame.match);
            continue;
        }
        if (frame.p >= typed.size()) continue;
        const std::string_view s = parts[frame.i];
        const std::string_view rest = typed.substr(frame.p);
        const bool last_syllable = frame.i + 1 == parts.size();
        // Typed in full.
        if (rest.substr(0, s.size()) == s) {
            stack.push_back({frame.p + s.size(), frame.i + 1, frame.match});
        }
        // By its initial: the first letter, or zh / ch / sh.
        const bool retroflex =
            s.size() > 2 && s[1] == 'h' && (s[0] == 'z' || s[0] == 'c' || s[0] == 's');
        for (std::size_t length = 1; length <= (retroflex ? 2u : 1u); ++length) {
            if (length < s.size() && rest.substr(0, length) == s.substr(0, length)) {
                SpellingMatch match = frame.match;
                match.credibility += kSpellingInitialCredibility;
                ++match.initials;
                stack.push_back({frame.p + length, frame.i + 1, match});
            }
        }
        // A final 儿 typed as r (erhua), a whole syllable.
        if (frame.i > 0 && last_syllable && s == "er" && rest[0] == 'r') {
            stack.push_back({frame.p + 1, frame.i + 1, frame.match});
        }
        // The last typed syllable cut short, longer than its initial ("xianz" + "ai" no, "xia"
        // of xian yes): only when the input ends there, and it covers the last syllable.
        if (last_syllable && rest.size() < s.size() && rest.size() > (retroflex ? 2u : 1u) &&
            s.substr(0, rest.size()) == rest) {
            SpellingMatch match = frame.match;
            match.credibility += kSpellingUnfinishedCredibility;
            match.unfinished = true;
            stack.push_back({typed.size(), frame.i + 1, match});
        }
    }
    return best;
}

}  // namespace cxxime

#endif  // CXXIME_PINYIN_SPELLING_MATCH_H_
