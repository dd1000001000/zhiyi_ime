// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_DATA_PATH_H_
#define CXXIME_DATA_PATH_H_

#include <string>
#include <windows.h>

namespace cxxime {

// Set the DLL module handle for correct path resolution when loaded
// into foreign processes (e.g. TSF DLL in Notepad.exe).
void set_module_handle(HMODULE hModule);

// Runtime override for data directory. When set (non-empty), data_dir()
// returns this path instead of the compile-time or default production path.
// Set via --data command-line flag on the server, or programmatically in tools.
void set_data_dir(const std::string& dir);

// Shared data directory (read-only).
// 1. Runtime override (set_data_dir)
// 2. Compile-time CXXIME_DATA_DIR (dev/test builds)
// 3. <exe_dir>\data\ (production)
std::string data_dir();

// data_dir() + filename
std::string data_path(const char* filename);

// Runtime override for the per-user writable directory. Intended for tools
// and tests that must not read or modify the active Windows user's data.
void set_user_data_dir(const std::string& dir);

// Per-user writable directory: <chosen folder>\zhiyi\ when the user chose a data folder
// (Settings > General) and it can be used, otherwise %USERPROFILE%\zhiyi\.
// Created automatically.
std::string user_data_dir();

// user_data_dir() + filename
std::string user_data_path(const char* filename);

// Machine-specific caches and downloads (Laya cache, graphics card tests, the translation
// model): <chosen folder>\zhiyi\local\, otherwise %LOCALAPPDATA%\zhiyi\. Created automatically.
// Not part of backups.
std::string local_data_dir();
std::string local_data_path(const char* filename);

// The data folder chosen in Settings > General (HKCU\Software\ZhiyiIME, DataDirectory), UTF-8
// without a trailing '\'; empty = the default places. Data lives in <folder>\zhiyi\.
std::string chosen_data_folder();
// Writes (or, when empty, removes) the registry value. Returns false when it could not.
bool set_chosen_data_folder(const std::string& folder);
// A folder is chosen but cannot be used (a removed drive): the default places are in use.
bool chosen_data_folder_unavailable();

// The default places, whatever is chosen.
std::string default_user_data_dir();
std::string default_local_data_dir();

// Held by Settings while it moves the data folder; zhiyi-server waits for it before loading.
constexpr wchar_t kDataMoveMutex[] = L"Local\\ZhiyiIME.DataMove";

} // namespace cxxime

#endif
