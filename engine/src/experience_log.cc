// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/experience_log.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <json.hpp>

#include <cxxime/config.h>
#include <cxxime/diagnostic_log_path.h>
#include <cxxime/engine.h>
#include <cxxime/keyboard_shortcut.h>
#include <cxxime/laya_rerank.h>
#include <cxxime/version.h>

namespace cxxime {

namespace {

constexpr auto kStatsInterval = std::chrono::minutes(30);
constexpr std::uint64_t kMaxFileBytes = 1024 * 1024;
constexpr std::uint32_t kLatencyBoundsMs[] = {5, 10, 20, 50, 100, 200};

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm = {};
    gmtime_s(&tm, &now);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string windows_build() {
    // RtlGetVersion is not subject to the manifest-based version lie of GetVersionEx.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info = {sizeof(info)};
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            fn(&info);
        }
    }
    return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
           "." + std::to_string(info.dwBuildNumber);
}

// The settings that shape the IME's behavior. Settings only: no user data.
nlohmann::json settings_of(const Config& c) {
    nlohmann::json s;
    s["input_mode"] = c.input_mode == 1 ? "wubi" : "pinyin";
    s["pinyin_scheme"] = c.pinyin_scheme;
    s["pinyin_initials"] = c.pinyin_initials;
    s["fuzzy_pinyin"] = c.fuzzy_pinyin;
    s["fuzzy_groups"] = c.fuzzy_groups;
    s["candidate_learning"] = c.candidate_learning;
    s["page_size"] = c.page_size;
    s["font_size"] = c.font_size;
    s["theme"] = c.theme;
    s["ui_language"] = c.ui_language;
    s["laya"] = c.laya.enable;
    s["english_correction"] = c.english.correction;
    s["english_words"] = c.english.word_mode;
    s["render_backend"] = c.render_backend;
    s["layout"] = c.layout;
    nlohmann::json keys;
    keys["ascii_toggle"] = keyboard_shortcut_string(c.ascii_toggle_shortcut);
    keys["style"] = keyboard_shortcut_string(c.english_style_shortcut);
    keys["punct"] = keyboard_shortcut_string(c.punct_toggle_shortcut);
    keys["shape"] = keyboard_shortcut_string(c.shape_toggle_shortcut);
    for (const char* tap : {"Shift_L", "Control_L"}) {
        const auto found = c.ascii_switch_key.find(tap);
        keys[tap] = found != c.ascii_switch_key.end() ? found->second : "";
    }
    s["switch_keys"] = keys;
    return s;
}

}  // namespace

ExperienceLog& ExperienceLog::instance() {
    static ExperienceLog log;
    return log;
}

void ExperienceLog::set_path_for_testing(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
}

void ExperienceLog::configure(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config.experience_program) {
        if (enabled_) write_stats_locked();
        enabled_ = false;
        return;
    }
    if (path_.empty()) {
        const std::wstring directory = diagnostic_log_directory();
        if (directory.empty()) return;
        path_ = directory + L"\\experience.jsonl";
    }
    const bool was_enabled = enabled_;
    enabled_ = true;
    if (!was_enabled) reset_stats_locked();
    if (!started_) {
        started_ = true;
        nlohmann::json start;
        start["t"] = utc_now();
        start["event"] = "start";
        start["version"] = CXXIME_VERSION_STRING;
        start["windows"] = windows_build();
        SYSTEM_INFO info = {};
        GetNativeSystemInfo(&info);
        start["cpu_count"] = info.dwNumberOfProcessors;
        MEMORYSTATUSEX memory = {sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory)) {
            start["memory_gb"] = static_cast<int>((memory.ullTotalPhys + (1ull << 29)) >> 30);
        }
        write_line_locked(start.dump());
    }
    const std::string summary = settings_of(config).dump();
    if (summary != config_summary_) {
        config_summary_ = summary;
        nlohmann::json record;
        record["t"] = utc_now();
        record["event"] = "config";
        record["settings"] = nlohmann::json::parse(summary);
        write_line_locked(record.dump());
    }
}

void ExperienceLog::record_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) ++errors_;
}

void ExperienceLog::record_key(std::uint32_t latency_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    ++keys_;
    max_latency_us_ = (std::max)(max_latency_us_, latency_us);
    size_t bucket = 0;
    while (bucket < std::size(kLatencyBoundsMs) && latency_us >= kLatencyBoundsMs[bucket] * 1000u) {
        ++bucket;
    }
    ++latency_buckets_[bucket];
    if (std::chrono::steady_clock::now() - window_start_ >= kStatsInterval) {
        write_stats_locked();
    }
}

void ExperienceLog::record_pick(const CandidatePick& pick) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    const size_t position = static_cast<size_t>(
        (std::clamp)(pick.index, 0, static_cast<int>(picks_by_position_.size()) - 1));
    ++picks_by_position_[position];
    if (pick.had_recommendation) {
        ++picks_with_recommendation_;
        if (pick.recommended) ++recommendation_picked_;
    }
}

void ExperienceLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) write_stats_locked();
}

void ExperienceLog::reset_stats_locked() {
    window_start_ = std::chrono::steady_clock::now();
    keys_ = errors_ = 0;
    max_latency_us_ = 0;
    latency_buckets_.fill(0);
    picks_by_position_.fill(0);
    picks_with_recommendation_ = recommendation_picked_ = 0;
    const LayaRerankStats laya = LayaRerank::instance().stats();
    laya_calls_at_window_start_ = laya.calls;
    laya_reordered_at_window_start_ = laya.reordered;
}

void ExperienceLog::write_stats_locked() {
    std::uint64_t picks = 0;
    for (std::uint64_t count : picks_by_position_) picks += count;
    if (keys_ == 0 && picks == 0 && errors_ == 0) {
        reset_stats_locked();
        return;
    }
    const LayaRerankStats laya = LayaRerank::instance().stats();
    nlohmann::json stats;
    stats["t"] = utc_now();
    stats["event"] = "stats";
    stats["minutes"] = std::chrono::duration_cast<std::chrono::minutes>(
                           std::chrono::steady_clock::now() - window_start_)
                           .count();
    stats["keys"] = keys_;
    stats["errors"] = errors_;
    stats["latency_ms_buckets"] = latency_buckets_;  // <5 <10 <20 <50 <100 <200 >=200
    stats["max_latency_ms"] = max_latency_us_ / 1000.0;
    stats["picks_by_position"] = picks_by_position_;
    stats["picks_with_recommendation"] = picks_with_recommendation_;
    stats["recommendation_picked"] = recommendation_picked_;
    stats["laya_ready"] = laya.model_ready;
    stats["laya_failed"] = laya.model_failed;
    stats["laya_calls"] = laya.calls - laya_calls_at_window_start_;
    stats["laya_reordered"] = laya.reordered - laya_reordered_at_window_start_;
    stats["laya_last_ms"] = laya.last_ms;
    write_line_locked(stats.dump());
    reset_stats_locked();
}

void ExperienceLog::write_line_locked(const std::string& line) {
    if (path_.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &data) &&
        ((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow) >=
            kMaxFileBytes) {
        std::wstring old = path_;
        const size_t dot = old.rfind(L".jsonl");
        old.insert(dot == std::wstring::npos ? old.size() : dot, L".1");
        MoveFileExW(path_.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path_.c_str(), L"ab") != 0 || !file) return;
    std::fwrite(line.data(), 1, line.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
}

}  // namespace cxxime
