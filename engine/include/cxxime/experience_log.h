// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// User experience improvement program: local logs in two opt-in tiers (both off by default;
// the second needs the first). Nothing is sent anywhere. docs/privacy.md lists every field.
//
// 1. Config::experience_program -> logs\experience.jsonl (1 MB, one old file kept): versions,
//    hardware size, settings, and the health of the IME (response errors, Laya model state).
// 2. Config::collect_input -> logs\input-YYYYMMDD.jsonl (7 days, 10 MB in all): usage
//    statistics, and one record per input: program and window title, the keys the IME
//    handled, the code, the candidates shown, the pick, the committed text and the context
//    given to Laya. Keys the IME does not handle (passed to the program) are never logged;
//    fields where the keyboard is disabled (passwords) never reach the IME.
//
// Revoking a tier stops logging but keeps the files: delete_all_logs() (Settings > Privacy)
// removes both tiers' files.
#ifndef CXXIME_EXPERIENCE_LOG_H_
#define CXXIME_EXPERIENCE_LOG_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cxxime {

struct Config;
struct CandidatePick;

// One input, from its first key to the commit (or cancel). Second tier only.
struct InputRecord {
    std::string app;           // program file name, e.g. WINWORD.EXE
    std::string window_title;
    std::string mode;          // pinyin / wubi / english / symbol
    std::vector<std::string> keys;  // the keys the IME handled, e.g. "n", "BACK", "SPACE"
    std::string code;          // the typed code (pinyin, Wubi code, English letters)
    std::vector<std::pair<std::string, bool>> candidates;  // shown (text, recommended)
    int picked = -1;           // candidate position, -1 when not picked from the list
    std::string committed;     // empty when cancelled
    std::string laya_context;  // the preceding text given to the Laya model
    std::uint32_t duration_ms = 0;
};

class ExperienceLog {
public:
    static ExperienceLog& instance();

    // Applies both tiers. The first tier writes a "start" record once and a "config" record
    // when the settings change.
    void configure(const Config& config);
    bool collecting_input() const { return collect_input_.load(std::memory_order_relaxed); }

    void record_error();                         // first tier: a response that failed
    void record_key(std::uint32_t latency_us);   // key downs; counted in the second tier
    void record_pick(const CandidatePick& pick); // second tier
    void record_input(const InputRecord& input); // second tier

    // Writes the pending health / statistics records (also every 30 minutes).
    void flush();

    // Deletes every log of both tiers in `directory` (default: the user's logs folder).
    static void delete_all_logs(const std::wstring& directory = {});

    // For tests: the logs folder (default: the user's logs folder).
    void set_directory_for_testing(const std::wstring& directory);

private:
    ExperienceLog() = default;
    bool ensure_directory_locked();
    void write_line_locked(const std::wstring& path, const std::string& line);
    void write_input_line_locked(const std::string& line);
    void write_window_locked();
    void reset_window_locked();

    std::mutex mutex_;
    bool enabled_ = false;  // first tier
    std::atomic<bool> collect_input_{false};
    bool started_ = false;
    std::string config_summary_;
    std::wstring directory_;

    std::chrono::steady_clock::time_point window_start_{};
    // First tier health.
    std::uint64_t errors_ = 0;
    int last_laya_state_ = -1;  // 0 loading / off, 1 ready, 2 failed
    // Second tier statistics.
    std::uint64_t keys_ = 0;
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
