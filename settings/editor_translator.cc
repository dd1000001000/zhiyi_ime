// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// "Offline translation" on the Learning page (docs/learning-mode.md): download, update and
// remove the translation model (update.h: the release tagged "translator"; translator_files.h:
// where it goes), turn it on, and choose where it runs: a discrete graphics card Vulkan offers
// with about 1 GB of its own memory (list_translator_gpus, gpu_adapters.h;
// learning.translator_min_gpu_memory_mb), or the CPU, offered
// last. Turning it on or choosing another one first translates a word there, and a choice the
// model cannot run on is not kept; on a card, a few pages are then timed there and on the CPU.

#include "editor_app.h"

#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
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

struct MtTested {
    bool works = false;
    int device_ms = 0;  // median page on the chosen card; 0: not timed (the CPU was chosen)
    int cpu_ms = 0;     // the same pages on the CPU; 0: not timed or it failed
};

// Three pages of seven common words, each page different so no answer comes from a cache.
const std::vector<std::vector<mt::Item>>& timing_pages() {
    static const std::vector<std::vector<mt::Item>> pages = [] {
        const char* words[3][7] = {
            {"现在", "朋友", "天气", "学习", "发展", "电脑", "工作"},
            {"晚饭", "城市", "音乐", "医生", "周末", "问题", "照片"},
            {"火车", "公司", "新闻", "时间", "颜色", "超市", "作业"},
        };
        std::vector<std::vector<mt::Item>> out(3);
        for (int p = 0; p < 3; ++p) {
            for (const char* word : words[p]) out[p].push_back({"en", word});
        }
        return out;
    }();
    return pages;
}

// The median time of a page on a started server, in ms; 0 when a page fails.
int time_pages(mt::LlamaServer& server) {
    std::vector<int> times;
    for (const auto& page : timing_pages()) {
        const auto start = std::chrono::steady_clock::now();
        const auto out = server.translate(page, {});
        if (out.empty() || out[0].text.empty()) return 0;
        times.push_back(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count()));
    }
    std::sort(times.begin(), times.end());
    return (std::max)(1, times[times.size() / 2]);
}

