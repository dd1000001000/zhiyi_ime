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

namespace {

constexpr wchar_t kRegistryKey[] = L"Software\\ZhiyiIME";
constexpr wchar_t kRegistryValue[] = L"DataDirectory";

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], len,
                            nullptr, nullptr);
    }
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], len);
    }
    return out;
}

std::wstring known_folder(int csidl, const wchar_t* tail) {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return {};
    return std::wstring(buf) + tail;
}

bool is_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring chosen_folder_wide() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, RRF_RT_REG_SZ, nullptr, buf,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring folder(buf);
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    return folder;
}

// <chosen folder>\zhiyi\ when the folder exists (and the zhiyi folder can be made), else empty.
std::wstring chosen_data_root() {
    const std::wstring folder = chosen_folder_wide();
    if (folder.empty() || !is_directory(folder)) return {};
    std::wstring root = folder;
    if (root.back() != L'\\') root += L'\\';
    root += L"zhiyi\\";
    CreateDirectoryW(root.c_str(), nullptr);
    return is_directory(root) ? root : std::wstring();
}

std::string ensure_dir(const std::wstring& dir) {
    if (dir.empty()) return {};
    CreateDirectoryW(dir.c_str(), nullptr);
    return to_utf8(dir);
}

}  // namespace

std::string chosen_data_folder() { return to_utf8(chosen_folder_wide()); }

bool set_chosen_data_folder(const std::string& folder) {
    if (folder.empty()) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring value = to_wide(folder);
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool chosen_data_folder_unavailable() {
    return !chosen_folder_wide().empty() && chosen_data_root().empty();
}

std::string default_user_data_dir() { return ensure_dir(known_folder(CSIDL_PROFILE, L"\\zhiyi\\")); }

std::string default_local_data_dir() {
    return ensure_dir(known_folder(CSIDL_LOCAL_APPDATA, L"\\zhiyi\\"));
}

std::string user_data_dir() {
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        if (!g_user_data_dir_override.empty()) {
            return g_user_data_dir_override;
        }
    }
    const std::wstring root = chosen_data_root();
    return root.empty() ? default_user_data_dir() : to_utf8(root);
}

std::string local_data_dir() {
    {
        std::lock_guard<std::mutex> lock(g_override_mutex);
        if (!g_user_data_dir_override.empty()) {
            return ensure_dir(to_wide(g_user_data_dir_override) + L"local\\");
        }
    }
    const std::wstring root = chosen_data_root();
    return root.empty() ? default_local_data_dir() : ensure_dir(root + L"local\\");
}

std::string local_data_path(const char* filename) { return local_data_dir() + filename; }

std::string user_data_path(const char* filename) { return user_data_dir() + filename; }

} // namespace cxxime
