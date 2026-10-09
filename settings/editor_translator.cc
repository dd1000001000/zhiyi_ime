// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// "Offline translation" on the Learning page (docs/learning-mode.md): download, update and
// remove the translation model (update.h: the release tagged "translator"; translator_files.h:
// where it goes), turn it on, and choose where it runs: a graphics card with more than 2 GB of
// its own memory (gpu_adapters.h), or the CPU, offered last. Turning it on or choosing another
// one first translates a word there, and a choice the model cannot run on is not kept.

#include "editor_app.h"

#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <json.hpp>
#include <miniz.h>

#include <cxxime/data_path.h>
#include <cxxime/machine_translation.h>
#include <cxxime/translator_files.h>
#include <cxxime/version.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

namespace fs = std::filesystem;

constexpr UINT kMtCheckedMessage = WM_APP + 80;    // lParam: MtChecked*
constexpr UINT kMtProgressMessage = WM_APP + 81;   // wParam: MB done, lParam: MB in all
constexpr UINT kMtInstalledMessage = WM_APP + 82;  // lParam: MtInstalled*
constexpr UINT kMtTestedMessage = WM_APP + 83;     // wParam: 1 when the card works

struct MtChecked {
    update::TranslatorCheckResult result;
};

struct MtInstalled {
    update::Status status = update::Status::kNetwork;
    std::wstring error;  // shown when not empty
};

fs::path translator_root() { return fs::path(utf8_to_wstr(local_data_dir())) / kTranslatorDir; }

std::wstring fill(std::wstring text, std::initializer_list<std::wstring> values) {
    int i = 0;
    for (const std::wstring& value : values) {
        const std::wstring mark = L"{" + std::to_wstring(i++) + L"}";
        const size_t at = text.find(mark);
        if (at != std::wstring::npos) text.replace(at, mark.size(), value);
    }
    return text;
}

std::wstring mb(std::uint64_t bytes) { return std::to_wstring((bytes + (1 << 19)) >> 20); }

// The model server of an installed version may be running (zhiyi-server starts it): end it so
// its files can be deleted.
void end_model_servers(const fs::path& under) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry = {sizeof(entry)};
    const std::wstring prefix = under.wstring();
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"llama-server.exe") != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                     FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size) &&
            _wcsnicmp(path, prefix.c_str(), prefix.size()) == 0) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 5000);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
}

