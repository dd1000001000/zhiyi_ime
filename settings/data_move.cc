// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "data_move.h"

#include <vector>

#include <windows.h>

namespace cxxime {
namespace settings {

namespace fs = std::filesystem;

namespace {

std::wstring lower(std::wstring text) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

bool copy_tree(const fs::path& from, const fs::path& to, const fs::path& skip, std::uint64_t& done,
               const std::function<void(std::uint64_t)>& progress) {
    std::error_code error;
    fs::create_directories(to, error);
    if (error) return false;
    if (!fs::exists(from, error)) return true;
    for (auto it = fs::recursive_directory_iterator(from, error);
         it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) return false;
        if (!skip.empty() && it->path() == skip) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path target = to / fs::relative(it->path(), from, error);
        if (error) return false;
        if (it->is_directory(error)) {
            fs::create_directories(target, error);
            if (error) return false;
            continue;
        }
        if (!it->is_regular_file(error)) continue;
        if (!CopyFileW(it->path().c_str(), target.c_str(), FALSE)) return false;
        const std::uint64_t size = it->file_size(error);
        if (error) return false;
        const std::uint64_t copied = fs::file_size(target, error);
        if (error || copied != size) return false;
        done += size;
        if (progress) progress(done);
    }
    return true;
}

// Deletes `dir` and everything in it but `keep`; returns how many files could not be deleted.
std::uint64_t remove_tree(const fs::path& dir, const fs::path& keep) {
    std::uint64_t left = 0;
    std::error_code error;
    if (!fs::exists(dir, error)) return 0;
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(dir, error);
         it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) break;
        if (!keep.empty() && it->path() == keep) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(error)) {
            dirs.push_back(it->path());
        } else {
            SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(it->path().c_str())) ++left;
        }
    }
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) RemoveDirectoryW(d->c_str());
    RemoveDirectoryW(dir.c_str());
    return left;
}

}  // namespace

bool path_within(const std::wstring& inner, const std::wstring& outer) {
    const std::wstring a = lower(inner), b = lower(outer);
    return !b.empty() && a.size() >= b.size() && a.compare(0, b.size(), b) == 0;
}

std::uint64_t data_size(const fs::path& root, const fs::path& skip) {
    std::uint64_t total = 0;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied,
                                                    error);
         it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) break;
        if (!skip.empty() && it->path() == skip) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(error)) total += it->file_size(error);
    }
    return total;
}

fs::path nested_local(const DataPlaces& places) {
    return path_within(places.local, places.user) ? fs::path(places.local).parent_path() : fs::path();
}

DataMoveResult move_data(const DataPlaces& from, const DataPlaces& to, bool use_existing,
                         const std::function<bool()>& switch_to,
                         const std::function<void(std::uint64_t)>& progress) {
    DataMoveResult result;
    const fs::path from_skip = nested_local(from);
    const fs::path to_skip = nested_local(to);
    // Only a copy into empty folders is cleaned up after a failure.
    const bool fresh = data_size(to.user, {}) == 0 && data_size(to.local, {}) == 0;
    std::uint64_t copied = 0;
    bool ok = use_existing || (copy_tree(from.user, to.user, from_skip, copied, progress) &&
                               copy_tree(from.local, to.local, {}, copied, progress));
    if (ok) ok = switch_to && switch_to();
    if (!ok) {
        if (!use_existing && fresh) {  // the partial copy goes; the old data was not touched
            remove_tree(to.local, {});
            remove_tree(to.user, to_skip);
        }
        result.copy_failed = true;
        return result;
    }
    result.ok = true;
    result.left = remove_tree(from.local, {}) + remove_tree(from.user, from_skip);
    return result;
}

}  // namespace settings
}  // namespace cxxime
