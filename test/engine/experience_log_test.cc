// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

// User experience improvement program log: off by default, and when on it holds settings and
// counts only.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include <json.hpp>

#include <cxxime/config.h>
#include <cxxime/engine.h>
#include <cxxime/experience_log.h>

#include "support/testutil.h"

namespace {

std::wstring temp_log() {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + L"zhiyi_experience_test.jsonl";
}

std::vector<nlohmann::json> read_lines(const std::wstring& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) lines.push_back(nlohmann::json::parse(line));
    }
    return lines;
}

}  // namespace

TEST(ExperienceLog, writes_nothing_while_off_and_counts_only_while_on) {
    const std::wstring path = temp_log();
    DeleteFileW(path.c_str());
    cxxime::ExperienceLog& log = cxxime::ExperienceLog::instance();
    log.set_path_for_testing(path);

    cxxime::Config off;
    log.configure(off);
    log.record_key(3000);
    log.flush();
    ASSERT_TRUE(read_lines(path).empty());

    cxxime::Config on;
    on.experience_program = true;
    log.configure(on);
    log.record_key(3000);    // < 5 ms
    log.record_key(30000);   // < 50 ms
    log.record_error();
    cxxime::CandidatePick pick;
    pick.index = 0;
    pick.recommended = true;
    pick.had_recommendation = true;
    log.record_pick(pick);
    pick.index = 2;
    pick.recommended = false;
    log.record_pick(pick);
    log.flush();

    const auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), 3u);
    ASSERT_TRUE(lines[0]["event"] == "start");
    ASSERT_TRUE(lines[0].contains("version"));
    ASSERT_TRUE(lines[1]["event"] == "config");
    ASSERT_TRUE(lines[1]["settings"]["input_mode"] == "pinyin");
    const nlohmann::json& stats = lines[2];
    ASSERT_TRUE(stats["event"] == "stats");
    ASSERT_EQ(stats["keys"].get<int>(), 2);
    ASSERT_EQ(stats["errors"].get<int>(), 1);
    ASSERT_EQ(stats["latency_ms_buckets"][0].get<int>(), 1);
    ASSERT_EQ(stats["latency_ms_buckets"][3].get<int>(), 1);
    ASSERT_EQ(stats["picks_by_position"][0].get<int>(), 1);
    ASSERT_EQ(stats["picks_by_position"][2].get<int>(), 1);
    ASSERT_EQ(stats["picks_with_recommendation"].get<int>(), 2);
    ASSERT_EQ(stats["recommendation_picked"].get<int>(), 1);

    // The same settings again: no new "config" record; turning it off writes nothing more.
    log.configure(on);
    log.configure(off);
    ASSERT_EQ(read_lines(path).size(), 3u);
    DeleteFileW(path.c_str());
}

RUN_ALL_TESTS()