// Unpacks the runtime archive into `to`; entries must stay inside it.
bool unzip(const fs::path& archive, const fs::path& to) {
    std::ifstream in(archive, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), {});
    mz_zip_archive zip = {};
    if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return false;
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; ok && i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            break;
        }
        const std::string name = stat.m_filename;
        if (name.empty() || name.find("..") != std::string::npos || name[0] == '/' ||
            name[0] == '\\' || name.find(':') != std::string::npos) {
            ok = false;
            break;
        }
        const fs::path target = to / fs::u8path(name);
        std::error_code error;
        if (stat.m_is_directory) {
            fs::create_directories(target, error);
            continue;
        }
        fs::create_directories(target.parent_path(), error);
        size_t size = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!bytes) {
            ok = false;
            break;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        ok = static_cast<bool>(out);
        mz_free(bytes);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// Same file content? (the installed model is kept for an update that does not change it)
bool same_sha256(const fs::path& path, const std::string& sha256) {
    std::string hash;
    return update::sha256_file(path.wstring(), &hash) && hash == sha256;
}

// Downloads and installs `manifest` as translator\v<N> and points translator.json at it.
MtInstalled install(const update::TranslatorManifest& manifest, const std::atomic<bool>* cancel,
                    HWND window) {
    MtInstalled result;
    const fs::path root = translator_root();
    const fs::path downloads = root / L"download";
    const std::wstring version = L"v" + std::to_wstring(manifest.version);
    const fs::path staging = root / (version + L".new");
    const fs::path final_dir = root / version;
    std::error_code error;
    fs::create_directories(downloads, error);
    fs::remove_all(staging, error);
    fs::create_directories(staging / kTranslatorRuntimeDir, error);

    const std::uint64_t total = manifest.model.size + manifest.runtime.size;
    std::uint64_t base = 0;
    DWORD last_post = 0;
    auto progress = [&](std::uint64_t done, std::uint64_t) {
        if (GetTickCount() - last_post < 200) return;
        last_post = GetTickCount();
        PostMessageW(window, kMtProgressMessage, static_cast<WPARAM>((base + done) >> 20),
                     static_cast<LPARAM>(total >> 20));
    };
    // The model: kept from the installed version when it is the same file.
    mt::InstalledModel installed;
    const fs::path model = staging / fs::u8path(manifest.model.file);
    if (mt::find_installed(&installed) && same_sha256(installed.model, manifest.model.sha256)) {
        if (!CreateHardLinkW(model.c_str(), installed.model.c_str(), nullptr)) {
            fs::copy_file(installed.model, model, error);
        }
    }
    if (!fs::exists(model, error)) {
        const fs::path download = downloads / fs::u8path(manifest.model.file);
        result.status = update::download_translator_file(manifest.model, download.wstring(), progress, cancel);
        if (result.status != update::Status::kOk) return result;
        fs::rename(download, model, error);
        if (error) {
            result.status = update::Status::kDisk;
            return result;
        }
    }
    base = manifest.model.size;
    const fs::path archive = downloads / fs::u8path(manifest.runtime.file);
    result.status = update::download_translator_file(manifest.runtime, archive.wstring(), progress, cancel);
    if (result.status != update::Status::kOk) return result;
    if (!unzip(archive, staging / kTranslatorRuntimeDir) ||
        !fs::exists(staging / kTranslatorRuntimeDir / kTranslatorServerExe, error)) {
        result.status = update::Status::kInvalid;
        return result;
    }
    // Into place: the version folder, then translator.json (written beside it and swapped).
    end_model_servers(final_dir);
    fs::remove_all(final_dir, error);
    fs::rename(staging, final_dir, error);
    if (error) {
        result.status = update::Status::kDisk;
        return result;
    }
    nlohmann::json state = {{"version", manifest.version},
                            {"dir", wstr_to_utf8(version)},
                            {"model", manifest.model.file}};
    if (!manifest.prompt.empty()) state["prompt"] = manifest.prompt;
    const fs::path json = root / kTranslatorInstalled;
    {
        std::ofstream out(fs::path(json.wstring() + L".tmp"), std::ios::binary | std::ios::trunc);
        out << state.dump(2) << "\n";
    }
    if (!MoveFileExW((json.wstring() + L".tmp").c_str(), json.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        result.status = update::Status::kDisk;
        return result;
    }
    // Older versions and the downloads go (a version still in use stays until next time).
    fs::remove_all(downloads, error);
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (entry.is_directory() && entry.path() != final_dir) fs::remove_all(entry.path(), error);
    }
    result.status = update::Status::kOk;
    return result;
}

}  // namespace

void EditorApp::create_translator_card(HWND panel, int y) {
    const int x0 = kPanelPadLeft;
    RECT client = {};
    GetClientRect(panel, &client);
    y = card_begin(panel, y, tr("learning.card_translator"));
    hTranslator_ = make_check(kTranslatorId, tr("learning.translator_enable"), x0, y, S(520), panel);
    y += kRowH;
    const int labels = label_width({"learning.translator_device"});
    const int device_x = make_aligned_label(tr("learning.translator_device"), x0 + S(20), labels, y, panel);
    hTranslatorDevice_ = make_combo(kTranslatorDeviceId, device_x, y, S(420), panel);
    mt_choices_ = list_gpu_adapters();
    const size_t cards = mt_choices_.size();
    GpuAdapter cpu;
    cpu.name = cpu_info().name;
    cpu.key = mt::kCpuDevice;
    mt_choices_.push_back(cpu);
    for (size_t i = 0; i < mt_choices_.size(); ++i) {
        std::wstring label = i == cards ? L"CPU" : cards > 1 ? L"GPU " + std::to_wstring(i + 1) : L"GPU";
        if (!mt_choices_[i].name.empty()) label += L"（" + utf8_to_wstr(mt_choices_[i].name) + L"）";
        combo_add(hTranslatorDevice_, label.c_str());
    }
    y += kRowH;
    hTranslatorStatus_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                         x0, y + S(5), client.right - x0 - S(240), S(kFontPt + 8), panel,
                                         nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hTranslatorStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hTranslatorAction_ = make_button(kTranslatorActionId, L"", client.right - x0 - S(206), y, S(120), panel);
    hTranslatorRemove_ = make_button(kTranslatorRemoveId, tr("learning.remove"), client.right - x0 - S(78), y,
                                     S(78), panel);
    y += kRowH;
    hTranslatorHint_ = make_hint(tr("learning.translator_hint"), x0, y - S(4), client.right - x0 * 2, panel);
    y += S(40);
    card_end(panel, y);
    show_translator_state();
}

