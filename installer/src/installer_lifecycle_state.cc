// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "installer_lifecycle_internal.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>
#include <objbase.h>

#include <json.hpp>

namespace cxxime {
namespace installer {
namespace lifecycle_internal {
namespace {

constexpr char kStateFormat[] = "zhiyi-install-lifecycle";
constexpr std::uint32_t kStateVersion = 1;
constexpr wchar_t kManifestName[] = L"install-manifest.json";
constexpr std::uint64_t kMaxStateSize = 64ULL * 1024ULL;
constexpr std::size_t kMaxGenerationNameLength = 128;
constexpr std::size_t kGenerationIdHexLength = 8;
static_assert(kGenerationIdHexLength % 2 == 0, "hex generation IDs require complete bytes");

std::wstring maintenance_dir(const std::wstring& root) { return root + L"\\maintenance"; }

std::wstring state_path(const std::wstring& root) {
    return maintenance_dir(root) + L"\\install-state.json";
}

bool read_file(const std::wstring& path, std::string* contents, unsigned long* error_code) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error_code, GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool succeeded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    if (succeeded &&
        (static_cast<std::uint64_t>(size.QuadPart) > kMaxStateSize || size.QuadPart > MAXDWORD)) {
        succeeded = false;
        error = ERROR_FILE_TOO_LARGE;
    }
    if (succeeded) {
        contents->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        succeeded =
            contents->empty() || (ReadFile(file, &(*contents)[0],
                                           static_cast<DWORD>(contents->size()), &read, nullptr) &&
                                  read == contents->size());
    }
    if (!succeeded && error == ERROR_SUCCESS) {
        error = GetLastError();
    }
    CloseHandle(file);
    set_error(error_code, error);
    return succeeded;
}

bool utf8_to_wide(const std::string& value, std::wstring* result) {
    if (!result || value.empty()) {
        return false;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return false;
    }
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &converted[0], count) != count) {
        return false;
    }
    *result = std::move(converted);
    return true;
}

bool wide_to_utf8(const std::wstring& value, std::string* result) {
    if (!result || value.empty()) {
        return false;
    }
    const int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return false;
    }
    std::string converted(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &converted[0], count, nullptr,
                            nullptr) != count) {
        return false;
    }
    *result = std::move(converted);
    return true;
}

bool valid_generation_name(const std::string& value) {
    return !value.empty() && value.size() <= kMaxGenerationNameLength && value != "." &&
           value != ".." && value.find("..") == std::string::npos &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
           });
}

bool path_does_not_exist(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    return false;
}

bool generation_paths_are_available(const std::wstring& root, const std::string& generation) {
    std::wstring wide;
    if (!utf8_to_wide(generation, &wide)) {
        return false;
    }
    const std::wstring pending_prefix = maintenance_dir(root) + L"\\ime-" + wide;
    return path_does_not_exist(root + L"\\" + wide) &&
           path_does_not_exist(pending_prefix + L"-x64.pending") &&
           path_does_not_exist(pending_prefix + L"-x86.pending");
}

} // namespace

void set_error(unsigned long* error_code, unsigned long value) {
    if (error_code) {
        *error_code = value;
    }
}

bool normalize_path(const std::wstring& path, std::wstring* normalized) {
    if (!normalized || path.empty()) {
        return false;
    }
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                              buffer.data(), nullptr);
        if (length == 0 || length >= 32768) {
            return false;
        }
        if (length < buffer.size()) {
            normalized->assign(buffer.data(), length);
            while (normalized->size() > 3 && normalized->back() == L'\\') {
                normalized->pop_back();
            }
            return normalized->size() >= 3 && (*normalized)[1] == L':' && (*normalized)[2] == L'\\';
        }
        buffer.resize(length + 1);
    }
}

bool generation_from_path(const std::wstring& root, const std::wstring& path,
                          std::string* generation) {
    if (!generation || path.empty()) {
        return false;
    }
    std::wstring normalized_root;
    std::wstring normalized_path;
    if (!normalize_path(root, &normalized_root) || !normalize_path(path, &normalized_path)) {
        return false;
    }
    const std::wstring prefix = normalized_root + L"\\";
    if (normalized_path.size() <= prefix.size() ||
        _wcsnicmp(normalized_path.c_str(), prefix.c_str(), prefix.size()) != 0) {
        return false;
    }
    const std::wstring leaf = normalized_path.substr(prefix.size());
    if (leaf.find(L'\\') != std::wstring::npos || !wide_to_utf8(leaf, generation) ||
        !valid_generation_name(*generation)) {
        return false;
    }
    return true;
}

std::wstring generation_path(const std::wstring& root, const std::string& generation) {
    std::wstring wide;
    return utf8_to_wide(generation, &wide) ? root + L"\\" + wide : std::wstring();
}

bool write_file_atomically(const std::wstring& path, const void* data, std::size_t size,
                           unsigned long* error_code) {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error_code, GetLastError());
        return false;
    }
    DWORD written = 0;
    bool succeeded =
        size <= MAXDWORD &&
        (size == 0 ||
         (WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size));
    if (succeeded) {
        succeeded = FlushFileBuffers(file) != FALSE;
    }
    DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (succeeded) {
        succeeded = MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        error = succeeded ? ERROR_SUCCESS : GetLastError();
    }
    if (!succeeded) {
        DeleteFileW(temporary.c_str());
    }
    set_error(error_code, error);
    return succeeded;
}

