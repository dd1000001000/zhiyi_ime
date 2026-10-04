// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "installer_lifecycle_internal.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>

#include <json.hpp>

namespace cxxime {
namespace installer {
namespace lifecycle_internal {
namespace {

constexpr wchar_t kManifestName[] = L"install-manifest.json";
constexpr std::uint64_t kMaxManifestSize = 1024ULL * 1024ULL;
constexpr DWORD kMoveFileDelayUntilReboot = 0x4;

constexpr const wchar_t* kLifecycleControlFiles[] = {
    L".zhiyi-install-complete",          L".zhiyi-install-transaction",
    L".zhiyi-install-transaction.tmp",   L".zhiyi-uninstall-transaction",
    L".zhiyi-uninstall-transaction.tmp",
};

bool read_file(const std::wstring& path, std::string* contents, unsigned long* error_code) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error_code, GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool succeeded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
                     static_cast<std::uint64_t>(size.QuadPart) <= kMaxManifestSize &&
                     size.QuadPart <= MAXDWORD;
    if (succeeded) {
        contents->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        succeeded =
            contents->empty() || (ReadFile(file, &(*contents)[0],
                                           static_cast<DWORD>(contents->size()), &read, nullptr) &&
                                  read == contents->size());
    }
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
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

bool valid_manifest_path(const std::string& value) {
    if (value.empty() || value.size() > 260 || value.front() == '/' || value.front() == '\\' ||
        value.find(':') != std::string::npos || value.find("..") != std::string::npos) {
        return false;
    }
    bool component_start = true;
    for (unsigned char ch : value) {
        if (ch == '/' || ch == '\\') {
            if (component_start) {
                return false;
            }
            component_start = true;
            continue;
        }
        if (ch < 0x20 || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' ||
            ch == '*') {
            return false;
        }
        component_start = false;
    }
    return !component_start;
}

bool load_manifest(const std::wstring& directory, std::vector<std::wstring>* files,
                   bool* manifest_present, unsigned long* error_code) {
    const std::wstring path = directory + L"\\" + kManifestName;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    *manifest_present = attributes != INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_error(error_code, GetLastError());
        return false;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    std::string contents;
    if (!read_file(path, &contents, error_code)) {
        return false;
    }
    try {
        const nlohmann::json manifest = nlohmann::json::parse(contents);
        if (!manifest.is_object() || manifest.value("format", "") != "zhiyi-install-manifest" ||
            manifest.value("version", 0u) != 1 || !manifest.contains("files") ||
            !manifest["files"].is_array() || manifest["files"].size() > 256) {
            throw std::runtime_error("invalid install manifest");
        }
        std::set<std::string> unique;
        for (const auto& item : manifest["files"]) {
            if (!item.is_string()) {
                throw std::runtime_error("invalid manifest path");
            }
            const std::string relative = item.get<std::string>();
            std::wstring wide;
            if (!valid_manifest_path(relative) || !unique.insert(relative).second ||
                !utf8_to_wide(relative, &wide)) {
                throw std::runtime_error("invalid manifest path");
            }
            std::replace(wide.begin(), wide.end(), L'/', L'\\');
            files->push_back(std::move(wide));
        }
        for (const auto* control_file : kLifecycleControlFiles) {
            if (std::find(files->begin(), files->end(), control_file) == files->end()) {
                files->emplace_back(control_file);
            }
        }
        return true;
    } catch (const std::exception&) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
}

void collect_parent_directories(const std::wstring& relative, std::set<std::wstring>* directories) {
    std::size_t separator = relative.find_last_of(L'\\');
    while (separator != std::wstring::npos) {
        directories->insert(relative.substr(0, separator));
        if (separator == 0) {
            break;
        }
        separator = relative.find_last_of(L'\\', separator - 1);
    }
}

bool path_has_reparse_component(const std::wstring& directory, const std::wstring& relative) {
    std::size_t separator = relative.find(L'\\');
    while (separator != std::wstring::npos) {
        const std::wstring parent = directory + L"\\" + relative.substr(0, separator);
        const DWORD attributes = GetFileAttributesW(parent.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return true;
        }
        separator = relative.find(L'\\', separator + 1);
    }
    return false;
}

bool is_lifecycle_control_file(const std::wstring& relative) {
    return std::any_of(
        std::begin(kLifecycleControlFiles), std::end(kLifecycleControlFiles),
        [&](const wchar_t* value) { return _wcsicmp(relative.c_str(), value) == 0; });
}

std::wstring path_key(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool has_discoverable_manifest(const std::wstring& root, const std::string& generation) {
    const std::wstring directory = generation_path(root, generation);
    const DWORD directory_attributes = GetFileAttributesW(directory.c_str());
    if (directory_attributes == INVALID_FILE_ATTRIBUTES ||
        !(directory_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return false;
    }
    const DWORD manifest_attributes =
        GetFileAttributesW((directory + L"\\" + kManifestName).c_str());
    return manifest_attributes != INVALID_FILE_ATTRIBUTES &&
           !(manifest_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool count_unknown_entries(const std::wstring& directory, const std::wstring& relative,
                           const std::set<std::wstring>& known_files,
                           const std::set<std::wstring>& known_directories, std::uint32_t* unknown,
                           unsigned long* error_code) {
    const std::wstring current = relative.empty() ? directory : directory + L"\\" + relative;
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((current + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        set_error(error_code, error);
        return false;
    }
    do {
        if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child =
            relative.empty() ? entry.cFileName : relative + L"\\" + entry.cFileName;
        const std::wstring key = path_key(child);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ++*unknown;
        } else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (known_directories.find(key) == known_directories.end()) {
                ++*unknown;
            } else if (!count_unknown_entries(directory, child, known_files, known_directories,
                                              unknown, error_code)) {
                FindClose(search);
                return false;
            }
        } else if (known_files.find(key) == known_files.end()) {
            ++*unknown;
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

bool remove_empty_directory_tree(const std::wstring& directory, bool* removed,
                                 bool* unknown_content, unsigned long* error_code) {
    *removed = false;
    *unknown_content = false;
    bool incomplete_child = false;
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                *unknown_content = true;
                continue;
            }
            bool child_removed = false;
            bool child_unknown = false;
            if (!remove_empty_directory_tree(directory + L"\\" + entry.cFileName, &child_removed,
                                             &child_unknown, error_code)) {
                FindClose(search);
                return false;
            }
            *unknown_content = *unknown_content || child_unknown;
            incomplete_child = incomplete_child || (!child_removed && !child_unknown);
        } while (FindNextFileW(search, &entry));
        const DWORD error = GetLastError();
        FindClose(search);
        if (error != ERROR_NO_MORE_FILES) {
            set_error(error_code, error);
            return false;
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            set_error(error_code, error);
            return false;
        }
    }
    if (*unknown_content || incomplete_child) {
        return true;
    }
    if (RemoveDirectoryW(directory.c_str())) {
        *removed = true;
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        *removed = true;
        return true;
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_DIR_NOT_EMPTY) {
        return true;
    }
    set_error(error_code, error);
    return false;
}

bool cleanup_generation(const std::wstring& root, const std::string& generation,
                        InstallLifecycleResult* result, bool* completed,
                        unsigned long* error_code) {
    *completed = false;
    const std::wstring directory = generation_path(root, generation);
    if (directory.empty()) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    const DWORD directory_attributes = GetFileAttributesW(directory.c_str());
    if (directory_attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *completed = true;
            return true;
        }
        set_error(error_code, error);
        return false;
    }
    if (!(directory_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }

    std::vector<std::wstring> files;
    bool manifest_present = false;
    unsigned long manifest_error = ERROR_SUCCESS;
    if (!load_manifest(directory, &files, &manifest_present, &manifest_error)) {
        if (!manifest_present &&
            (manifest_error == ERROR_FILE_NOT_FOUND || manifest_error == ERROR_PATH_NOT_FOUND)) {
            bool removed = false;
            bool unknown_content = false;
            if (!remove_empty_directory_tree(directory, &removed, &unknown_content, error_code)) {
                return false;
            }
            if (unknown_content) {
                ++result->unknown_files;
                *completed = true;
            } else if (removed) {
                ++result->cleaned_generations;
                *completed = true;
            }
            set_error(error_code, ERROR_SUCCESS);
            return true;
        }
        set_error(error_code, manifest_error);
        return false;
    }
    std::set<std::wstring> parent_directories;
    std::set<std::wstring> known_files = {path_key(kManifestName)};
    std::uint32_t scheduled = 0;
    bool unresolved = false;
    for (const auto& relative : files) {
        collect_parent_directories(relative, &parent_directories);
        known_files.insert(path_key(relative));
        if (path_has_reparse_component(directory, relative)) {
            unresolved = true;
            continue;
        }
        const std::wstring path = directory + L"\\" + relative;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            unresolved = true;
            continue;
        }
        if (DeleteFileW(path.c_str())) {
            continue;
        }
        const DWORD error = GetLastError();
        if ((error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) &&
            MoveFileExW(path.c_str(), nullptr, kMoveFileDelayUntilReboot)) {
            ++scheduled;
            continue;
        }
        unresolved = true;
    }

    std::set<std::wstring> known_directories;
    for (const auto& relative : parent_directories) {
        known_directories.insert(path_key(relative));
    }
    std::uint32_t unknown = 0;
    if (!count_unknown_entries(directory, L"", known_files, known_directories, &unknown,
                               error_code)) {
        return false;
    }
    result->unknown_files += unknown;

    result->scheduled_files += scheduled;
    if (unresolved || (scheduled != 0 && unknown != 0)) {
        return true;
    }

    std::vector<std::wstring> directories(parent_directories.begin(), parent_directories.end());
    std::sort(directories.begin(), directories.end(),
              [](const auto& left, const auto& right) { return left.size() > right.size(); });
    if (scheduled != 0) {
        for (const auto& relative : directories) {
            if (!MoveFileExW((directory + L"\\" + relative).c_str(), nullptr,
                             kMoveFileDelayUntilReboot)) {
                return true;
            }
        }
        if (manifest_present && !MoveFileExW((directory + L"\\" + kManifestName).c_str(), nullptr,
                                             kMoveFileDelayUntilReboot)) {
            return true;
        }
        if (!MoveFileExW(directory.c_str(), nullptr, kMoveFileDelayUntilReboot)) {
            return true;
        }
        *completed = true;
        ++result->cleaned_generations;
        return true;
    }
    for (const auto& relative : directories) {
        RemoveDirectoryW((directory + L"\\" + relative).c_str());
    }
    if (manifest_present) {
        DeleteFileW((directory + L"\\" + kManifestName).c_str());
    }
    if (!RemoveDirectoryW(directory.c_str())) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED &&
            MoveFileExW(directory.c_str(), nullptr, kMoveFileDelayUntilReboot)) {
            ++result->scheduled_files;
        } else if (error == ERROR_DIR_NOT_EMPTY) {
            if (unknown != 0) {
                *completed = true;
                ++result->cleaned_generations;
            }
            return true;
        } else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return true;
        }
    }
    *completed = true;
    ++result->cleaned_generations;
    return true;
}

} // namespace

bool validate_generation(const std::wstring& root, const std::string& generation,
                         unsigned long* error_code) {
    const std::wstring directory = generation_path(root, generation);
    if (directory.empty()) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_error(error_code, GetLastError());
        return false;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    std::vector<std::wstring> files;
    bool manifest_present = false;
    if (!load_manifest(directory, &files, &manifest_present, error_code)) {
        return false;
    }
    for (const auto& relative : files) {
        if (path_has_reparse_component(directory, relative)) {
            set_error(error_code, ERROR_INVALID_DATA);
            return false;
        }
        if (is_lifecycle_control_file(relative)) {
            continue;
        }
        const DWORD file_attributes = GetFileAttributesW((directory + L"\\" + relative).c_str());
        if (file_attributes == INVALID_FILE_ATTRIBUTES) {
            set_error(error_code, GetLastError());
            return false;
        }
        if (file_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_error(error_code, ERROR_INVALID_DATA);
            return false;
        }
    }
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

bool collect_garbage(const std::wstring& root, State* state, InstallLifecycleResult* result,
                     unsigned long* error_code) {
    std::vector<std::string> remaining;
    for (const auto& generation : state->retired) {
        if (generation == state->active) {
            set_error(error_code, ERROR_INVALID_DATA);
            return false;
        }
        bool completed = false;
        if (!cleanup_generation(root, generation, result, &completed, error_code)) {
            ++result->unknown_files;
            ++result->remaining_generations;
            if (!has_discoverable_manifest(root, generation)) {
                remaining.push_back(generation);
            }
            set_error(error_code, ERROR_SUCCESS);
            continue;
        }
        if (!completed) {
            ++result->remaining_generations;
            remaining.push_back(generation);
        }
    }
    state->retired = std::move(remaining);
    return true;
}

} // namespace lifecycle_internal
} // namespace installer
} // namespace cxxime
