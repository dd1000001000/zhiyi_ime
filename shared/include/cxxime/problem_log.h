// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// problems.log in the diagnostics log folder (diagnostic_log_path.h): one line for each failure
// a user may report without knowing why (settings that could not be saved, ...). Always on, so
// Collect Diagnostics carries the reason; it never holds typed text. Past kProblemLogMaxBytes the
// file becomes problems.log.1 (the one before is dropped).
#ifndef CXXIME_PROBLEM_LOG_H_
#define CXXIME_PROBLEM_LOG_H_

#include <cstdint>
#include <string>

namespace cxxime {

constexpr wchar_t kProblemLogName[] = L"problems.log";
constexpr std::uint64_t kProblemLogMaxBytes = 256 * 1024;

// "2026-10-11 14:30:05 zhiyi-server.exe [config] <message>" appended to problems.log.
void log_problem(const char* area, const std::string& message);
// The same, in `directory` (tests).
void log_problem_to(const std::wstring& directory, const char* area, const std::string& message);
// Tests that provoke failures: log_problem writes nothing, so the real problems.log stays clean.
void disable_problem_log();

// Windows' text for an error code in English, with the code: "Access is denied. (5)".
std::string windows_error_text(unsigned long error_code);

}  // namespace cxxime

#endif  // CXXIME_PROBLEM_LOG_H_
