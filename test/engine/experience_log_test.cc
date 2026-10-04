// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

// User experience improvement program: two opt-in tiers. The first logs versions, settings
// and health only; the second (which needs the first) adds statistics and the input itself.

#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include <json.hpp>

#include <cxxime/config.h>
#include <cxxime/engine.h>
#include <cxxime/experience_log.h>

#include "support/testutil.h"

namespace {

std::wstring temp_directory() {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    std::wstring path = std::wstring(directory) + L"zhiyi_experience_test";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

std::vector<std::wstring> files_in(const std::wstring& directory) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW((directory + L"\\*.jsonl").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        names.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return names;
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

std::wstring input_file(const std::wstring& directory) {
    for (const std::wstring& name : files_in(directory)) {
        if (name.rfind(L"input-", 0) == 0) return directory + L"\\" + name;
    }
    return {};
}

cxxime::InputRecord sample_input() {
    cxxime::InputRecord input;
    input.app = "notepad.exe";
    input.window_title = "a.txt - Notepad";
    input.mode = "pinyin";
    input.keys = {"n", "i", "SPACE"};
    input.code = "ni";
    input.candidates = {{"你", true}, {"呢", false}};
    input.picked = 0;
    input.committed = "你";
    input.laya_context = "我说";
    return input;
}

}  // namespace

TEST(ExperienceLog, tiers_control_what_is_logged_and_one_call_deletes_everything) {
    const std::wstring dir = temp_directory();
    cxxime::ExperienceLog::delete_all_logs(dir);
    cxxime::ExperienceLog& log = cxxime::ExperienceLog::instance();
    log.set_directory_for_testing(dir);

    // Off: nothing at all.
    cxxime::Config off;
    log.configure(off);
    log.record_key(3000);
    log.record_error();
    log.record_input(sample_input());
    log.flush();
    ASSERT_TRUE(files_in(dir).empty());
    ASSERT_TRUE(!log.collecting_input());

    // Input collection alone does nothing: it needs the first tier.
    cxxime::Config input_only;
    input_only.collect_input = true;
    log.configure(input_only);
    ASSERT_TRUE(!log.collecting_input());
    log.record_input(sample_input());
    ASSERT_TRUE(files_in(dir).empty());

    // First tier: versions, settings and health; no statistics, no input.
    cxxime::Config basic;
    basic.experience_program = true;
    log.configure(basic);
    log.record_key(3000);
    log.record_error();
    cxxime::CandidatePick pick;
    pick.index = 1;
    log.record_pick(pick);
    log.record_input(sample_input());
    log.flush();
    ASSERT_EQ(files_in(dir).size(), 1u);
    auto lines = read_lines(dir + L"\\experience.jsonl");
    ASSERT_EQ(lines.size(), 3u);
    ASSERT_TRUE(lines[0]["event"] == "start");
    ASSERT_TRUE(lines[1]["event"] == "config");
    ASSERT_TRUE(lines[1]["settings"]["collect_input"] == false);
    ASSERT_TRUE(lines[2]["event"] == "health");
    ASSERT_EQ(lines[2]["errors"].get<int>(), 1);
    ASSERT_TRUE(input_file(dir).empty());

    // Second tier: statistics and inputs go to the input log.
    cxxime::Config full = basic;
    full.collect_input = true;
    log.configure(full);
    ASSERT_TRUE(log.collecting_input());
    log.record_key(30000);
    pick.index = 0;
    pick.recommended = true;
    pick.had_recommendation = true;
    log.record_pick(pick);
    log.record_input(sample_input());
    log.flush();
    const std::wstring input_path = input_file(dir);
    ASSERT_TRUE(!input_path.empty());
    lines = read_lines(input_path);
    ASSERT_EQ(lines.size(), 2u);
    ASSERT_TRUE(lines[0]["event"] == "input");
    ASSERT_TRUE(lines[0]["app"] == "notepad.exe");
    ASSERT_TRUE(lines[0]["committed"] == "你");
    ASSERT_TRUE(lines[0]["keys"].size() == 3);
    ASSERT_TRUE(lines[0]["candidates"][0]["recommended"] == true);
    ASSERT_TRUE(lines[0]["laya_context"] == "我说");
    ASSERT_TRUE(lines[1]["event"] == "stats");
    ASSERT_EQ(lines[1]["keys"].get<int>(), 1);
    ASSERT_EQ(lines[1]["recommendation_picked"].get<int>(), 1);
    // The settings change is in the first tier's log.
    ASSERT_TRUE(read_lines(dir + L"\\experience.jsonl").back()["settings"]["collect_input"] ==
                true);

    // Revoking the second tier stops it and keeps the files.
    log.configure(basic);
    ASSERT_TRUE(!log.collecting_input());
    log.record_input(sample_input());
    ASSERT_EQ(read_lines(input_path).size(), 2u);

    cxxime::ExperienceLog::delete_all_logs(dir);
    ASSERT_TRUE(files_in(dir).empty());
    log.configure(off);
    RemoveDirectoryW(dir.c_str());
}

RUN_ALL_TESTS()
