// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "config_store.h"

#include <cstdint>
#include <string>
#include <utility>

#include <windows.h>

#include <json.hpp>

#include <cxxime/problem_log.h>

namespace {

void set_failure(ConfigStoreFailure* failure, ConfigStoreFailure value) {
    if (failure) {
        *failure = value;
    }
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), &result[0], size) != size) {
        return {};
    }
    return result;
}

bool read_file(const std::wstring& path, std::string* contents, unsigned long* error_code) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_code) {
            *error_code = GetLastError();
        }
        return false;
    }

    LARGE_INTEGER size = {};
    bool succeeded = GetFileSizeEx(file, &size) != FALSE;
    unsigned long result = succeeded ? ERROR_SUCCESS : GetLastError();
    if (succeeded &&
        (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(cxxime::CONTROL_MAX_PAYLOAD))) {
        succeeded = false;
        result = ERROR_FILE_TOO_LARGE;
    }
    if (succeeded) {
        contents->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        succeeded =
            contents->empty() || (ReadFile(file, &(*contents)[0],
                                           static_cast<DWORD>(contents->size()), &read, nullptr) &&
                                  read == static_cast<DWORD>(contents->size()));
        result = succeeded ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(file);
    if (error_code) {
        *error_code = result;
    }
    return succeeded;
}

bool atomic_write_file(const std::wstring& path, const std::string& contents,
                       unsigned long* error_code) {
    const std::wstring temporary_path = path + L".tmp";
    HANDLE file = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error_code) {
            *error_code = GetLastError();
        }
        return false;
    }

    DWORD written = 0;
    bool succeeded =
        contents.empty() ||
        (WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
         written == static_cast<DWORD>(contents.size()));
    if (succeeded) {
        succeeded = FlushFileBuffers(file) != FALSE;
    }
    unsigned long result = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);

    if (succeeded) {
        succeeded = MoveFileExW(temporary_path.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        result = succeeded ? ERROR_SUCCESS : GetLastError();
    }
    if (!succeeded) {
        DeleteFileW(temporary_path.c_str());
    }
    if (error_code) {
        *error_code = result;
    }
    return succeeded;
}

// Before 1.5.16 the package turned on the fuzzy pairs en=eng and in=ing; now none are on. Someone
// who used an earlier version (settings or learned words in the user folder) and never chose
// the pairs keeps the old ones: they are written into the user's settings once.
bool keep_earlier_fuzzy_pairs(const std::wstring& user_path, nlohmann::json* user_config) {
    if (user_config->contains("engine") && (*user_config)["engine"].is_object() &&
        (*user_config)["engine"].contains("fuzzy_groups")) {
        return false;
    }
    const std::wstring folder = user_path.substr(0, user_path.find_last_of(L"\\/") + 1);
    bool earlier_user = false;
    for (const wchar_t* name : {L"default.json", L"learning_pinyin.tsv", L"user_pinyin.tsv",
                                L"learning_english.json", L"input_state.json"}) {
        earlier_user = earlier_user ||
                       GetFileAttributesW((folder + name).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!earlier_user) {
        return false;
    }
    if (!(*user_config)["engine"].is_object()) {
        (*user_config)["engine"] = nlohmann::json::object();
    }
    (*user_config)["engine"]["fuzzy_groups"] = nlohmann::json::array({"en_eng", "in_ing"});
    return true;
}

} // namespace

bool ConfigStore::initialize(const std::string& base_config_path,
                             const std::string& user_config_path, const std::string& themes_path,
                             std::shared_ptr<const cxxime::Config>* config,
                             unsigned long* error_code, ConfigStoreFailure* failure) {
    set_failure(failure, ConfigStoreFailure::kNone);
    if (!config || base_config_path.empty() || user_config_path.empty() || themes_path.empty()) {
        if (error_code) {
            *error_code = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    std::wstring user_path = utf8_to_wide(user_config_path);
    if (user_path.empty()) {
        set_failure(failure, ConfigStoreFailure::kUserConfig);
        if (error_code) {
            *error_code = ERROR_NO_UNICODE_TRANSLATION;
        }
        return false;
    }

    std::string user_json = "{}";
    DWORD attributes = GetFileAttributesW(user_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !read_file(user_path, &user_json, error_code)) {
            set_failure(failure, ConfigStoreFailure::kUserConfig);
            return false;
        }
    } else {
        unsigned long attributes_error = GetLastError();
        if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND) {
            if (error_code) {
                *error_code = attributes_error;
            }
            set_failure(failure, ConfigStoreFailure::kUserConfig);
            return false;
        }
    }

    try {
        nlohmann::json parsed = nlohmann::json::parse(user_json);
        if (!parsed.is_object()) {
            set_failure(failure, ConfigStoreFailure::kUserConfig);
            if (error_code) {
                *error_code = ERROR_INVALID_DATA;
            }
            return false;
        }
        const bool kept_fuzzy_pairs = keep_earlier_fuzzy_pairs(user_path, &parsed);
        user_json = parsed.dump(4);
        unsigned long write_error = ERROR_SUCCESS;
        if (kept_fuzzy_pairs && !atomic_write_file(user_path, user_json + "\n", &write_error)) {
            cxxime::log_problem("config", "cannot write " + user_config_path + ": " +
                                              cxxime::windows_error_text(write_error));
        }
    } catch (const nlohmann::json::exception&) {
        set_failure(failure, ConfigStoreFailure::kUserConfig);
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }

    base_config_path_ = base_config_path;
    user_config_path_ = user_config_path;
    themes_path_ = themes_path;
    if (!build_effective_config(user_json, config, error_code, failure)) {
        return false;
    }
    user_config_json_ = std::move(user_json);
    if (error_code) {
        *error_code = ERROR_SUCCESS;
    }
    return true;
}

