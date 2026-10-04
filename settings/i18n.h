// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Settings UI strings. Each language is a flat UTF-8 JSON file data/ui.<code>.json
// ({"key": "text", ...}, plus "_language_name"); adding a language only needs a new file.
// Keys missing from the selected language fall back to English, then to the key itself.
// "Follow the system" picks the Windows UI language when a file exists for it (any region of
// the same language), and English otherwise.

#ifndef CXXIME_SETTINGS_I18N_H_
#define CXXIME_SETTINGS_I18N_H_

#include <string>
#include <vector>

namespace cxxime {
namespace settings {

inline constexpr char kDefaultUiLanguage[] = "zh-CN";  // listed first
inline constexpr char kFallbackUiLanguage[] = "en-US";
inline constexpr char kAutoUiLanguage[] = "auto";

struct UiLanguage {
    std::string code;    // "zh-CN", "en-US"
    std::wstring name;   // its own name: "简体中文", "English"
};

// Languages with a data/ui.<code>.json file, the default language first.
std::vector<UiLanguage> available_ui_languages();

// "auto" (or an unavailable code) -> the Windows UI language if available, else English.
std::string resolve_ui_language(const std::string& setting);

// Loads the strings of `code` (already resolved).
void load_ui_strings(const std::string& code);

// Translated text for `key`; the pointer stays valid until the next load_ui_strings().
const wchar_t* tr(const char* key);

} // namespace settings
} // namespace cxxime

#endif // CXXIME_SETTINGS_I18N_H_
