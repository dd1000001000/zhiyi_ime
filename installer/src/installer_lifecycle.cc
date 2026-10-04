// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/installer_lifecycle.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <windows.h>

#include "installer_lifecycle_internal.h"

namespace cxxime {
namespace installer {

bool prepare_install_lifecycle(const std::wstring& root, const std::wstring& registered_active,
                               const std::string& version, InstallLifecycleResult* result,
                               unsigned long* error_code) {
    using namespace lifecycle_internal;
    if (!result) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root) ||
        !ensure_maintenance_directory(normalized_root, error_code)) {
        return false;
    }
    State state;
    InstallLifecycleResult prepared;
    unsigned long state_error = ERROR_SUCCESS;
    if (!load_state(normalized_root, &state, &state_error)) {
        if (state_error != ERROR_INVALID_DATA && state_error != ERROR_FILE_TOO_LARGE) {
            set_error(error_code, state_error);
            return false;
        }
        state = {};
        set_error(error_code, ERROR_SUCCESS);
    }
    if (!state.prepared.empty()) {
        const std::wstring previous_target = generation_path(normalized_root, state.prepared);
        const std::wstring transaction = previous_target + L"\\.zhiyi-install-transaction";
        if (GetFileAttributesW(transaction.c_str()) != INVALID_FILE_ATTRIBUTES) {
            set_error(error_code, ERROR_BUSY);
            return false;
        }
        const std::string abandoned = state.prepared;
        state.prepared.clear();
        add_retired(&state, abandoned);
    }
    if (!reconcile_state(normalized_root, registered_active, &state, error_code) ||
        !discover_retired_generations(normalized_root, &state, error_code) ||
        !save_state(normalized_root, state, error_code) ||
        !collect_garbage(normalized_root, &state, &prepared, error_code) ||
        !allocate_target(normalized_root, version, &prepared, error_code)) {
        return false;
    }
    if (!generation_from_path(normalized_root, prepared.install_target, &state.prepared) ||
        !save_state(normalized_root, state, error_code)) {
        return false;
    }
    *result = std::move(prepared);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool commit_install_lifecycle(const std::wstring& root, const std::wstring& new_active,
                              const std::wstring& old_active, InstallLifecycleResult* result,
                              unsigned long* error_code) {
    using namespace lifecycle_internal;
    if (!result) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root)) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    State state;
    if (!load_state(normalized_root, &state, error_code)) {
        return false;
    }
    std::string new_generation;
    if (!generation_from_path(normalized_root, new_active, &new_generation)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    if (!state.prepared.empty() && state.prepared != new_generation) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    if (!validate_generation(normalized_root, new_generation, error_code)) {
        return false;
    }
    std::string old_generation;
    if (!old_active.empty() &&
        !generation_from_path(normalized_root, old_active, &old_generation)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    if (state.active != new_generation) {
        const std::string stale_active = state.active;
        state.active = new_generation;
        add_retired(&state, stale_active);
    }
    state.retired.erase(std::remove(state.retired.begin(), state.retired.end(), new_generation),
                        state.retired.end());
    add_retired(&state, old_generation);
    state.prepared.clear();
    InstallLifecycleResult committed;
    if (!save_state(normalized_root, state, error_code)) {
        return false;
    }
    committed.install_target = new_active;
    *result = std::move(committed);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool collect_install_garbage(const std::wstring& root, InstallLifecycleResult* result,
                             unsigned long* error_code) {
    using namespace lifecycle_internal;
    if (!result) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root)) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    State state;
    InstallLifecycleResult collected;
    if (!load_state(normalized_root, &state, error_code) ||
        !discover_retired_generations(normalized_root, &state, error_code) ||
        !save_state(normalized_root, state, error_code) ||
        !collect_garbage(normalized_root, &state, &collected, error_code) ||
        !save_state(normalized_root, state, error_code)) {
        return false;
    }
    *result = std::move(collected);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool get_prepared_install_target(const std::wstring& root, InstallLifecycleResult* result,
                                 unsigned long* error_code) {
    using namespace lifecycle_internal;
    if (!result) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root)) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    State state;
    if (!load_state(normalized_root, &state, error_code)) {
        if (error_code &&
            (*error_code == ERROR_INVALID_DATA || *error_code == ERROR_FILE_TOO_LARGE)) {
            *result = {};
            set_error(error_code, ERROR_SUCCESS);
            return true;
        }
        return false;
    }
    InstallLifecycleResult prepared;
    if (!state.prepared.empty()) {
        prepared.install_target = generation_path(normalized_root, state.prepared);
        if (prepared.install_target.empty()) {
            set_error(error_code, ERROR_INVALID_DATA);
            return false;
        }
    }
    *result = std::move(prepared);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool validate_uninstall_lifecycle(const std::wstring& root, const std::wstring& active,
                                  unsigned long* error_code) {
    using namespace lifecycle_internal;
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root)) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    State state;
    if (!load_state(normalized_root, &state, error_code)) {
        return false;
    }
    if (!state.prepared.empty()) {
        const std::wstring prepared_target = generation_path(normalized_root, state.prepared);
        const std::wstring transaction = prepared_target + L"\\.zhiyi-install-transaction";
        if (GetFileAttributesW(transaction.c_str()) != INVALID_FILE_ATTRIBUTES) {
            set_error(error_code, ERROR_BUSY);
            return false;
        }
        const std::string abandoned = state.prepared;
        state.prepared.clear();
        add_retired(&state, abandoned);
    }
    std::string active_generation;
    if (!generation_from_path(normalized_root, active, &active_generation)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    state.active = active_generation;
    state.retired.erase(std::remove(state.retired.begin(), state.retired.end(), active_generation),
                        state.retired.end());
    if (!discover_retired_generations(normalized_root, &state, error_code) ||
        !validate_generation(normalized_root, active_generation, error_code)) {
        return false;
    }
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool uninstall_install_lifecycle(const std::wstring& root, const std::wstring& active,
                                 InstallLifecycleResult* result, unsigned long* error_code) {
    using namespace lifecycle_internal;
    if (!result) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::wstring normalized_root;
    if (!normalize_path(root, &normalized_root)) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    State state;
    if (!load_state(normalized_root, &state, error_code)) {
        return false;
    }
    std::string active_generation;
    if (!active.empty() && !generation_from_path(normalized_root, active, &active_generation)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    state.active.clear();
    state.prepared.clear();
    add_retired(&state, active_generation);
    InstallLifecycleResult cleaned;
    if (!discover_retired_generations(normalized_root, &state, error_code) ||
        !save_state(normalized_root, state, error_code) ||
        !collect_garbage(normalized_root, &state, &cleaned, error_code) ||
        !save_state(normalized_root, state, error_code)) {
        return false;
    }
    *result = std::move(cleaned);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool write_install_lifecycle_result(const std::wstring& path, const InstallLifecycleResult& result,
                                    unsigned long* error_code) {
    std::wstring contents = L"[lifecycle]\r\n";
    contents += L"target=" + result.install_target + L"\r\n";
    contents += L"cleaned=" + std::to_wstring(result.cleaned_generations) + L"\r\n";
    contents += L"scheduled=" + std::to_wstring(result.scheduled_files) + L"\r\n";
    contents += L"unknown=" + std::to_wstring(result.unknown_files) + L"\r\n";
    contents += L"remaining=" + std::to_wstring(result.remaining_generations) + L"\r\n";
    constexpr wchar_t kBom = 0xfeff;
    std::vector<wchar_t> output;
    output.reserve(contents.size() + 1);
    output.push_back(kBom);
    output.insert(output.end(), contents.begin(), contents.end());
    return lifecycle_internal::write_file_atomically(path, output.data(),
                                                     output.size() * sizeof(wchar_t), error_code);
}

} // namespace installer
} // namespace cxxime
