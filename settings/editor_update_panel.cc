// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Settings > Updates: the reminder switch (Config::update_notify, checked when settings opens),
// a check by hand, and the new version with its notes and an Update button. Updating downloads
// the installer, checks it (update.h) and starts it in update mode; settings then closes, and
// the installer opens it again on this page.

#include "editor_app.h"

#include <initializer_list>
#include <thread>

#include <cxxime/version.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

enum ControlId {
    kUpdateNotifyId = 1401,
    kCheckUpdateId,
    kInstallUpdateId,
};

constexpr UINT kUpdateCheckedMessage = WM_APP + 40;     // lParam: update::CheckResult*
constexpr UINT kUpdateProgressMessage = WM_APP + 41;    // wParam done, lParam total (bytes)
constexpr UINT kUpdateDownloadedMessage = WM_APP + 42;  // lParam: DownloadResult*

constexpr int kPromptUpdateId = 100;
constexpr int kPromptLaterId = 101;
constexpr int kPromptSkipId = 102;
constexpr size_t kPromptNotesLimit = 600;

struct DownloadResult {
    update::Status status = update::Status::kNetwork;
    std::wstring path;
};

// tr() text with {0}, {1} replaced.
std::wstring format(const wchar_t* pattern, std::initializer_list<std::wstring> values) {
    std::wstring text = pattern;
    int index = 0;
    for (const std::wstring& value : values) {
        const std::wstring field = L"{" + std::to_wstring(index++) + L"}";
        const size_t at = text.find(field);
        if (at != std::wstring::npos) text.replace(at, field.size(), value);
    }
    return text;
}

std::wstring megabytes(std::uint64_t bytes) {
    wchar_t text[32];
    swprintf(text, 32, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// Line breaks for an EDIT control.
std::wstring windows_lines(const std::wstring& text) {
    std::wstring lines;
    for (const wchar_t ch : text) {
        if (ch == L'\r') continue;
        if (ch == L'\n') lines += L'\r';
        lines += ch;
    }
    return lines;
}

HWND make_text(const wchar_t* text, int x, int y, int width, int height, HWND parent) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, width,
                                   height, parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return control;
}

}  // namespace

void EditorApp::create_update_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    int y = kPanelPadTop;
    hUpdateNotify_ = make_check(kUpdateNotifyId, tr("update.notify"), x0, y, S(460), panel);
    y += kRowH;
    make_hint(tr("update.notify_hint"), x0 + S(20), y - S(6), S(440), panel);
    y += kRowH + S(8);
    make_text(format(tr("update.current"), {CXXIME_VERSION_WSTRING}).c_str(), x0, y, S(460),
              kCtrlH, panel);
    y += kRowH;
    hCheckUpdate_ = make_button(kCheckUpdateId, tr("update.check"), x0, y, S(120), panel);
    hUpdateStatus_ = make_text(L"", x0 + S(132), y + S(4), S(330), S(40), panel);
    y += S(42);
    hNewVersion_ = make_text(L"", x0, y, S(300), kCtrlH, panel);
    hReleaseLink_ = make_web_link(kReleaseLinkId, tr("update.view_release"), x0 + S(300), y,
                                  S(160), panel);
    y += S(28);
    hReleaseNotes_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, x0, y,
        S(456), S(76), panel, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hReleaseNotes_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    y += S(84);
    hInstallUpdate_ = make_button(kInstallUpdateId, tr("update.install"), x0, y, S(120), panel);
    hUpdateProgress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD, x0 + S(132),
                                       y + S(6), S(324), S(16), panel, nullptr,
                                       GetModuleHandle(nullptr), nullptr);
    SendMessageW(hUpdateProgress_, PBM_SETRANGE32, 0, 1000);
    hInstallHint_ = make_hint(tr("update.install_hint"), x0 + S(132), y + S(5), S(324), panel, 1);
    show_update_state();
}