void EditorApp::populate_translator() {
    if (!hTranslator_) return;
    set_check(hTranslator_, config_.mt_enable);
    int index = 0;
    for (size_t i = 0; i < mt_choices_.size(); ++i) {
        if (mt_choices_[i].key == config_.mt_device) index = static_cast<int>(i);
    }
    combo_set_index(hTranslatorDevice_, index);
    show_translator_state();
}

void EditorApp::read_translator(Config& config) {
    if (!hTranslator_) return;
    const int index = combo_index(hTranslatorDevice_);
    config.mt_enable = mt::find_installed(nullptr) && get_check(hTranslator_);
    if (index >= 0 && index < static_cast<int>(mt_choices_.size())) {
        config.mt_device = mt_choices_[static_cast<size_t>(index)].key;
    }
}

void EditorApp::show_translator_state() {
    if (!hTranslatorStatus_) return;
    mt::InstalledModel installed;
    const bool have = mt::find_installed(&installed);
    std::wstring status;
    const wchar_t* action = nullptr;
    switch (mt_busy_) {
    case MtBusy::kChecking:
        status = tr("learning.checking");
        break;
    case MtBusy::kDownloading:
        status = fill(tr("learning.downloading"), {std::to_wstring(mt_done_mb_), std::to_wstring(mt_total_mb_)});
        action = tr("learning.cancel");
        break;
    case MtBusy::kTesting:
        status = tr("learning.translator_testing");
        break;
    case MtBusy::kNone:
        if (!have) {
            status = tr("learning.translator_missing");
            action = tr("learning.download");
        } else {
            status = fill(tr("learning.translator_installed"), {std::to_wstring(installed.version)});
            if (mt_remote_known_ && mt_remote_.version > installed.version) {
                status += L" · " + fill(tr("learning.has_update"), {std::to_wstring(mt_remote_.version)});
                action = tr("learning.update_to");
            } else {
                if (mt_remote_known_) status += L" · " + std::wstring(tr("learning.latest"));
                action = tr("learning.check");
            }
        }
        break;
    }
    if (!mt_note_.empty() && mt_busy_ == MtBusy::kNone) status += L" · " + mt_note_;
    SetWindowTextW(hTranslatorStatus_, status.c_str());
    std::wstring label = action ? action : L"";
    if (mt_busy_ == MtBusy::kNone && have && mt_remote_known_ && mt_remote_.version > installed.version) {
        label = fill(tr("learning.update_to"), {std::to_wstring(mt_remote_.version)});
    }
    SetWindowTextW(hTranslatorAction_, label.c_str());
    ShowWindow(hTranslatorAction_, action ? SW_SHOW : SW_HIDE);
    EnableWindow(hTranslatorAction_, mt_busy_ != MtBusy::kTesting && mt_busy_ != MtBusy::kChecking);
    ShowWindow(hTranslatorRemove_, have && mt_busy_ == MtBusy::kNone ? SW_SHOW : SW_HIDE);
    EnableWindow(hTranslator_, have && mt_busy_ == MtBusy::kNone);
    if (!have) set_check(hTranslator_, false);
    EnableWindow(hTranslatorDevice_, have && mt_busy_ == MtBusy::kNone && get_check(hTranslator_));
}

void EditorApp::start_translator_check() {
    mt_busy_ = MtBusy::kChecking;
    mt_note_.clear();
    show_translator_state();
    const HWND window = hwnd_;
    std::thread([window] {
        auto* checked = new MtChecked{update::check_translator()};
        if (!PostMessageW(window, kMtCheckedMessage, 0, reinterpret_cast<LPARAM>(checked))) delete checked;
    }).detach();
}

