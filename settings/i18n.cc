// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "i18n.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

using StringTable = std::unordered_map<std::string, std::wstring>;

StringTable g_strings;   // selected language
StringTable g_fallback;  // kFallbackUiLanguage

std::string ui_file_path(const std::string& code) {
    return cxxime::data_path(("ui." + code + ".json").c_str());
}

bool read_table(const std::string& code, StringTable* table) {
    table->clear();
    std::ifstream file(utf8_to_wstr(ui_file_path(code)), std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (!json.is_object()) {
        return false;
    }
    for (const auto& [key, value] : json.items()) {
        if (value.is_string()) {
            (*table)[key] = utf8_to_wstr(value.get<std::string>());
        }
    }
    return true;
}

// "zh-CN" from LCID 2052, "en-US" from 1033 ...
std::string windows_ui_language() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name,
                         LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        return wstr_to_utf8(name);
    }
    return kFallbackUiLanguage;
}

std::string primary_subtag(const std::string& code) {
    return code.substr(0, code.find('-'));
}

} // namespace

std::vector<UiLanguage> available_ui_languages() {
    std::vector<UiLanguage> languages;
    const std::wstring pattern = utf8_to_wstr(cxxime::data_path("ui.*.json"));
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::string file = wstr_to_utf8(entry.cFileName);  // ui.<code>.json
            if (file.size() <= 8) {
                continue;
            }
            const std::string code = file.substr(3, file.size() - 8);
            StringTable table;
            if (read_table(code, &table)) {
                const auto name = table.find("_language_name");
                languages.push_back(
                    {code, name != table.end() ? name->second : utf8_to_wstr(code)});
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    std::sort(languages.begin(), languages.end(), [](const UiLanguage& a, const UiLanguage& b) {
        if ((a.code == kDefaultUiLanguage) != (b.code == kDefaultUiLanguage)) {
            return a.code == kDefaultUiLanguage;
        }
        return a.code < b.code;
    });
    return languages;
}

std::string resolve_ui_language(const std::string& setting) {
    const std::vector<UiLanguage> languages = available_ui_languages();
    auto available = [&](const std::string& code) {
        return std::any_of(languages.begin(), languages.end(),
                           [&](const UiLanguage& language) { return language.code == code; });
    };
    if (setting != kAutoUiLanguage && available(setting)) {
        return setting;
    }
    const std::string system = windows_ui_language();
    if (available(system)) {
        return system;
    }
    // Same language, other region (zh-TW -> zh-CN, en-GB -> en-US).
    for (const UiLanguage& language : languages) {
        if (primary_subtag(language.code) == primary_subtag(system)) {
            return language.code;
        }
    }
    return kFallbackUiLanguage;
}

void load_ui_strings(const std::string& code) {
    read_table(kFallbackUiLanguage, &g_fallback);
    if (code == kFallbackUiLanguage || !read_table(code, &g_strings)) {
        g_strings = g_fallback;
    }
}

const wchar_t* tr(const char* key) {
    auto found = g_strings.find(key);
    if (found != g_strings.end()) {
        return found->second.c_str();
    }
    found = g_fallback.find(key);
    if (found != g_fallback.end()) {
        return found->second.c_str();
    }
    // Unknown key: show the key itself so the gap is visible.
    static std::unordered_map<std::string, std::wstring> missing;
    auto& text = missing[key];
    if (text.empty()) {
        text = utf8_to_wstr(key);
    }
    return text.c_str();
}

} // namespace settings
} // namespace cxxime
