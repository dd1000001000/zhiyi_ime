// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#pragma once

#include <string>

namespace cxxime {

// Semantic admission for external personal data, not wire validation: text, mixed text, or
// emoji alone; not other symbols alone (the \ symbol input offers those).
// Use at load/import/edit boundaries, not candidate queries or commit learning.
bool is_ordinary_candidate_text(const std::string& text);

} // namespace cxxime