void EditorApp::start_translator_install() {
    mt_busy_ = MtBusy::kDownloading;
    mt_note_.clear();
    mt_done_mb_ = 0;
    mt_total_mb_ = 0;
    mt_cancel_ = std::make_shared<std::atomic<bool>>(false);
    show_translator_state();
    const HWND window = hwnd_;
    const auto cancel = mt_cancel_;
    std::thread([window, cancel] {
        auto* done = new MtInstalled;
        const update::TranslatorCheckResult checked = update::check_translator();
        if (checked.status != update::Status::kOk) {
            done->status = checked.status;
        } else if (!checked.manifest.min_app.empty() &&
                   update::is_newer_release(checked.manifest.min_app, CXXIME_VERSION_STRING)) {
            done->status = update::Status::kInvalid;
            done->error = fill(tr("learning.translator_needs_app"), {utf8_to_wstr(checked.manifest.min_app)});
        } else {
            *done = install(checked.manifest, cancel.get(), window);
        }
        if (!PostMessageW(window, kMtInstalledMessage, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

void EditorApp::remove_translator() {
    if (MessageBoxW(hwnd_, tr("learning.translator_remove_confirm"), tr("window.title"),
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    const fs::path root = translator_root();
    end_model_servers(root);
    std::error_code error;
    fs::remove_all(root, error);
    set_check(hTranslator_, false);
    mt_remote_known_ = false;
    mt_note_ = error ? tr("learning.translator_remove_later") : L"";
    show_translator_state();
}

void EditorApp::start_translator_test() {
    const int index = combo_index(hTranslatorDevice_);
    mt::InstalledModel installed;
    if (index < 0 || index >= static_cast<int>(mt_choices_.size()) || !mt::find_installed(&installed)) return;
    mt_busy_ = MtBusy::kTesting;
    show_translator_state();
    const HWND window = hwnd_;
    const std::string key = mt_choices_[static_cast<size_t>(index)].key;
    std::thread([window, installed, key] {
        mt::LlamaServer server;
        std::string error;
        bool works = server.start(installed, key, &error);
        if (works) {
            const auto out = server.translate({{"en", "\xE4\xBD\xA0\xE5\xA5\xBD"}}, {});  // 你好
            works = !out.empty() && !out[0].empty();
        }
        server.stop();
        PostMessageW(window, kMtTestedMessage, works ? 1 : 0, 0);
    }).detach();
}

bool EditorApp::handle_translator_command(int control_id, int notification) {
    switch (control_id) {
    case kTranslatorId:
        if (notification == BN_CLICKED) {
            show_translator_state();
            if (get_check(hTranslator_)) start_translator_test();
        }
        return true;
    case kTranslatorDeviceId:
        if (notification == CBN_SELCHANGE && get_check(hTranslator_)) start_translator_test();
        return true;
    case kTranslatorActionId:
        if (notification != BN_CLICKED) return true;
        if (mt_busy_ == MtBusy::kDownloading) {
            if (mt_cancel_) mt_cancel_->store(true);
        } else if (mt_busy_ == MtBusy::kNone) {
            mt::InstalledModel installed;
            const bool have = mt::find_installed(&installed);
            if (!have || (mt_remote_known_ && mt_remote_.version > installed.version)) {
                start_translator_install();
            } else {
                start_translator_check();
            }
        }
        return true;
    case kTranslatorRemoveId:
        if (notification == BN_CLICKED && mt_busy_ == MtBusy::kNone) remove_translator();
        return true;
    default:
        return false;
    }
}

bool EditorApp::handle_translator_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kMtCheckedMessage: {
        std::unique_ptr<MtChecked> checked(reinterpret_cast<MtChecked*>(lparam));
        mt_busy_ = MtBusy::kNone;
        if (checked->result.status == update::Status::kOk) {
            mt_remote_ = checked->result.manifest;
            mt_remote_known_ = true;
        } else {
            mt_note_ = checked->result.status == update::Status::kInvalid ? tr("learning.failed_invalid")
                                                                           : tr("learning.failed_network");
        }
        show_translator_state();
        return true;
    }
    case kMtProgressMessage:
        mt_done_mb_ = static_cast<std::uint64_t>(wparam);
        mt_total_mb_ = static_cast<std::uint64_t>(lparam);
        show_translator_state();
        return true;
    case kMtInstalledMessage: {
        std::unique_ptr<MtInstalled> done(reinterpret_cast<MtInstalled*>(lparam));
        mt_busy_ = MtBusy::kNone;
        mt_cancel_.reset();
        mt_remote_known_ = false;
        if (done->status == update::Status::kOk) {
            mt_note_.clear();
            show_translator_state();
            set_check(hTranslator_, true);  // downloaded to be used: on, after a test where it runs
            start_translator_test();
        } else {
            mt_note_ = !done->error.empty() ? done->error
                       : done->status == update::Status::kCancelled ? tr("learning.cancelled")
                       : done->status == update::Status::kDisk      ? tr("learning.failed_disk")
                       : done->status == update::Status::kInvalid   ? tr("learning.failed_invalid")
                                                                     : tr("learning.failed_network");
            show_translator_state();
        }
        return true;
    }
    case kMtTestedMessage:
        mt_busy_ = MtBusy::kNone;
        if (wparam == 0) {
            set_check(hTranslator_, false);
            show_translator_state();
            MessageBoxW(hwnd_, tr("learning.translator_card_failed"), tr("window.title"),
                        MB_OK | MB_ICONINFORMATION);
        } else {
            show_translator_state();
        }
        return true;
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