bool ensure_maintenance_directory(const std::wstring& root, unsigned long* error_code) {
    const std::wstring directory = maintenance_dir(root);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            set_error(error_code, error);
            return false;
        }
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

bool load_state(const std::wstring& root, State* state, unsigned long* error_code) {
    if (!state) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    const std::wstring path = state_path(root);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *state = {};
            set_error(error_code, ERROR_SUCCESS);
            return true;
        }
        set_error(error_code, error);
        return false;
    }
    std::string contents;
    if (!read_file(path, &contents, error_code)) {
        return false;
    }
    try {
        const nlohmann::json object = nlohmann::json::parse(contents);
        if (!object.is_object() || object.value("format", "") != kStateFormat ||
            object.value("version", 0u) != kStateVersion || !object.contains("active") ||
            !object["active"].is_string() || !object.contains("prepared") ||
            !object["prepared"].is_string() || !object.contains("retired") ||
            !object["retired"].is_array()) {
            throw std::runtime_error("invalid lifecycle state");
        }
        State parsed;
        parsed.active = object["active"].get<std::string>();
        parsed.prepared = object["prepared"].get<std::string>();
        if (!parsed.active.empty() && !valid_generation_name(parsed.active)) {
            throw std::runtime_error("invalid active generation");
        }
        if (!parsed.prepared.empty() &&
            (!valid_generation_name(parsed.prepared) || parsed.prepared == parsed.active)) {
            throw std::runtime_error("invalid prepared generation");
        }
        std::set<std::string> unique;
        for (const auto& item : object["retired"]) {
            if (!item.is_string()) {
                throw std::runtime_error("invalid retired generation");
            }
            const std::string value = item.get<std::string>();
            if (!valid_generation_name(value) || value == parsed.active ||
                value == parsed.prepared || !unique.insert(value).second) {
                throw std::runtime_error("invalid retired generation");
            }
            parsed.retired.push_back(value);
        }
        *state = std::move(parsed);
        set_error(error_code, ERROR_SUCCESS);
        return true;
    } catch (const std::exception&) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
}

bool save_state(const std::wstring& root, const State& state, unsigned long* error_code) {
    const nlohmann::json object = {
        {"format", kStateFormat},     {"version", kStateVersion}, {"active", state.active},
        {"prepared", state.prepared}, {"retired", state.retired},
    };
    const std::string contents = object.dump(2) + "\n";
    return ensure_maintenance_directory(root, error_code) &&
           write_file_atomically(state_path(root), contents.data(), contents.size(), error_code);
}

void add_retired(State* state, const std::string& generation) {
    if (!state || generation.empty() || generation == state->active ||
        std::find(state->retired.begin(), state->retired.end(), generation) !=
            state->retired.end()) {
        return;
    }
    state->retired.push_back(generation);
}

bool reconcile_state(const std::wstring& root, const std::wstring& registered_active, State* state,
                     unsigned long* error_code) {
    std::string active;
    if (!registered_active.empty() && !generation_from_path(root, registered_active, &active)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    if (active.empty() && !state->active.empty()) {
        const std::string stale_active = state->active;
        state->active.clear();
        add_retired(state, stale_active);
    } else if (!active.empty() && state->active != active) {
        const std::string stale_active = state->active;
        state->active = active;
        add_retired(state, stale_active);
        state->retired.erase(std::remove(state->retired.begin(), state->retired.end(), active),
                             state->retired.end());
    }
    return true;
}

bool discover_retired_generations(const std::wstring& root, State* state,
                                  unsigned long* error_code) {
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        set_error(error_code, error);
        return false;
    }
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0 ||
            _wcsicmp(entry.cFileName, L"maintenance") == 0 ||
            _wcsicmp(entry.cFileName, L"update") == 0) {
            continue;
        }
        std::string generation;
        const std::wstring name(entry.cFileName);
        if (!wide_to_utf8(name, &generation) || !valid_generation_name(generation) ||
            generation == state->active || generation == state->prepared) {
            continue;
        }
        const std::wstring manifest = root + L"\\" + name + L"\\" + kManifestName;
        const DWORD attributes = GetFileAttributesW(manifest.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            add_retired(state, generation);
        }
    } while (FindNextFileW(search, &entry));
    const DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) {
        set_error(error_code, error);
        return false;
    }
    return true;
}

bool allocate_target(const std::wstring& root, const std::string& version,
                     InstallLifecycleResult* result, unsigned long* error_code) {
    if (!valid_generation_name(version) ||
        version.size() > kMaxGenerationNameLength - kGenerationIdHexLength - 1) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    constexpr char kHex[] = "0123456789abcdef";
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
        GUID identifier = {};
        if (FAILED(CoCreateGuid(&identifier))) {
            set_error(error_code, ERROR_GEN_FAILURE);
            return false;
        }
        const auto* bytes = reinterpret_cast<const unsigned char*>(&identifier);
        std::string generation = version + ".";
        generation.reserve(version.size() + 1 + kGenerationIdHexLength);
        for (std::size_t index = 0; index < kGenerationIdHexLength / 2; ++index) {
            generation.push_back(kHex[bytes[index] >> 4]);
            generation.push_back(kHex[bytes[index] & 0x0f]);
        }
        const std::wstring path = generation_path(root, generation);
        if (!path.empty() && generation_paths_are_available(root, generation)) {
            result->install_target = path;
            set_error(error_code, ERROR_SUCCESS);
            return true;
        }
    }
    set_error(error_code, ERROR_TOO_MANY_NAMES);
    return false;
}

} // namespace lifecycle_internal
} // namespace installer
} // namespace cxxime