bool ConfigStore::prepare_update(const std::vector<ConfigMutation>& mutations,
                                PreparedConfigUpdate* update, unsigned long* error_code) {
    if (!update || mutations.empty()) {
        if (error_code) {
            *error_code = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    nlohmann::json user_config;
    try {
        user_config = nlohmann::json::parse(user_config_json_);
        for (const auto& mutation : mutations) {
            nlohmann::json payload = nlohmann::json::parse(mutation.payload);
            if (mutation.kind == cxxime::UserConfigMutationKind::kReplace) {
                user_config = std::move(payload);
            } else {
                user_config.merge_patch(payload);
            }
            if (!user_config.is_object()) {
                cxxime::log_problem("config", "the new settings are not a JSON object");
                if (error_code) {
                    *error_code = ERROR_INVALID_DATA;
                }
                return false;
            }
        }
    } catch (const nlohmann::json::exception& e) {
        cxxime::log_problem("config", std::string("the new settings are not valid JSON: ") + e.what());
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }

    std::string serialized = user_config.dump(4);
    PreparedConfigUpdate prepared;
    ConfigStoreFailure failure = ConfigStoreFailure::kNone;
    if (!build_effective_config(serialized, &prepared.config, error_code, &failure)) {
        cxxime::log_problem("config",
                            failure == ConfigStoreFailure::kBaseConfig
                                ? "cannot read the package settings " + base_config_path_
                            : failure == ConfigStoreFailure::kThemes
                                ? "cannot read the themes " + themes_path_
                                : std::string("the new settings were not accepted"));
        return false;
    }

    prepared.user_config_json = std::move(serialized);
    *update = std::move(prepared);
    if (error_code) {
        *error_code = ERROR_SUCCESS;
    }
    return true;
}

bool ConfigStore::commit_update(const PreparedConfigUpdate& update, unsigned long* error_code) {
    if (!update.config || update.user_config_json.empty()) {
        if (error_code) {
            *error_code = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    if (update.user_config_json != user_config_json_) {
        std::wstring user_path = utf8_to_wide(user_config_path_);
        if (user_path.empty()) {
            cxxime::log_problem("config", "the settings file path is not valid UTF-8");
            if (error_code) {
                *error_code = ERROR_NO_UNICODE_TRANSLATION;
            }
            return false;
        }
        unsigned long write_error = ERROR_SUCCESS;
        if (!atomic_write_file(user_path, update.user_config_json + "\n", &write_error)) {
            cxxime::log_problem("config", "cannot write " + user_config_path_ + ": " +
                                              cxxime::windows_error_text(write_error));
            if (error_code) {
                *error_code = write_error;
            }
            return false;
        }
        user_config_json_ = update.user_config_json;
    }

    if (error_code) {
        *error_code = ERROR_SUCCESS;
    }
    return true;
}

bool ConfigStore::build_effective_config(const std::string& user_config_json,
                                         std::shared_ptr<const cxxime::Config>* config,
                                         unsigned long* error_code,
                                         ConfigStoreFailure* failure) const {
    auto candidate = std::make_shared<cxxime::Config>();
    if (!candidate->load(base_config_path_)) {
        set_failure(failure, ConfigStoreFailure::kBaseConfig);
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }
    if (!candidate->load_user_json(user_config_json)) {
        set_failure(failure, ConfigStoreFailure::kUserConfig);
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }
    if (!candidate->load_themes(themes_path_)) {
        set_failure(failure, ConfigStoreFailure::kThemes);
        if (error_code) {
            *error_code = ERROR_INVALID_DATA;
        }
        return false;
    }
    set_failure(failure, ConfigStoreFailure::kNone);
    *config = std::move(candidate);
    return true;
}
