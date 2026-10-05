// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "user_backup_service.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/user_backup.h>
#include <cxxime/user_backup_control.h>

#include "config_write_coordinator.h"
#include "session_manager.h"

namespace {

struct UserDataMapping {
    const char* archive_path;
    const char* file_name;
    cxxime::UserBackupComponent component;
};

constexpr UserDataMapping kUserDataMappings[] = {
    {"lexicon/user_pinyin.tsv", "user_pinyin.tsv", cxxime::UserBackupComponent::kUserLexicon},
    {"lexicon/user_wubi.tsv", "user_wubi.tsv", cxxime::UserBackupComponent::kUserLexicon},
    {"ranking/candidate_order_pinyin.tsv", "candidate_order_pinyin.tsv",
     cxxime::UserBackupComponent::kCandidateOrder},
    {"ranking/candidate_order_wubi.tsv", "candidate_order_wubi.tsv",
     cxxime::UserBackupComponent::kCandidateOrder},
    {"learning/learning_pinyin.tsv", "learning_pinyin.tsv", cxxime::UserBackupComponent::kLearning},
    {"learning/learning_wubi.tsv", "learning_wubi.tsv", cxxime::UserBackupComponent::kLearning},
    {"learning/learning_composition.tsv", "learning_composition.tsv",
     cxxime::UserBackupComponent::kLearning},
    {"disabled/disabled_pinyin.tsv", "disabled_pinyin.tsv",
     cxxime::UserBackupComponent::kDisabledSystemLexicon},
    {"disabled/disabled_wubi.tsv", "disabled_wubi.tsv",
     cxxime::UserBackupComponent::kDisabledSystemLexicon},
};

bool includes(std::uint32_t components, cxxime::UserBackupComponent component) {
    return (components & cxxime::user_backup_component_flag(component)) != 0;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), &result[0], count) == count
               ? result
               : std::wstring();
}

std::wstring full_path(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }
    std::wstring result(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, &result[0], nullptr);
    if (written == 0 || written >= required) {
        return {};
    }
    result.resize(written);
    return result;
}

bool export_path_overwrites_user_data(const std::wstring& path) {
    const std::wstring destination = full_path(path);
    const std::wstring config = full_path(utf8_to_wide(cxxime::user_data_path("default.json")));
    if (destination.empty() ||
        (!config.empty() && _wcsicmp(destination.c_str(), config.c_str()) == 0)) {
        return true;
    }
    return std::any_of(std::begin(kUserDataMappings), std::end(kUserDataMappings),
                       [&](const UserDataMapping& mapping) {
                           const std::wstring managed =
                               full_path(utf8_to_wide(cxxime::user_data_path(mapping.file_name)));
                           return !managed.empty() &&
                                  _wcsicmp(destination.c_str(), managed.c_str()) == 0;
                       });
}

nlohmann::json take_device_settings(nlohmann::json* config) {
    nlohmann::json device = nlohmann::json::object();
    if (config->contains("diagnostics")) {
        device["diagnostics"] = (*config)["diagnostics"];
        config->erase("diagnostics");
    }
    // Backups before 0.7.5 also kept the floating status window position here; ignored.
    config->erase("status_window");
    return device;
}

