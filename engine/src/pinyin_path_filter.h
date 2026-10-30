// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PINYIN_PATH_FILTER_H_
#define CXXIME_PINYIN_PATH_FILTER_H_

#include <string>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/syllabifier.h>

namespace cxxime {

bool path_consumes_entire_input(const std::string& input, const SegmentedPath& path);
bool is_normal_composition_path(const std::string& input, const SegmentedPath& path);
bool is_complete_normal_path(const std::string& input, const SegmentedPath& path);
bool is_repeated_short_code_path(const std::string& input, const SegmentedPath& path);
// The input without its syllable separators ("xi'an" -> "xian"): the code the learned words, the
// user's candidate order and the user dictionary are kept under.
std::string without_separators(const std::string& input);
// Whether `syllables` (a:b:c) read the input with every typed separator between two syllables,
// each syllable typed in full (ü as v) or by its initial: a learned word kept under the letters
// alone (显示 for "xianshi") does not answer "xi'anshi". True when the input has no separator or
// the candidate no syllables.
bool respects_separators(const std::string& input, const std::string& syllables);
// Drops the candidates that do not respect the separators typed in `input`.
void keep_separator_matches(const std::string& input, std::vector<Candidate>& candidates);

} // namespace cxxime

#endif // CXXIME_PINYIN_PATH_FILTER_H_
