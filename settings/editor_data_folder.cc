// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// "Data location" on the General page (docs/settings-guide.md): every file the input method
// writes (user data, caches, downloaded models) can live in a folder the user picks instead of
// drive C. The choice is in the registry (data_path.h); the data goes to <folder>\zhiyi\, the
// machine-specific part to <folder>\zhiyi\local\.
//
// Moving: zhiyi-server is closed (it saves its data on the way out) and held off with
// kDataMoveMutex, the files are copied and compared by size, the registry value is switched,
// the old copies are deleted, and the server is started again. On a failure before the switch
// nothing changes (the partial copy is removed).

#include "editor_app.h"

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <shobjidl.h>

#include <cxxime/data_path.h>
#include <cxxime/server_launcher.h>

#include "data_move.h"
#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

namespace fs = std::filesystem;

constexpr UINT kDataMovedMessage = WM_APP + 70;     // lParam: DataMoveDone*
constexpr UINT kDataProgressMessage = WM_APP + 71;  // wParam: MB copied, lParam: MB in all
constexpr int kServerStopWaitMs = 30000;

struct DataMoveDone {
    bool ok = false;
    std::wstring error;        // why it failed (shown in a message box)
    std::uint64_t left = 0;    // old files that could not be deleted
};

std::wstring with_slash(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path;
}

std::wstring lower(std::wstring text) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring full_path(const std::wstring& path) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : path;
}

DataPlaces current_places() {
    return {with_slash(utf8_to_wstr(user_data_dir())), with_slash(utf8_to_wstr(local_data_dir()))};
}

DataPlaces places_for(const std::wstring& folder) {
    if (folder.empty()) {
        return {with_slash(utf8_to_wstr(default_user_data_dir())),
                with_slash(utf8_to_wstr(default_local_data_dir()))};
    }
    const std::wstring user = with_slash(folder) + L"zhiyi\\";
    return {user, user + L"local\\"};
}

bool server_running() {
    HANDLE instance = OpenMutexW(SYNCHRONIZE, FALSE, kServerInstanceMutex);
    if (!instance) return false;
    CloseHandle(instance);
    return true;
}

// Asks zhiyi-server to exit (it saves its data first) and waits until it has.
bool stop_server() {
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"ZhiyiIMEServerClass",
                                                        nullptr)) != nullptr;) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    for (int waited = 0; waited < kServerStopWaitMs; waited += 100) {
        if (!server_running()) return true;
        Sleep(100);
    }
    return false;
}

// Something the input method keeps there already.
bool has_zhiyi_data(const std::wstring& user_dir) {
    for (const wchar_t* name : {L"default.json", L"learning_pinyin.tsv", L"user_pinyin.tsv",
                                L"learning_english.json"}) {
        if (GetFileAttributesW((user_dir + name).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

std::wstring size_mb(std::uint64_t bytes) {
    return std::to_wstring((bytes + 1024 * 1024 - 1) / (1024 * 1024));
}

std::wstring fill(std::wstring text, std::initializer_list<std::wstring> values) {
    int i = 0;
    for (const std::wstring& value : values) {
        const std::wstring mark = L"{" + std::to_wstring(i++) + L"}";
        for (size_t at = text.find(mark); at != std::wstring::npos; at = text.find(mark, at + value.size())) {
            text.replace(at, mark.size(), value);
        }
    }
    return text;
}

std::wstring pick_folder(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
    dialog->SetTitle(tr("general.data_pick"));
    std::wstring path;
    IShellItem* result = nullptr;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            path = name;
            CoTaskMemFree(name);
        }
        result->Release();
    }
    dialog->Release();
    return path;
}

// The installed program (C:\Program Files\ZhiyiIME\<version>\ -> its base folder).
std::wstring install_base() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path exe(std::wstring(buf, n));
    return with_slash(exe.parent_path().parent_path().wstring());
}

}  // namespace

void EditorApp::create_data_card(HWND panel, int y) {
    const int x0 = kPanelPadLeft;
    y = card_begin(panel, y, tr("general.card_data"));
    const int x = make_aligned_label(tr("general.data_location"), x0,
                                     label_width({"general.data_location"}), y, panel);
    RECT client = {};
    GetClientRect(panel, &client);
    hDataFolder_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                                   x, y + S(4), client.right - x - S(16), S(kFontPt + 8), panel,
                                   nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hDataFolder_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    y += kRowH;
    hDataHint_ = make_hint(tr("general.data_hint"), x, y - S(6), S(500), panel, 1);
    y += S(22);
    hDataChange_ = make_button(kDataChangeId, tr("general.data_change"), x, y, S(110), panel);
    hDataDefault_ = make_button(kDataDefaultId, tr("general.data_default"), x + S(120), y, S(110), panel);
    y += kRowH;
    card_end(panel, y);
    show_data_folder();
}

