// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/installer_ui_language.h>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>

#include <json.hpp>

namespace cxxime {
namespace installer {

namespace {

constexpr char kAuto[] = "auto";

bool read_config(const std::wstring& path, nlohmann::json* config, bool* missing) {
    *missing = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        *missing = attributes == INVALID_FILE_ATTRIBUTES &&
                   (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
        if (*missing) *config = nlohmann::json::object();
        return *missing;
    }
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (contents.find_first_not_of(" \t\r\n") == std::string::npos) {
        *config = nlohmann::json::object();
        return true;
    }
    *config = nlohmann::json::parse(contents, nullptr, false);
    return !config->is_discarded() && config->is_object();
}

std::string language_of(const nlohmann::json& config) {
    if (config.contains("ui") && config["ui"].is_object() && config["ui"].contains("language") &&
        config["ui"]["language"].is_string()) {
        const std::string value = config["ui"]["language"].get<std::string>();
        return value.empty() ? kAuto : value;
    }
    return kAuto;
}

bool write_config(const std::wstring& path, const nlohmann::json& config) {
    // %USERPROFILE%\zhiyi may not exist yet on a first install.
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    const std::wstring temp = path + L".installer.tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << config.dump(4) << "\n";
        if (!file.flush()) {
            file.close();
            DeleteFileW(temp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::string narrow(const wchar_t* text) {
    std::string out;
    for (const wchar_t* p = text; p && *p; ++p) out += *p < 0x80 ? static_cast<char>(*p) : '?';
    return out;
}

}  // namespace

std::string ui_language_for_langid(unsigned short langid) {
    return PRIMARYLANGID(langid) == LANG_CHINESE ? "zh-CN" : "en-US";
}

std::string read_ui_language(const std::wstring& user_config_path) {
    nlohmann::json config;
    bool missing = false;
    if (!read_config(user_config_path, &config, &missing)) return {};
    return language_of(config);
}

bool write_ui_language(const std::wstring& user_config_path, const std::string& code,
                       const std::string& system_code) {
    nlohmann::json config;
    bool missing = false;
    if (!read_config(user_config_path, &config, &missing)) return false;
    const std::string current = language_of(config);
    if (current == code || (current == kAuto && code == system_code)) return true;
    if (!config.contains("ui") || !config["ui"].is_object()) config["ui"] = nlohmann::json::object();
    config["ui"]["language"] = code;
    return write_config(user_config_path, config);
}

namespace {

std::string privacy_answer(const nlohmann::json& config, const char* key) {
    if (config.contains("privacy") && config["privacy"].is_object() &&
        config["privacy"].contains(key) && config["privacy"][key].is_boolean()) {
        return config["privacy"][key].get<bool>() ? "on" : "off";
    }
    return "unset";
}

}  // namespace

PrivacyAnswers read_privacy(const std::wstring& user_config_path) {
    nlohmann::json config;
    bool missing = false;
    if (!read_config(user_config_path, &config, &missing)) return {};
    return {privacy_answer(config, "experience_program"), privacy_answer(config, "collect_input")};
}

bool write_privacy(const std::wstring& user_config_path, bool join, bool collect_input) {
    nlohmann::json config;
    bool missing = false;
    if (!read_config(user_config_path, &config, &missing)) return false;
    collect_input = collect_input && join;
    bool changed = false;
    for (const auto& [key, value] : {std::pair{"experience_program", join},
                                     std::pair{"collect_input", collect_input}}) {
        const std::string current = privacy_answer(config, key);
        if (current == (value ? "on" : "off") || (!value && current == "unset")) continue;
        if (!config.contains("privacy") || !config["privacy"].is_object()) {
            config["privacy"] = nlohmann::json::object();
        }
        config["privacy"][key] = value;
        changed = true;
    }
    return !changed || write_config(user_config_path, config);
}

int get_privacy_command(const wchar_t* user_config_path) {
    const PrivacyAnswers answers = read_privacy(user_config_path);
    if (answers.experience_program.empty()) return 1;
    std::fputs((answers.experience_program + " " + answers.collect_input).c_str(), stdout);
    return 0;
}

int set_privacy_command(const wchar_t* user_config_path, const wchar_t* join,
                        const wchar_t* collect_input) {
    const std::string program = narrow(join);
    const std::string input = narrow(collect_input);
    for (const std::string& answer : {program, input}) {
        if (answer != "on" && answer != "off") return 64;
    }
    return write_privacy(user_config_path, program == "on", input == "on") ? 0 : 1;
}

int get_ui_language_command(const wchar_t* user_config_path) {
    const std::string language = read_ui_language(user_config_path);
    if (language.empty()) return 1;
    std::fputs(language.c_str(), stdout);
    return 0;
}

int set_ui_language_command(const wchar_t* user_config_path, const wchar_t* code) {
    const std::string language = narrow(code);
    if (language != "zh-CN" && language != "en-US") return 64;
    const std::string system = ui_language_for_langid(GetUserDefaultUILanguage());
    return write_ui_language(user_config_path, language, system) ? 0 : 1;
}

}  // namespace installer
}  // namespace cxxime
