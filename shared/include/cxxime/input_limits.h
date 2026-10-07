// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_INPUT_LIMITS_H_
#define CXXIME_INPUT_LIMITS_H_

#include <cstddef>

namespace cxxime {

constexpr std::size_t kMaxInputCodeLength = 64;
constexpr std::size_t kMaxWubiCodeLength = 4;
constexpr std::size_t kCandidateCapacity = 10;
constexpr std::size_t kCandidateTextCapacity = 256;
// Annotation after a candidate (IPCResponse::candidate_comments), with its terminator.
constexpr std::size_t kCandidateCommentCapacity = 64;
// Learning mode translation of a candidate (candidate_presentation.h), with its terminator.
constexpr std::size_t kCandidateGlossCapacity = 128;

} // namespace cxxime

#endif // CXXIME_INPUT_LIMITS_H_