void EditorApp::show_data_folder() {
    if (!hDataFolder_) return;
    const std::wstring chosen = utf8_to_wstr(chosen_data_folder());
    const bool unavailable = chosen_data_folder_unavailable();
    const std::wstring shown = unavailable ? with_slash(chosen) + L"zhiyi\\"
                                           : utf8_to_wstr(user_data_dir());
    SetWindowTextW(hDataFolder_, shown.c_str());
    if (!data_moving_) {
        SetWindowTextW(hDataHint_, unavailable ? fill(tr("general.data_unavailable"),
                                                      {utf8_to_wstr(default_user_data_dir())}).c_str()
                                               : tr("general.data_hint"));
    }
    EnableWindow(hDataChange_, !data_moving_);
    EnableWindow(hDataDefault_, !data_moving_ && !chosen.empty());
}

void EditorApp::choose_data_folder(bool to_default) {
    if (data_moving_) return;
    std::wstring folder;
    if (!to_default) {
        folder = pick_folder(hwnd_);
        if (folder.empty()) return;
        folder = full_path(folder);
        while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    }
    const DataPlaces from = current_places();
    const DataPlaces to = places_for(folder);
    if (lower(from.user) == lower(to.user)) return;  // already there

    // Not into the data itself, the program, or the system.
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    if (path_within(to.user, from.user) || path_within(from.user, to.user) ||
        path_within(to.user, from.local) || path_within(to.user, install_base()) ||
        path_within(to.user, with_slash(windows))) {
        MessageBoxW(hwnd_, tr("general.data_bad_folder"), tr("window.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    // Writable?
    std::error_code error;
    fs::create_directories(to.user, error);
    const std::wstring probe = to.user + L".zhiyi-write-test";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(to.user.c_str());
        MessageBoxW(hwnd_, tr("general.data_not_writable"), tr("window.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    CloseHandle(file);

    // Data there already (a reinstall): use it, or replace it with the current data.
    bool use_existing = false;
    if (has_zhiyi_data(to.user)) {
        const int answer = MessageBoxW(hwnd_, fill(tr("general.data_existing"), {to.user}).c_str(),
                                       tr("window.title"), MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return;
        use_existing = answer == IDYES;
    }
    const fs::path skip = nested_local(from);
    const std::uint64_t bytes = use_existing ? 0
                                             : data_size(from.user, skip) + data_size(from.local, {});
    ULARGE_INTEGER free = {};
    if (!use_existing && GetDiskFreeSpaceExW(to.user.c_str(), &free, nullptr, nullptr) &&
        free.QuadPart < bytes + 64ull * 1024 * 1024) {
        MessageBoxW(hwnd_, fill(tr("general.data_no_space"), {size_mb(bytes)}).c_str(),
                    tr("window.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(hwnd_,
                    fill(tr(use_existing ? "general.data_confirm_existing" : "general.data_confirm"),
                         {to.user, size_mb(bytes), from.user})
                        .c_str(),
                    tr("window.title"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    // Unsaved choices on the pages are saved first: the server is about to restart.
    if (!save_config()) return;

    data_moving_ = true;
    SetWindowTextW(hDataHint_, tr("general.data_moving"));
    show_data_folder();
    const HWND window = hwnd_;
    std::thread([window, from, to, folder, use_existing, bytes] {
        auto* done = new DataMoveDone;
        // Held until the server may load again (it waits for it at start).
        HANDLE moving = CreateMutexW(nullptr, TRUE, kDataMoveMutex);
        if (!stop_server()) {
            done->error = tr("general.data_server_busy");
        } else {
            DWORD last_post = 0;
            const DataMoveResult result = move_data(
                from, to, use_existing, [&] { return set_chosen_data_folder(wstr_to_utf8(folder)); },
                [&](std::uint64_t copied) {
                    if (GetTickCount() - last_post < 200) return;
                    last_post = GetTickCount();
                    PostMessageW(window, kDataProgressMessage, static_cast<WPARAM>(copied >> 20),
                                 static_cast<LPARAM>(bytes >> 20));
                });
            done->ok = result.ok;
            done->left = result.left;
            if (!result.ok) done->error = tr("general.data_copy_failed");
        }
        if (moving) {
            ReleaseMutex(moving);
            CloseHandle(moving);
        }
        start_server_on_demand();
        if (!PostMessageW(window, kDataMovedMessage, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

bool EditorApp::handle_data_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kDataProgressMessage) {
        SetWindowTextW(hDataHint_, fill(tr("general.data_progress"),
                                        {std::to_wstring(wparam), std::to_wstring(lparam)}).c_str());
        return true;
    }
    if (message != kDataMovedMessage) return false;
    std::unique_ptr<DataMoveDone> done(reinterpret_cast<DataMoveDone*>(lparam));
    data_moving_ = false;
    show_data_folder();
    if (!done->ok) {
        MessageBoxW(hwnd_, done->error.c_str(), tr("window.title"), MB_OK | MB_ICONERROR);
    } else if (done->left > 0) {
        MessageBoxW(hwnd_, fill(tr("general.data_left"), {std::to_wstring(done->left)}).c_str(),
                    tr("window.title"), MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

}  // namespace settings
}  // namespace cxxime
