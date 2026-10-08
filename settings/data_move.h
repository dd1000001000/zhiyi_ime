// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Moving the data folder (Settings > General, editor_data_folder.cc): copy, compare, switch,
// delete the old copies. Kept apart from the window so it can be tested on its own.
#ifndef CXXIME_SETTINGS_DATA_MOVE_H_
#define CXXIME_SETTINGS_DATA_MOVE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace cxxime {
namespace settings {

// Where the data is or goes: user data, and the machine-specific part (inside the user data
// folder when a folder is chosen). Both end in '\'.
struct DataPlaces {
    std::wstring user;
    std::wstring local;
};

// `inner` is `outer` or inside it (both ending in '\'; case-insensitive).
bool path_within(const std::wstring& inner, const std::wstring& outer);

// Total size of the files in `root`, leaving out the folder `skip` (empty: none).
std::uint64_t data_size(const std::filesystem::path& root, const std::filesystem::path& skip);

// The folder inside `places.user` that holds `places.local`, or empty.
std::filesystem::path nested_local(const DataPlaces& places);

struct DataMoveResult {
    bool ok = false;
    bool copy_failed = false;  // nothing was switched; the old data is as it was
    std::uint64_t left = 0;    // old files that could not be deleted (in use)
};

// Copies `from` to `to` (unless `use_existing`), each file compared by size, then calls
// `switch_to()` (which records the new place) and deletes the old copies. When copying or
// switching fails the partial copy is removed and the old data stays. `progress(copied)` is
// called as files are copied.
DataMoveResult move_data(const DataPlaces& from, const DataPlaces& to, bool use_existing,
                         const std::function<bool()>& switch_to,
                         const std::function<void(std::uint64_t)>& progress);

}  // namespace settings
}  // namespace cxxime

#endif  // CXXIME_SETTINGS_DATA_MOVE_H_