void EditorApp::show_update_state() {
    if (!hUpdateStatus_) return;
    SetWindowTextW(hUpdateStatus_, update_status_.c_str());
    EnableWindow(hCheckUpdate_, update_busy_ == UpdateBusy::kNone);
    const bool downloading = update_busy_ == UpdateBusy::kDownloading;
    for (HWND control : {hNewVersion_, hReleaseLink_, hReleaseNotes_, hInstallUpdate_}) {
        ShowWindow(control, have_update_ ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(hInstallHint_, have_update_ && !downloading ? SW_SHOW : SW_HIDE);
    ShowWindow(hUpdateProgress_, have_update_ && downloading ? SW_SHOW : SW_HIDE);
    SetWindowTextW(hInstallUpdate_, downloading ? tr("update.cancel") : tr("update.install"));
    EnableWindow(hInstallUpdate_, update_busy_ != UpdateBusy::kChecking);
    if (!have_update_) return;
    std::wstring title = format(tr("update.available"), {utf8_to_wstr(update_manifest_.version)});
    if (!update_manifest_.published.empty()) {
        title += L" · " + utf8_to_wstr(update_manifest_.published);
    }
    SetWindowTextW(hNewVersion_, title.c_str());
    const std::string notes = update_manifest_.notes_for(ui_language_);
    SetWindowTextW(hReleaseNotes_,
                   notes.empty() ? tr("update.no_notes") : windows_lines(utf8_to_wstr(notes)).c_str());
    const std::uint64_t total = update_total_ ? update_total_ : 1;
    SendMessageW(hUpdateProgress_, PBM_SETPOS, static_cast<WPARAM>(update_done_ * 1000 / total), 0);
}

// At start: the result of an update started from here, then the check when reminders are on.
void EditorApp::init_update() {
    update::State state = update::read_state();
    if (!state.pending_version.empty()) {
        if (state.pending_version == CXXIME_VERSION_STRING) {
            update_status_ = format(tr("update.updated"), {CXXIME_VERSION_WSTRING});
            initial_panel_ = cxxime::SettingsPanel::kUpdate;
            update::clean_downloads(update::download_directory());
        }
        state.pending_version.clear();  // else the installation did not finish
        update::write_state(state);
    }
}

void EditorApp::start_update_check(bool automatic) {
    if (update_busy_ != UpdateBusy::kNone) return;
    update_busy_ = UpdateBusy::kChecking;
    if (!automatic) update_status_ = tr("update.checking");
    show_update_state();
    const HWND window = hwnd_;
    std::thread([window, automatic] {
        auto* result = new update::CheckResult(update::check_latest(CXXIME_VERSION_STRING));
        if (!PostMessageW(window, kUpdateCheckedMessage, automatic,
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void EditorApp::on_update_checked(bool automatic, const update::CheckResult& result) {
    update_busy_ = UpdateBusy::kNone;
    if (result.status == update::Status::kOk && result.newer) {
        const bool same = have_update_ && update_manifest_.version == result.manifest.version;
        update_manifest_ = result.manifest;
        if (!same) update_done_ = 0;
        update_total_ = update_manifest_.size;
        have_update_ = true;
        update_status_ = format(tr("update.available_status"),
                                {utf8_to_wstr(update_manifest_.version)});
        show_update_state();
        if (automatic && config_.update_notify && !update_prompted_ &&
            update::read_state().skipped_version != update_manifest_.version) {
            update_prompted_ = true;
            show_update_prompt();
        }
        return;
    }
    if (automatic) {  // quiet: only a check by hand reports problems
        show_update_state();
        return;
    }
    switch (result.status) {
    case update::Status::kOk: update_status_ = tr("update.latest"); break;
    case update::Status::kNotFound: update_status_ = tr("update.not_found"); break;
    case update::Status::kInvalid: update_status_ = tr("update.invalid"); break;
    default: update_status_ = tr("update.network_failed"); break;
    }
    show_update_state();
}

void EditorApp::show_update_prompt() {
    const std::wstring main = format(tr("update.prompt_main"),
                                     {utf8_to_wstr(update_manifest_.version),
                                      CXXIME_VERSION_WSTRING});
    std::wstring notes = utf8_to_wstr(update_manifest_.notes_for(ui_language_));
    if (notes.size() > kPromptNotesLimit) notes = notes.substr(0, kPromptNotesLimit) + L"…";
    const std::wstring content = notes.empty() ? std::wstring(tr("update.no_notes")) : notes;
    const TASKDIALOG_BUTTON buttons[] = {
        {kPromptUpdateId, tr("update.install")},
        {kPromptLaterId, tr("update.prompt_later")},
        {kPromptSkipId, tr("update.prompt_skip")},
    };
    TASKDIALOGCONFIG dialog = {sizeof(dialog)};
    dialog.hwndParent = hwnd_;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = tr("window.title");
    dialog.pszMainIcon = TD_INFORMATION_ICON;
    dialog.pszMainInstruction = main.c_str();
    dialog.pszContent = content.c_str();
    dialog.pButtons = buttons;
    dialog.cButtons = ARRAYSIZE(buttons);
    dialog.nDefaultButton = kPromptUpdateId;
    int pressed = kPromptLaterId;
    if (FAILED(TaskDialogIndirect(&dialog, &pressed, nullptr, nullptr))) return;
    if (pressed == kPromptUpdateId) {
        show_panel(kUpdatePanel);
        start_update_download();
    } else if (pressed == kPromptSkipId) {
        update::State state = update::read_state();
        state.skipped_version = update_manifest_.version;
        update::write_state(state);
    }
}

void EditorApp::start_update_download() {
    if (update_busy_ != UpdateBusy::kNone || !have_update_) return;
    update_busy_ = UpdateBusy::kDownloading;
    update_total_ = update_manifest_.size;
    update_status_ = format(tr("update.downloading"),
                            {megabytes(update_done_), megabytes(update_total_)});
    update_cancel_ = std::make_shared<std::atomic<bool>>(false);
    show_update_state();
    const HWND window = hwnd_;
    const update::Manifest manifest = update_manifest_;
    const std::shared_ptr<std::atomic<bool>> cancel = update_cancel_;
    std::thread([window, manifest, cancel] {
        auto* result = new DownloadResult;
        result->status = update::download_installer(
            manifest, update::download_directory(),
            [window](std::uint64_t done, std::uint64_t total) {
                PostMessageW(window, kUpdateProgressMessage, static_cast<WPARAM>(done),
                             static_cast<LPARAM>(total));
            },
            cancel.get(), &result->path);
        if (!PostMessageW(window, kUpdateDownloadedMessage, 0,
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void EditorApp::on_update_downloaded(const std::wstring& path, update::Status status) {
    update_busy_ = UpdateBusy::kNone;
    update_cancel_.reset();
    switch (status) {
    case update::Status::kOk: break;
    case update::Status::kCancelled: update_status_ = tr("update.cancelled"); break;
    case update::Status::kInvalid: update_status_ = tr("update.verify_failed"); break;
    case update::Status::kDisk: update_status_ = tr("update.disk_failed"); break;
    default: update_status_ = tr("update.download_failed"); break;
    }
    if (status != update::Status::kOk) {
        if (status == update::Status::kInvalid) update_done_ = 0;
        show_update_state();
        return;
    }
    update_status_ = tr("update.starting");
    show_update_state();
    UpdateWindow(hwnd_);
    // Remembered first: the installer opens settings again, which then reports the update.
    update::State state = update::read_state();
    state.pending_version = update_manifest_.version;
    update::write_state(state);
    DWORD error = ERROR_SUCCESS;
    if (update::launch_installer(path, update_manifest_.sha256, hwnd_, &error)) {
        DestroyWindow(hwnd_);  // the installer replaces this program
        return;
    }
    state.pending_version.clear();
    update::write_state(state);
    update_status_ = error == ERROR_CANCELLED ? tr("update.install_cancelled")
                                              : tr("update.launch_failed");
    show_update_state();
}

bool EditorApp::handle_update_command(int control_id, int notification) {
    if (notification != BN_CLICKED) return false;
    switch (control_id) {
    case kCheckUpdateId:
        start_update_check(false);
        return true;
    case kInstallUpdateId:
        if (update_busy_ == UpdateBusy::kDownloading) {
            if (update_cancel_) update_cancel_->store(true);
        } else {
            start_update_download();
        }
        return true;
    default:
        return false;
    }
}

bool EditorApp::handle_update_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kUpdateCheckedMessage: {
        std::unique_ptr<update::CheckResult> result(
            reinterpret_cast<update::CheckResult*>(lparam));
        on_update_checked(wparam != 0, *result);
        return true;
    }
    case kUpdateProgressMessage:
        if (update_busy_ == UpdateBusy::kDownloading) {
            update_done_ = static_cast<std::uint64_t>(wparam);
            update_total_ = static_cast<std::uint64_t>(lparam);
            update_status_ = update_done_ >= update_total_
                                 ? std::wstring(tr("update.verifying"))
                                 : format(tr("update.downloading"),
                                          {megabytes(update_done_), megabytes(update_total_)});
            show_update_state();
        }
        return true;
    case kUpdateDownloadedMessage: {
        std::unique_ptr<DownloadResult> result(reinterpret_cast<DownloadResult*>(lparam));
        on_update_downloaded(result->path, result->status);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
