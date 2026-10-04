// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The installer language chosen in the setup language dialog becomes the settings language
// (user config ui.language), so the settings app speaks the language the user installed in.
#ifndef CXXIME_INSTALLER_UI_LANGUAGE_H_
#define CXXIME_INSTALLER_UI_LANGUAGE_H_

#include <string>

namespace cxxime {
namespace installer {

// Settings language for a Windows language id: Chinese -> "zh-CN", anything else -> "en-US"
// (as the settings app resolves "auto").
std::string ui_language_for_langid(unsigned short langid);

// ui.language of the user config: its value, "auto" when the file or the value is missing,
// empty when the file cannot be read or is not a JSON object.
std::string read_ui_language(const std::wstring& user_config_path);

// Sets ui.language to `code` ("zh-CN" / "en-US"). A config that follows the system ("auto" or
// unset) is left alone when `code` is what the system language gives anyway (`system_code`).
// Other settings are kept; an unreadable config is not touched (returns false).
bool write_ui_language(const std::wstring& user_config_path, const std::string& code,
                       const std::string& system_code);

// Command line: "get-ui-language <config>" prints the value; "set-ui-language <config> <code>".
int get_ui_language_command(const wchar_t* user_config_path);
int set_ui_language_command(const wchar_t* user_config_path, const wchar_t* code);

// The user experience improvement program answers of the setup: privacy.experience_program
// and privacy.collect_input (which needs the first). Each "on", "off", or "unset" (no answer
// yet); empty when the config cannot be read.
struct PrivacyAnswers {
    std::string experience_program;
    std::string collect_input;
};
PrivacyAnswers read_privacy(const std::wstring& user_config_path);
// Other settings are kept; an unreadable config is not touched (returns false). Unset values
// stay unset when the answer is "off" (the default), so a first "no" writes no file.
bool write_privacy(const std::wstring& user_config_path, bool join, bool collect_input);
// "get-privacy <config>" prints "<experience> <input>" (e.g. "on off");
// "set-privacy <config> on|off on|off" (input collection only together with the program).
int get_privacy_command(const wchar_t* user_config_path);
int set_privacy_command(const wchar_t* user_config_path, const wchar_t* join,
                        const wchar_t* collect_input);

}  // namespace installer
}  // namespace cxxime

#endif  // CXXIME_INSTALLER_UI_LANGUAGE_H_
