// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/experience_log.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>

#include <json.hpp>

#include <cxxime/config.h>
#include <cxxime/diagnostic_log_path.h>
#include <cxxime/engine.h>
#include <cxxime/keyboard_shortcut.h>
#include <cxxime/laya_rerank.h>
#include <cxxime/version.h>

namespace cxxime {

namespace {

constexpr auto kWindow = std::chrono::minutes(30);
constexpr std::uint64_t kMaxExperienceBytes = 1024 * 1024;
constexpr std::uint64_t kMaxInputBytes = 10 * 1024 * 1024;
constexpr int kInputDays = 7;
constexpr std::uint32_t kLatencyBoundsMs[] = {5, 10, 20, 50, 100, 200};
constexpr wchar_t kExperienceFile[] = L"experience.jsonl";
constexpr wchar_t kExperienceOldFile[] = L"experience.1.jsonl";
constexpr wchar_t kInputPrefix[] = L"input-";  // input-YYYYMMDD.jsonl

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm = {};
    gmtime_s(&tm, &now);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::wstring utc_day(int days_ago = 0) {
    const std::time_t day = std::time(nullptr) - static_cast<std::time_t>(days_ago) * 86400;
    std::tm tm = {};
    gmtime_s(&tm, &day);
    wchar_t text[16] = {};
    wcsftime(text, 16, L"%Y%m%d", &tm);
    return text;
}

std::uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// input-YYYYMMDD.jsonl in `directory`, oldest first (the names sort by day).
std::map<std::wstring, std::uint64_t> input_files(const std::wstring& directory) {
    std::map<std::wstring, std::uint64_t> files;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((directory + L"\\input-*.jsonl").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return files;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            files[data.cFileName] =
                (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return files;
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

// The settings that shape the IME's behavior.
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
    s["collect_input"] = c.collect_input;
    s["update_notify"] = c.update_notify;
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

int laya_state(const LayaRerankStats& stats) {
    return stats.model_ready ? 1 : stats.model_failed ? 2 : 0;
}

}  // namespace

ExperienceLog& ExperienceLog::instance() {
    static ExperienceLog log;
    return log;
}

void ExperienceLog::set_directory_for_testing(const std::wstring& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    directory_ = directory;
}

bool ExperienceLog::ensure_directory_locked() {
    if (directory_.empty()) directory_ = diagnostic_log_directory();
    return !directory_.empty();
}

void ExperienceLog::configure(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool enable = config.experience_program;
    const bool collect = enable && config.collect_input;
    if (!enable) {
        if (enabled_) write_window_locked();
        enabled_ = false;
        collect_input_.store(false, std::memory_order_relaxed);
        return;
    }
    if (!ensure_directory_locked()) return;
    if (!enabled_) reset_window_locked();
    if (collect_input_.load(std::memory_order_relaxed) && !collect) {
        write_window_locked();  // the statistics so far, while still allowed
    }
    enabled_ = true;
    collect_input_.store(collect, std::memory_order_relaxed);
    const std::wstring path = directory_ + L"\\" + kExperienceFile;
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
        write_line_locked(path, start.dump());
    }
    const std::string summary = settings_of(config).dump();
    if (summary != config_summary_) {
        config_summary_ = summary;
        nlohmann::json record;
        record["t"] = utc_now();
        record["event"] = "config";
        record["settings"] = nlohmann::json::parse(summary);
        write_line_locked(path, record.dump());
    }
}

void ExperienceLog::record_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) ++errors_;
}

void ExperienceLog::record_key(std::uint32_t latency_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    if (collect_input_.load(std::memory_order_relaxed)) {  // statistics: second tier only
        ++keys_;
        max_latency_us_ = (std::max)(max_latency_us_, latency_us);
        size_t bucket = 0;
        while (bucket < std::size(kLatencyBoundsMs) &&
               latency_us >= kLatencyBoundsMs[bucket] * 1000u) {
            ++bucket;
        }
        ++latency_buckets_[bucket];
    }
    // Every 30 minutes of use: the health record (first tier) and the statistics.
    if (std::chrono::steady_clock::now() - window_start_ >= kWindow) write_window_locked();
}

void ExperienceLog::record_pick(const CandidatePick& pick) {
    if (!collecting_input()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!collect_input_.load(std::memory_order_relaxed)) return;
    const size_t position = static_cast<size_t>(
        (std::clamp)(pick.index, 0, static_cast<int>(picks_by_position_.size()) - 1));
    ++picks_by_position_[position];
    if (pick.had_recommendation) {
        ++picks_with_recommendation_;
        if (pick.recommended) ++recommendation_picked_;
    }
}

void ExperienceLog::record_input(const InputRecord& input) {
    if (!collecting_input()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!collect_input_.load(std::memory_order_relaxed)) return;
    nlohmann::json record;
    record["t"] = utc_now();
    record["event"] = "input";
    record["app"] = input.app;
    record["window_title"] = input.window_title;
    record["mode"] = input.mode;
    record["keys"] = input.keys;
    record["code"] = input.code;
    nlohmann::json candidates = nlohmann::json::array();
    for (const auto& [text, recommended] : input.candidates) {
        candidates.push_back({{"text", text}, {"recommended", recommended}});
    }
    record["candidates"] = candidates;
    record["picked"] = input.picked;
    record["committed"] = input.committed;
    record["laya_context"] = input.laya_context;
    record["duration_ms"] = input.duration_ms;
    // Invalid UTF-8 (a truncated title) is replaced rather than failing the record.
    write_input_line_locked(record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void ExperienceLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) write_window_locked();
}

void ExperienceLog::reset_window_locked() {
    window_start_ = std::chrono::steady_clock::now();
    errors_ = 0;
    keys_ = 0;
    max_latency_us_ = 0;
    latency_buckets_.fill(0);
    picks_by_position_.fill(0);
    picks_with_recommendation_ = recommendation_picked_ = 0;
    const LayaRerankStats laya = LayaRerank::instance().stats();
    laya_calls_at_window_start_ = laya.calls;
    laya_reordered_at_window_start_ = laya.reordered;
}

void ExperienceLog::write_window_locked() {
    const LayaRerankStats laya = LayaRerank::instance().stats();
    const std::string now = utc_now();
    // First tier: health, only when something happened (errors, a model state change).
    const int state = laya_state(laya);
    if (errors_ > 0 || state != last_laya_state_) {
        last_laya_state_ = state;
        nlohmann::json health;
        health["t"] = now;
        health["event"] = "health";
        health["errors"] = errors_;
        health["laya"] = state == 1 ? "ready" : state == 2 ? "failed" : "off";
        write_line_locked(directory_ + L"\\" + kExperienceFile, health.dump());
    }
    // Second tier: usage statistics.
    std::uint64_t picks = 0;
    for (std::uint64_t count : picks_by_position_) picks += count;
    if (collect_input_.load(std::memory_order_relaxed) && (keys_ > 0 || picks > 0)) {
        nlohmann::json stats;
        stats["t"] = now;
        stats["event"] = "stats";
        stats["minutes"] = std::chrono::duration_cast<std::chrono::minutes>(
                               std::chrono::steady_clock::now() - window_start_)
                               .count();
        stats["keys"] = keys_;
        stats["latency_ms_buckets"] = latency_buckets_;  // <5 <10 <20 <50 <100 <200 >=200
        stats["max_latency_ms"] = max_latency_us_ / 1000.0;
        stats["picks_by_position"] = picks_by_position_;
        stats["picks_with_recommendation"] = picks_with_recommendation_;
        stats["recommendation_picked"] = recommendation_picked_;
        stats["laya_calls"] = laya.calls - laya_calls_at_window_start_;
        stats["laya_reordered"] = laya.reordered - laya_reordered_at_window_start_;
        stats["laya_last_ms"] = laya.last_ms;
        write_input_line_locked(stats.dump());
    }
    reset_window_locked();
}

void ExperienceLog::write_line_locked(const std::wstring& path, const std::string& line) {
    if (directory_.empty()) return;
    if (file_size(path) >= kMaxExperienceBytes) {
        MoveFileExW(path.c_str(), (directory_ + L"\\" + kExperienceOldFile).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return;
    std::fwrite(line.data(), 1, line.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
}

void ExperienceLog::write_input_line_locked(const std::string& line) {
    if (directory_.empty()) return;
    const std::wstring today = std::wstring(kInputPrefix) + utc_day() + L".jsonl";
    const std::wstring oldest_kept = std::wstring(kInputPrefix) + utc_day(kInputDays - 1) + L".jsonl";
    // Keep 7 days and 10 MB in all: older days go first; today's file stops growing at the cap.
    auto files = input_files(directory_);
    std::uint64_t total = 0;
    for (const auto& [name, size] : files) total += size;
    for (const auto& [name, size] : files) {
        if (name == today) continue;
        if (name < oldest_kept || total + line.size() > kMaxInputBytes) {
            if (DeleteFileW((directory_ + L"\\" + name).c_str())) total -= size;
        }
    }
    if (total + line.size() + 1 > kMaxInputBytes) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, (directory_ + L"\\" + today).c_str(), L"ab") != 0 || !file) return;
    std::fwrite(line.data(), 1, line.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
}

void ExperienceLog::delete_all_logs(const std::wstring& directory) {
    const std::wstring dir = directory.empty() ? diagnostic_log_directory() : directory;
    if (dir.empty()) return;
    for (const wchar_t* name : {kExperienceFile, kExperienceOldFile}) {
        DeleteFileW((dir + L"\\" + name).c_str());
    }
    for (const auto& [name, size] : input_files(dir)) {
        DeleteFileW((dir + L"\\" + name).c_str());
    }
}

}  // namespace cxxime