bool split_config(const std::string& config_json, std::string* portable, std::string* device) {
    try {
        nlohmann::json config = nlohmann::json::parse(config_json);
        if (!config.is_object()) {
            return false;
        }
        const nlohmann::json device_config = take_device_settings(&config);
        *portable = config.dump(4) + "\n";
        *device = device_config.dump(4) + "\n";
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

const cxxime::UserBackupEntry* find_entry(const cxxime::UserBackupArchive& archive,
                                          const char* path) {
    const auto found =
        std::find_if(archive.entries.begin(), archive.entries.end(),
                     [&](const cxxime::UserBackupEntry& entry) { return entry.path == path; });
    return found == archive.entries.end() ? nullptr : &*found;
}

bool make_config_patch(const cxxime::UserBackupArchive& archive, std::uint32_t components,
                       std::string* patch) {
    try {
        nlohmann::json imported = nlohmann::json::object();
        if (includes(components, cxxime::UserBackupComponent::kSettings)) {
            const auto* entry = find_entry(archive, "config/settings.json");
            if (!entry) {
                return false;
            }
            nlohmann::json portable = nlohmann::json::parse(entry->contents);
            if (!portable.is_object()) {
                return false;
            }
            take_device_settings(&portable);
            imported.merge_patch(portable);
        }
        if (includes(components, cxxime::UserBackupComponent::kDeviceSettings)) {
            const auto* entry = find_entry(archive, "config/device.json");
            if (!entry) {
                return false;
            }
            nlohmann::json imported_device = nlohmann::json::parse(entry->contents);
            if (!imported_device.is_object()) {
                return false;
            }
            imported.merge_patch(take_device_settings(&imported_device));
        }
        *patch = imported.dump();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::vector<cxxime::UserBackupEntry>
make_entries(const std::string& config_json, const std::map<std::string, std::string>& user_data,
             std::uint32_t components) {
    std::vector<cxxime::UserBackupEntry> entries;
    std::string portable;
    std::string device;
    if (includes(components, cxxime::UserBackupComponent::kSettings) ||
        includes(components, cxxime::UserBackupComponent::kDeviceSettings)) {
        if (!split_config(config_json, &portable, &device)) {
            return {};
        }
        if (includes(components, cxxime::UserBackupComponent::kSettings)) {
            entries.push_back({cxxime::UserBackupComponent::kSettings, "config/settings.json",
                               std::move(portable)});
        }
        if (includes(components, cxxime::UserBackupComponent::kDeviceSettings)) {
            entries.push_back({cxxime::UserBackupComponent::kDeviceSettings, "config/device.json",
                               std::move(device)});
        }
    }
    for (const UserDataMapping& mapping : kUserDataMappings) {
        if (!includes(components, mapping.component)) {
            continue;
        }
        const auto contents = user_data.find(mapping.file_name);
        if (contents == user_data.end()) {
            return {};
        }
        entries.push_back({mapping.component, mapping.archive_path, contents->second});
    }
    return entries;
}

bool required_entries_present(const cxxime::UserBackupArchive& archive, std::uint32_t components) {
    if (includes(components, cxxime::UserBackupComponent::kSettings) &&
        !find_entry(archive, "config/settings.json")) {
        return false;
    }
    if (includes(components, cxxime::UserBackupComponent::kDeviceSettings) &&
        !find_entry(archive, "config/device.json")) {
        return false;
    }
    return std::all_of(std::begin(kUserDataMappings), std::end(kUserDataMappings),
                       [&](const UserDataMapping& mapping) {
                           return !includes(components, mapping.component) ||
                                  find_entry(archive, mapping.archive_path);
                       });
}

std::map<std::string, std::string> selected_user_data(const cxxime::UserBackupArchive& archive,
                                                      std::uint32_t components) {
    std::map<std::string, std::string> files;
    for (const UserDataMapping& mapping : kUserDataMappings) {
        if (!includes(components, mapping.component)) {
            continue;
        }
        const auto* entry = find_entry(archive, mapping.archive_path);
        if (entry) {
            files[mapping.file_name] = entry->contents;
        }
    }
    return files;
}

std::vector<std::string> selected_user_data_names(std::uint32_t components) {
    std::vector<std::string> names;
    for (const UserDataMapping& mapping : kUserDataMappings) {
        if (includes(components, mapping.component)) {
            names.push_back(mapping.file_name);
        }
    }
    return names;
}

} // namespace

UserBackupService::UserBackupService(SessionManager* session_manager,
                                     ConfigWriteCoordinator* config_writer)
    : session_manager_(session_manager)
    , config_writer_(config_writer) {}

bool UserBackupService::handle_request(const std::string& payload, std::string* response_payload) {
    cxxime::UserBackupControlRequest request;
    if (!response_payload || !cxxime::decode_user_backup_request(payload, &request)) {
        return false;
    }
    cxxime::UserBackupControlResult result;
    result.operation = request.operation;
    unsigned long error = ERROR_INVALID_DATA;
    const std::wstring path = utf8_to_wide(request.path);
    if (path.empty() || !session_manager_ || !config_writer_) {
        result.error_code = path.empty() ? ERROR_NO_UNICODE_TRANSLATION : ERROR_INVALID_HANDLE;
        return cxxime::encode_user_backup_result(result, response_payload);
    }

    if (request.operation == cxxime::UserBackupOperation::kInspect) {
        cxxime::UserBackupArchive archive;
        result.succeeded = cxxime::read_user_backup_archive(path, &archive, &error);
        result.error_code = result.succeeded
                                ? ERROR_SUCCESS
                                : (error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
        if (result.succeeded) {
            result.summary = archive.summary;
        }
        return cxxime::encode_user_backup_result(result, response_payload);
    }

    if (request.operation == cxxime::UserBackupOperation::kExport) {
        if (export_path_overwrites_user_data(path)) {
            result.error_code = ERROR_ACCESS_DENIED;
            return cxxime::encode_user_backup_result(result, response_payload);
        }
        std::string snapshot_config;
        std::map<std::string, std::string> current_user_data;
        const auto user_data_names = selected_user_data_names(request.components);
        const bool needs_config =
            includes(request.components, cxxime::UserBackupComponent::kSettings) ||
            includes(request.components, cxxime::UserBackupComponent::kDeviceSettings);
        result.succeeded =
            (!needs_config || config_writer_->snapshot_user_config(&snapshot_config, &error)) &&
            (user_data_names.empty() ||
             session_manager_->snapshot_user_data(user_data_names, &current_user_data));
        const auto entries = make_entries(snapshot_config, current_user_data, request.components);
        if (result.succeeded) {
            result.succeeded = !entries.empty() && cxxime::write_user_backup_archive(
                                                       path, entries, &result.summary, &error);
        }
        result.error_code = result.succeeded
                                ? ERROR_SUCCESS
                                : (error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
        return cxxime::encode_user_backup_result(result, response_payload);
    }

    cxxime::UserBackupArchive archive;
    const bool archive_read = cxxime::read_user_backup_archive(path, &archive, &error);
    if (request.operation != cxxime::UserBackupOperation::kImport || !archive_read ||
        (request.components & ~archive.summary.components) != 0 ||
        !required_entries_present(archive, request.components)) {
        if (archive_read) {
            error = ERROR_INVALID_DATA;
        }
        result.error_code = error;
        return cxxime::encode_user_backup_result(result, response_payload);
    }

    const bool import_config =
        includes(request.components, cxxime::UserBackupComponent::kSettings) ||
        includes(request.components, cxxime::UserBackupComponent::kDeviceSettings);
    if (import_config) {
        std::string config_patch;
        std::string applied_config;
        if (!make_config_patch(archive, request.components, &config_patch)) {
            ++result.skipped_count;
        } else {
            const nlohmann::json patch = nlohmann::json::parse(config_patch);
            if (!patch.empty() &&
                config_writer_->submit(cxxime::UserConfigMutationKind::kMergePatch, config_patch,
                                       &applied_config, &error)) {
                result.imported_count += patch.size();
            } else if (!patch.empty()) {
                for (const auto& item : patch.items()) {
                    const std::string section = nlohmann::json({{item.key(), item.value()}}).dump();
                    if (config_writer_->submit(cxxime::UserConfigMutationKind::kMergePatch, section,
                                               &applied_config, &error)) {
                        ++result.imported_count;
                    } else {
                        ++result.skipped_count;
                    }
                }
            }
        }
    }
    const auto imported_data = selected_user_data(archive, request.components);
    std::size_t imported_data_count = 0;
    std::size_t skipped_data_count = 0;
    if (!imported_data.empty()) {
        session_manager_->merge_user_data(imported_data, &imported_data_count, &skipped_data_count);
    }
    result.imported_count += imported_data_count;
    result.skipped_count += skipped_data_count;
    result.succeeded = true;
    result.error_code = ERROR_SUCCESS;
    result.summary = archive.summary;
    return cxxime::encode_user_backup_result(result, response_payload);
}