// A version as Settings shows it after "v": the manifest's name ("4-1026") when it has one.
std::wstring shown_version(std::uint32_t version, const std::string& name) {
    return name.empty() ? std::to_wstring(version) : utf8_to_wstr(name);
}

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
    auto progress = [&](std::uint64_t done, std::uint64_t size) {
        if (done < size && GetTickCount() - last_post < 200) return;  // the end always shows
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
    // Downloaded files stay in `downloads` until the whole install is done, so a failed try
    // (e.g. the runtime download) does not lose the model already downloaded.
    auto fetch = [&](const update::TranslatorFile& file, const fs::path& path) {
        if (fs::exists(path, error) && same_sha256(path, file.sha256)) {
            progress(file.size, file.size);
            return update::Status::kOk;
        }
        return update::download_translator_file(file, path.wstring(), progress, cancel);
    };
    if (!fs::exists(model, error)) {
        const fs::path download = downloads / fs::u8path(manifest.model.file);
        result.status = fetch(manifest.model, download);
        if (result.status != update::Status::kOk) return result;
        if (!CreateHardLinkW(model.c_str(), download.c_str(), nullptr)) {
            fs::copy_file(download, model, error);
            if (error) {
                result.status = update::Status::kDisk;
                return result;
            }
        }
    }
    base = manifest.model.size;
    const fs::path archive = downloads / fs::u8path(manifest.runtime.file);
    result.status = fetch(manifest.runtime, archive);
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
    if (!manifest.name.empty()) state["name"] = manifest.name;
    const fs::path json = root / kTranslatorInstalled;
    {
        std::ofstream out(fs::path(json.wstring() + L".tmp"), std::ios::binary | std::ios::trunc);
        out << state.dump(2) << "\n";
    }
    if (!MoveFileExW((json.wstring() + L".tmp").c_str(), json.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        result.status = update::Status::kDisk;
        return result;
    }
    // Older versions and the downloads go. zhiyi-server may still run the previous model: it is
    // ended, and zhiyi-server starts the new one for its next page (machine_translator.cc).
    fs::remove_all(downloads, error);
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (!entry.is_directory() || entry.path() == final_dir) continue;
        end_model_servers(entry.path() / L"");
        fs::remove_all(entry.path(), error);
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
    fill_translator_devices();
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

// The cards are asked from Vulkan (about 0.15 s) only once the model is here: before, the box
// is disabled and lists the CPU alone.
void EditorApp::fill_translator_devices() {
    mt_choices_ = mt::find_installed(nullptr)
                      ? cached_translator_gpus(static_cast<std::uint64_t>(config_.mt_min_gpu_memory_mb))
                      : std::vector<GpuAdapter>{};
    const size_t cards = mt_choices_.size();
    GpuAdapter cpu;
    cpu.name = cached_cpu_info().name;
    cpu.key = mt::kCpuDevice;
    mt_choices_.push_back(cpu);
    SendMessageW(hTranslatorDevice_, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < mt_choices_.size(); ++i) {
        std::wstring label = i == cards ? L"CPU" : cards > 1 ? L"GPU " + std::to_wstring(i + 1) : L"GPU";
        if (!mt_choices_[i].name.empty()) label += L"（" + utf8_to_wstr(mt_choices_[i].name) + L"）";
        combo_add(hTranslatorDevice_, label.c_str());
    }
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
            status = fill(tr("learning.translator_installed"), {shown_version(installed.version, installed.name)});
            if (mt_remote_known_ && mt_remote_.version > installed.version) {
                status += L" · " + fill(tr("learning.has_update"), {shown_version(mt_remote_.version, mt_remote_.name)});
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
        label = fill(tr("learning.update_to"), {shown_version(mt_remote_.version, mt_remote_.name)});
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
    if (hTranslatorHint_) SetWindowTextW(hTranslatorHint_, tr("learning.translator_hint"));
    const HWND window = hwnd_;
    const std::string key = mt_choices_[static_cast<size_t>(index)].key;
    std::thread([window, installed, key] {
        auto tested = std::make_unique<MtTested>();
        {
            mt::LlamaServer server;
            std::string error;
            tested->works = server.start(installed, key, &error);
            if (tested->works) {
                const auto out = server.translate({{"en", "\xE4\xBD\xA0\xE5\xA5\xBD"}}, {});  // 你好
                tested->works = !out.empty() && !out[0].text.empty();
            }
            // An old card may run the model slower than the processor: time both.
            if (tested->works && key != mt::kCpuDevice) tested->device_ms = time_pages(server);
        }
        if (tested->device_ms > 0) {
            mt::LlamaServer cpu;
            std::string error;
            if (cpu.start(installed, mt::kCpuDevice, &error)) tested->cpu_ms = time_pages(cpu);
        }
        PostMessageW(window, kMtTestedMessage, 0, reinterpret_cast<LPARAM>(tested.release()));
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
            fill_translator_devices();  // the cards, now that the model is here
            populate_translator();
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
    case kMtTestedMessage: {
        std::unique_ptr<MtTested> tested(reinterpret_cast<MtTested*>(lparam));
        mt_busy_ = MtBusy::kNone;
        if (!tested->works) {
            set_check(hTranslator_, false);
            show_translator_state();
            MessageBoxW(hwnd_, tr("learning.translator_card_failed"), tr("window.title"),
                        MB_OK | MB_ICONINFORMATION);
            return true;
        }
        show_translator_state();
        if (hTranslatorHint_ && tested->device_ms > 0 && tested->cpu_ms > 0) {
            const bool slower = tested->device_ms > tested->cpu_ms;
            SetWindowTextW(hTranslatorHint_,
                           fill(tr(slower ? "learning.translator_slower" : "learning.translator_speed"),
                                {std::to_wstring(tested->device_ms), std::to_wstring(tested->cpu_ms)})
                               .c_str());
        }
        return true;
    }
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
