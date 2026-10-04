// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// User experience improvement program (opt-in, Config::experience_program): a local log of how
// the IME runs, %USERPROFILE%\zhiyi\logs\experience.jsonl (one JSON object per line; rotated
// at 1 MB to experience.1.jsonl). It holds the IME version, Windows version, settings, key
// processing speed, errors, the Laya model state and how often each candidate position and the
// recommendation are picked. It never holds text: nothing typed, no candidates, no program
// names. Nothing is sent anywhere; the user decides whether to share the file.
// The full list is in docs/privacy.md.
#ifndef CXXIME_EXPERIENCE_LOG_H_
#define CXXIME_EXPERIENCE_LOG_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace cxxime {

struct Config;
struct CandidatePick;

class ExperienceLog {
public:
    static ExperienceLog& instance();

    // Turns the log on or off. While on: a "start" record the first time, a "config" record
    // when the settings change. Turning it off writes the pending statistics first.
    void configure(const Config& config);

    // One key press handled by the engine (key downs only).
    void record_key(std::uint32_t latency_us);
    // A response that could not be built (the key then reaches the application unhandled).
    void record_error();
    void record_pick(const CandidatePick& pick);

    // Writes the statistics gathered since the last "stats" record (also every 30 minutes).
    void flush();

    // For tests: the log file (default: the user's logs folder).
    void set_path_for_testing(const std::wstring& path);

private:
    ExperienceLog() = default;
    void write_line_locked(const std::string& line);
    void write_stats_locked();
    void reset_stats_locked();

    std::mutex mutex_;
    bool enabled_ = false;
    bool started_ = false;
    std::string config_summary_;
    std::wstring path_;

    std::chrono::steady_clock::time_point window_start_{};
    std::uint64_t keys_ = 0;
    std::uint64_t errors_ = 0;
    std::uint32_t max_latency_us_ = 0;
    // Latency buckets: < 5, 10, 20, 50, 100, 200 ms, and longer.
    std::array<std::uint64_t, 7> latency_buckets_{};
    std::array<std::uint64_t, 10> picks_by_position_{};  // the 10th also counts later ones
    std::uint64_t picks_with_recommendation_ = 0;
    std::uint64_t recommendation_picked_ = 0;
    long long laya_calls_at_window_start_ = 0;
    long long laya_reordered_at_window_start_ = 0;
};

}  // namespace cxxime

#endif  // CXXIME_EXPERIENCE_LOG_H_
