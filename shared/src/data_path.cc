// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/data_path.h>
#include <mutex>
#include <windows.h>
#include <shlobj.h>

namespace cxxime {

// Protects data_dir_override from concurrent read/write.
// set_data_dir() is called once at startup, but data_dir() may be called
// from any thread (TSF DLL loaded in multiple processes).
static std::mutex g_override_mutex;
static std::string g_data_dir_override;
static std::string g_user_data_dir_override;
static HMODULE g_module_handle = nullptr;

void set_module_handle(HMODULE hModule) {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_module_handle = hModule;
}

void set_data_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_data_dir_override = dir;
    if (!dir.empty() && dir.back() != '\\') {
        g_data_dir_override += '\\';
    }
}

void set_user_data_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_user_data_dir_override = dir;
    if (!dir.empty() && dir.back() != '\\') {
        g_user_data_dir_override += '\\';
    }
}

std::string data_dir() {
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        if (!g_data_dir_override.empty()) return g_data_dir_override;
    }

#ifdef CXXIME_DATA_DIR
    return CXXIME_DATA_DIR;
#else
    // Production: <dll_dir>\data\ (magic static — thread-safe init)
    static std::string cached;
    if (cached.empty()) {
        HMODULE mod = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_override_mutex);
            mod = g_module_handle;
        }
        wchar_t modPath[MAX_PATH];
        if (GetModuleFileNameW(mod, modPath, MAX_PATH)) {
            std::wstring dataDir(modPath);
            dataDir.erase(dataDir.rfind(L'\\') + 1);
            dataDir += L"data\\";
            int len =
                WideCharToMultiByte(CP_UTF8, 0, dataDir.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                cached.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, dataDir.c_str(), -1, &cached[0], len, nullptr,
                                    nullptr);
            }
        }
    }
    return cached;
#endif
}

std::string data_path(const char* filename) { return data_dir() + filename; }

std::string user_data_dir() {
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        if (!g_user_data_dir_override.empty()) {
            return g_user_data_dir_override;
        }
    }

    // magic static — thread-safe init, no lock needed
    static std::string dir;
    if (dir.empty()) {
        wchar_t profile[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile))) {
            std::wstring wdir(profile);
            wdir += L"\\zhiyi\\";
            CreateDirectoryW(wdir.c_str(), nullptr);
            int len =
                WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                dir.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), -1, &dir[0], len, nullptr, nullptr);
            }
        }
    }
    return dir;
}

std::string user_data_path(const char* filename) { return user_data_dir() + filename; }

} // namespace cxxime
