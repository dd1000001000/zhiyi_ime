// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The Voice input page (docs/voice-input.md): turn it on, the voice key set on the Keys page,
// download, update and remove the model (update.h: the release tagged "voice"; voice_files.h:
// where it goes), and where and how it recognizes: the CPU or a graphics card (DirectML), the
// language, numbers as digits. The longest recording (voice.max_seconds) is only in the file.
//
// Choosing a card times the model there and on the CPU (voice::time_recognition, 5 s of sound)
// as the General page does for the recommendation model; the hint shows both, in the warning
// color when the card is slower (the choice stays). A downloaded model is timed on the chosen
// device right away. Results are kept per card, driver and model version in voice-gpu.json in
// local_data_dir(); a card the model cannot run on goes back to the CPU and is not offered
// again until its driver or the model changes.

#include "editor_app.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/gpu_adapters.h>
#include <cxxime/version.h>
#include <cxxime/voice_files.h>
#include <cxxime/voice_recognizer.h>

#include "editor_app_internal.h"
#include "i18n.h"
#include "key_capture.h"

namespace cxxime {
namespace settings {

namespace {

namespace fs = std::filesystem;

enum ControlId {
    kVoiceEnableId = 5801,
    kVoiceKeysLinkId,
    kVoiceActionId,
    kVoiceRemoveId,
    kVoiceDeviceId,
    kVoiceLanguageId,
    kVoiceItnId,
};

constexpr UINT kVoiceCheckedMessage = WM_APP + 100;    // lParam: VoiceChecked*
constexpr UINT kVoiceProgressMessage = WM_APP + 101;   // wParam: MB done, lParam: MB in all
constexpr UINT kVoiceInstalledMessage = WM_APP + 102;  // lParam: VoiceInstalled*
constexpr UINT kVoiceTestedMessage = WM_APP + 103;     // lParam: VoiceTested*

// voice.language in combo order.
const char* const kLanguages[] = {"auto", "zh", "en", "yue"};
const char* const kLanguageLabels[] = {"voice.lang_auto", "voice.lang_zh", "voice.lang_en",
                                       "voice.lang_yue"};

struct VoiceChecked {
    update::VoiceCheckResult result;
};

struct VoiceInstalled {
    update::Status status = update::Status::kNetwork;
    std::wstring error;  // shown when not empty
};

struct VoiceTested {
    bool card = false;    // a card was timed (else only the CPU)
    GpuAdapter adapter;   // that card
    std::string model;    // model_id() of what was timed
    double gpu_ms = 0.0;  // 0: it does not run on the card
    double cpu_ms = 0.0;  // 0: not timed, or it failed
};

fs::path voice_root() { return fs::path(utf8_to_wstr(local_data_dir())) / voice::kVoiceDir; }

// What the speed tests are kept for: the installed model's version ("" without one).
std::string model_id() {
    voice::InstalledModel installed;
    return voice::find_installed(&installed) ? "sensevoice-v" + std::to_string(installed.version) : "";
}

std::wstring test_state_path() { return utf8_to_wstr(local_data_path("voice-gpu.json")); }

nlohmann::json read_test_state() {
    std::ifstream file{fs::path(test_state_path())};
    nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (!state.is_object() || !state.contains("tests") || !state["tests"].is_array()) {
        state = {{"tests", nlohmann::json::array()}};
    }
    return state;
}

// The recorded test of this card with this driver and model, or nullptr.
const nlohmann::json* find_test(const nlohmann::json& state, const GpuAdapter& adapter, const std::string& model) {
    for (const auto& test : state["tests"]) {
        if (test.is_object() && test.value("key", "") == adapter.key &&
            test.value("driver", std::uint64_t{0}) == adapter.driver_version && test.value("model", "") == model) {
            return &test;
        }
    }
    return nullptr;
}

void remember_test(const VoiceTested& tested) {
    nlohmann::json state = read_test_state();
    auto& tests = state["tests"];
    tests.erase(std::remove_if(tests.begin(), tests.end(),
                               [&](const nlohmann::json& test) {
                                   return test.is_object() && test.value("key", "") == tested.adapter.key;
                               }),
                tests.end());
    tests.push_back({{"key", tested.adapter.key},
                     {"name", tested.adapter.name},
                     {"driver", tested.adapter.driver_version},
                     {"model", tested.model},
                     {"gpu_ms", tested.gpu_ms},
                     {"cpu_ms", tested.cpu_ms}});
    const std::wstring path = test_state_path();
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::ofstream file{fs::path(path), std::ios::binary | std::ios::trunc};
    file << state.dump(2) << "\n";
}

std::wstring format_ms(const wchar_t* text, double gpu_ms, double cpu_ms) {
    std::wstring out = text;
    const std::pair<const wchar_t*, double> values[] = {{L"{0}", gpu_ms}, {L"{1}", cpu_ms}};
    for (const auto& [mark, ms] : values) {
        const size_t at = out.find(mark);
        if (at != std::wstring::npos) out.replace(at, 3, std::to_wstring(static_cast<int>(ms + 0.5)));
    }
    return out;
}

std::wstring fill(std::wstring text, std::initializer_list<std::wstring> values) {
    int i = 0;
    for (const std::wstring& value : values) {
        const std::wstring mark = L"{" + std::to_wstring(i++) + L"}";
        const size_t at = text.find(mark);
        if (at != std::wstring::npos) text.replace(at, mark.size(), value);
    }
    return text;
}

bool same_sha256(const fs::path& path, const std::string& sha256) {
    std::string hash;
    return update::sha256_file(path.wstring(), &hash) && hash == sha256;
}

// Downloads `manifest` into voice\v<N> and points voice.json at it. Files the installed version
// already has (same SHA-256) are linked, not downloaded again.
VoiceInstalled install(const update::VoiceManifest& manifest, const std::atomic<bool>* cancel,
                       HWND window) {
    VoiceInstalled result;
    const fs::path root = voice_root();
    const fs::path downloads = root / L"download";
    const std::wstring version = L"v" + std::to_wstring(manifest.version);
    const fs::path staging = root / (version + L".new");
    const fs::path final_dir = root / version;
    std::error_code error;
    fs::create_directories(downloads, error);
    fs::remove_all(staging, error);
    fs::create_directories(staging, error);

    const std::uint64_t total = manifest.total_size();
    std::uint64_t base = 0;
    DWORD last_post = 0;
    auto progress = [&](std::uint64_t done, std::uint64_t size) {
        if (done < size && GetTickCount() - last_post < 200) return;  // the end always shows
        last_post = GetTickCount();
        PostMessageW(window, kVoiceProgressMessage, static_cast<WPARAM>((base + done) >> 20),
                     static_cast<LPARAM>(total >> 20));
    };
    voice::InstalledModel installed;
    const bool have = voice::find_installed(&installed);
    for (const update::TranslatorFile& file : manifest.files) {
        const fs::path target = staging / fs::u8path(file.file);
        const fs::path old = have ? fs::path(installed.dir) / fs::u8path(file.file) : fs::path();
        if (have && fs::exists(old, error) && same_sha256(old, file.sha256)) {
            if (!CreateHardLinkW(target.c_str(), old.c_str(), nullptr)) fs::copy_file(old, target, error);
        }
        if (!fs::exists(target, error)) {
            // Downloads stay until the whole install is done: a failed try keeps what came.
            const fs::path download = downloads / fs::u8path(file.file);
            if (fs::exists(download, error) && same_sha256(download, file.sha256)) {
                progress(file.size, file.size);
            } else {
                result.status = update::download_translator_file(file, download.wstring(), progress, cancel);
                if (result.status != update::Status::kOk) return result;
            }
            if (!CreateHardLinkW(target.c_str(), download.c_str(), nullptr)) {
                fs::copy_file(download, target, error);
                if (error) {
                    result.status = update::Status::kDisk;
                    return result;
                }
            }
        }
        base += file.size;
    }
    // Into place: the version folder, then voice.json (written beside it and swapped).
    fs::remove_all(final_dir, error);
    fs::rename(staging, final_dir, error);
    if (error) {
        result.status = update::Status::kDisk;
        return result;
    }
    const nlohmann::json state = {{"version", manifest.version}, {"dir", wstr_to_utf8(version)}};
    const fs::path json = root / voice::kVoiceInstalled;
    {
        std::ofstream out(fs::path(json.wstring() + L".tmp"), std::ios::binary | std::ios::trunc);
        out << state.dump(2) << "\n";
    }
    if (!MoveFileExW((json.wstring() + L".tmp").c_str(), json.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        result.status = update::Status::kDisk;
        return result;
    }
    // Older versions and the downloads go (a version the server still has loaded stays until
    // next time).
    fs::remove_all(downloads, error);
    for (const auto& entry : fs::directory_iterator(root, error)) {
        if (entry.is_directory() && entry.path() != final_dir) fs::remove_all(entry.path(), error);
    }
    result.status = update::Status::kOk;
    return result;
}

}  // namespace

void EditorApp::create_voice_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    RECT client = {};
    GetClientRect(panel, &client);
    const int labels = label_width({"voice.key", "voice.device", "voice.language"});

    int y = card_begin(panel, kPanelPadTop, tr("voice.card"));
    hVoiceEnable_ = make_check(kVoiceEnableId, tr("voice.enable"), x0, y, S(440), panel);
    y += kRowH;
    const int key_x = make_aligned_label(tr("voice.key"), x0, labels, y, panel);
    hVoiceKeyText_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, key_x,
                                     y + S(5), S(200), S(kFontPt + 8), panel, nullptr,
                                     GetModuleHandle(nullptr), nullptr);
    SendMessageW(hVoiceKeyText_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    make_button(kVoiceKeysLinkId, tr("voice.key_set"), key_x + S(210), y, S(150), panel);
    y += kRowH;
    make_hint(tr("voice.usage"), x0, y - S(2), S(560), panel, 3);
    y += S(3 * (kFontPt + 6) + 6);
    y = card_end(panel, y);

    y = card_begin(panel, y, tr("voice.card_model"));
    hVoiceStatus_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                    x0, y + S(5), client.right - x0 - S(240), S(kFontPt + 8), panel,
                                    nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hVoiceStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    hVoiceAction_ = make_button(kVoiceActionId, L"", client.right - x0 - S(206), y, S(120), panel);
    hVoiceRemove_ = make_button(kVoiceRemoveId, tr("learning.remove"), client.right - x0 - S(78), y,
                                S(78), panel);
    y += kRowH;
    make_hint(tr("voice.model_hint"), x0, y - S(4), S(560), panel, 2);
    y += S(2 * (kFontPt + 6) + 8);

    const int device_x = make_aligned_label(tr("voice.device"), x0, labels, y, panel);
    hVoiceDevice_ = make_combo(kVoiceDeviceId, device_x, y, S(420), panel);
    fill_voice_devices();
    y += kRowH;
    hVoiceDeviceHint_ = make_hint(tr("voice.device_hint"), device_x, y - S(6), S(440), panel, 2);
    y += S(2 * (kFontPt + 6));
    const int language_x = make_aligned_label(tr("voice.language"), x0, labels, y, panel);
    hVoiceLanguage_ = make_combo(kVoiceLanguageId, language_x, y, S(300), panel);
    for (const char* key : kLanguageLabels) combo_add(hVoiceLanguage_, tr(key));
    y += kRowH;
    hVoiceItn_ = make_check(kVoiceItnId, tr("voice.itn"), x0, y, S(520), panel);
    y += kRowH;
    card_end(panel, y + S(4));
    show_voice_state();
}

// "CPU（…）", then the cards DirectML offers ("GPU（…）", "GPU 1（…）" …) as on the General page,
// without those the model could not run on (with this driver and model).
void EditorApp::fill_voice_devices() {
    voice_choices_.clear();
    const nlohmann::json state = read_test_state();
    const std::string model = model_id();
    for (GpuAdapter& adapter : list_gpu_adapters()) {
        const nlohmann::json* test = find_test(state, adapter, model);
        if (!test || test->value("gpu_ms", -1.0) > 0) voice_choices_.push_back(std::move(adapter));
    }
    auto named = [](const char* key, const std::wstring& name, const std::wstring& count) {
        std::wstring text = tr(key);
        for (const auto& [mark, value] : {std::pair{L"{0}", &name}, std::pair{L"{1}", &count}}) {
            const size_t at = text.find(mark);
            if (at != std::wstring::npos) text.replace(at, 3, *value);
        }
        return text;
    };
    SendMessageW(hVoiceDevice_, CB_RESETCONTENT, 0, 0);
    const CpuInfo cpu = cpu_info();
    const std::wstring cpu_name = cpu.name.empty() ? L"?" : utf8_to_wstr(cpu.name);
    combo_add(hVoiceDevice_, (cpu.packages > 1 ? named("general.device_cpus", cpu_name, std::to_wstring(cpu.packages))
                                               : named("general.device_cpu", cpu_name, L""))
                                 .c_str());
    for (std::size_t i = 0; i < voice_choices_.size(); ++i) {
        const std::wstring name = utf8_to_wstr(voice_choices_[i].name);
        combo_add(hVoiceDevice_, (voice_choices_.size() > 1 ? named("general.device_gpu_n", name, std::to_wstring(i + 1))
                                                            : named("general.device_gpu", name, L""))
                                     .c_str());
    }
}

void EditorApp::show_voice_device_hint(const wchar_t* text, bool warning) {
    if (!hVoiceDeviceHint_) return;
    voice_device_warning_ = warning;
    SetWindowTextW(hVoiceDeviceHint_, text);
    InvalidateRect(hVoiceDeviceHint_, nullptr, TRUE);
}

// The hint for the device chosen now: a card's recorded times, else what choosing one does.
void EditorApp::show_voice_device() {
    const int index = combo_index(hVoiceDevice_);
    if (index > 0 && index <= static_cast<int>(voice_choices_.size())) {
        const nlohmann::json state = read_test_state();
        if (const nlohmann::json* test = find_test(state, voice_choices_[index - 1], model_id())) {
            const double gpu_ms = test->value("gpu_ms", 0.0), cpu_ms = test->value("cpu_ms", 0.0);
            const bool slower = gpu_ms >= cpu_ms;
            show_voice_device_hint(
                format_ms(tr(slower ? "voice.device_slower" : "voice.device_result"), gpu_ms, cpu_ms).c_str(),
                slower);
            return;
        }
    }
    if (index <= 0 && voice_cpu_ms_ > 0) {
        show_voice_device_hint(format_ms(tr("voice.device_cpu_result"), 0, voice_cpu_ms_).c_str(), false);
        return;
    }
    show_voice_device_hint(tr(voice_choices_.empty() ? "voice.device_hint_cpu" : "voice.device_hint"), false);
}

// Times the chosen device: a card and then the CPU, or the CPU alone.
void EditorApp::start_voice_test() {
    voice::InstalledModel installed;
    if (voice_busy_ != VoiceBusy::kNone || !voice::find_installed(&installed)) return;
    const int index = combo_index(hVoiceDevice_);
    auto* tested = new VoiceTested;
    tested->model = model_id();
    if (index > 0 && index <= static_cast<int>(voice_choices_.size())) {
        tested->card = true;
        tested->adapter = voice_choices_[index - 1];
    }
    voice_busy_ = VoiceBusy::kTesting;
    show_voice_state();
    show_voice_device_hint(tr("voice.device_testing"), false);
    const HWND window = hwnd_;
    std::thread([window, installed, tested] {
        std::string error;
        if (tested->card) {
            tested->gpu_ms = voice::time_recognition(installed.model, installed.tokens,
                                                     static_cast<int>(tested->adapter.index), &error);
        }
        if (!tested->card || tested->gpu_ms > 0) {
            tested->cpu_ms = voice::time_recognition(installed.model, installed.tokens, -1, &error);
        }
        if (!PostMessageW(window, kVoiceTestedMessage, 0, reinterpret_cast<LPARAM>(tested))) delete tested;
    }).detach();
}

void EditorApp::populate_voice() {
    if (!hVoiceEnable_) return;
    set_check(hVoiceEnable_, config_.voice_enable && voice::find_installed(nullptr));
    int device = 0;
    for (std::size_t i = 0; i < voice_choices_.size(); ++i) {
        if (voice_choices_[i].key == config_.voice_device) device = static_cast<int>(i) + 1;
    }
    combo_set_index(hVoiceDevice_, device);
    show_voice_device();
    int language = 0;
    for (int i = 0; i < static_cast<int>(std::size(kLanguages)); ++i) {
        if (config_.voice_language == kLanguages[i]) language = i;
    }
    combo_set_index(hVoiceLanguage_, language);
    set_check(hVoiceItn_, config_.voice_itn);
    show_voice_key();
    show_voice_state();
}

void EditorApp::read_voice(Config& config) {
    if (!hVoiceEnable_) return;
    config.voice_enable = voice::find_installed(nullptr) && get_check(hVoiceEnable_);
    const int device = combo_index(hVoiceDevice_);
    config.voice_device = device > 0 && device <= static_cast<int>(voice_choices_.size())
                              ? voice_choices_[static_cast<std::size_t>(device - 1)].key
                              : std::string();
    const int language = std::clamp(combo_index(hVoiceLanguage_), 0, static_cast<int>(std::size(kLanguages)) - 1);
    config.voice_language = kLanguages[language];
    config.voice_itn = get_check(hVoiceItn_);
}

// The voice key as the Keys page has it now (saved or not).
void EditorApp::show_voice_key() {
    if (!hVoiceKeyText_) return;
    const KeyChoice choice = hVoiceKey_ ? key_capture_get(hVoiceKey_) : KeyChoice::none();
    SetWindowTextW(hVoiceKeyText_, key_choice_text(choice, tr("voice.key_none")).c_str());
}

void EditorApp::show_voice_state() {
    if (!hVoiceStatus_) return;
    voice::InstalledModel installed;
    const bool have = voice::find_installed(&installed);
    std::wstring status;
    const wchar_t* action = nullptr;
    switch (voice_busy_) {
    case VoiceBusy::kChecking:
        status = tr("learning.checking");
        break;
    case VoiceBusy::kTesting:
        status = fill(tr("voice.installed"), {std::to_wstring(installed.version)});
        break;
    case VoiceBusy::kDownloading:
        status = fill(tr("learning.downloading"), {std::to_wstring(voice_done_mb_), std::to_wstring(voice_total_mb_)});
        action = tr("learning.cancel");
        break;
    case VoiceBusy::kNone:
        if (!have) {
            status = tr("voice.missing");
            action = tr("learning.download");
        } else {
            status = fill(tr("voice.installed"), {std::to_wstring(installed.version)});
            if (voice_remote_known_ && voice_remote_.version > installed.version) {
                status += L" · " + fill(tr("learning.has_update"), {std::to_wstring(voice_remote_.version)});
                action = tr("learning.update_to");
            } else {
                if (voice_remote_known_) status += L" · " + std::wstring(tr("learning.latest"));
                action = tr("learning.check");
            }
        }
        break;
    }
    if (!voice_note_.empty() && voice_busy_ == VoiceBusy::kNone) status += L" · " + voice_note_;
    SetWindowTextW(hVoiceStatus_, status.c_str());
    std::wstring label = action ? action : L"";
    if (voice_busy_ == VoiceBusy::kNone && have && voice_remote_known_ &&
        voice_remote_.version > installed.version) {
        label = fill(tr("learning.update_to"), {std::to_wstring(voice_remote_.version)});
    }
    SetWindowTextW(hVoiceAction_, label.c_str());
    ShowWindow(hVoiceAction_, action ? SW_SHOW : SW_HIDE);
    EnableWindow(hVoiceAction_, voice_busy_ != VoiceBusy::kChecking && voice_busy_ != VoiceBusy::kTesting);
    ShowWindow(hVoiceRemove_, have && voice_busy_ == VoiceBusy::kNone ? SW_SHOW : SW_HIDE);
    // Voice input needs the model; the device is timed with it.
    EnableWindow(hVoiceEnable_, have && voice_busy_ == VoiceBusy::kNone);
    EnableWindow(hVoiceDevice_, have && voice_busy_ == VoiceBusy::kNone);
    if (!have) set_check(hVoiceEnable_, false);
}

void EditorApp::start_voice_check() {
    voice_busy_ = VoiceBusy::kChecking;
    voice_note_.clear();
    show_voice_state();
    const HWND window = hwnd_;
    std::thread([window] {
        auto* checked = new VoiceChecked{update::check_voice()};
        if (!PostMessageW(window, kVoiceCheckedMessage, 0, reinterpret_cast<LPARAM>(checked))) delete checked;
    }).detach();
}

void EditorApp::start_voice_install() {
    voice_busy_ = VoiceBusy::kDownloading;
    voice_note_.clear();
    voice_done_mb_ = 0;
    voice_total_mb_ = 0;
    voice_cancel_ = std::make_shared<std::atomic<bool>>(false);
    show_voice_state();
    const HWND window = hwnd_;
    const auto cancel = voice_cancel_;
    std::thread([window, cancel] {
        auto* done = new VoiceInstalled;
        const update::VoiceCheckResult checked = update::check_voice();
        if (checked.status != update::Status::kOk) {
            done->status = checked.status;
        } else if (!checked.manifest.min_app.empty() &&
                   update::is_newer_release(checked.manifest.min_app, CXXIME_VERSION_STRING)) {
            done->status = update::Status::kInvalid;
            done->error = fill(tr("voice.needs_app"), {utf8_to_wstr(checked.manifest.min_app)});
        } else {
            *done = install(checked.manifest, cancel.get(), window);
        }
        if (!PostMessageW(window, kVoiceInstalledMessage, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

void EditorApp::remove_voice() {
    if (MessageBoxW(hwnd_, tr("voice.remove_confirm"), tr("window.title"), MB_OKCANCEL | MB_ICONQUESTION) !=
        IDOK) {
        return;
    }
    std::error_code error;
    fs::remove_all(voice_root(), error);
    set_check(hVoiceEnable_, false);
    voice_remote_known_ = false;
    voice_cpu_ms_ = 0;
    voice_note_ = error ? tr("learning.translator_remove_later") : L"";
    show_voice_state();
    show_voice_device();
}

bool EditorApp::handle_voice_command(int control_id, int notification) {
    switch (control_id) {
    case kVoiceEnableId:
        if (notification == BN_CLICKED) show_voice_state();
        return true;
    case kVoiceKeysLinkId:
        if (notification == BN_CLICKED) show_panel(2);  // the Keys page
        return true;
    case kVoiceDeviceId:
        if (notification == CBN_SELCHANGE) {
            // A card not timed yet (with this driver and model) is timed now.
            const int index = combo_index(hVoiceDevice_);
            const nlohmann::json state = read_test_state();
            if (index > 0 && index <= static_cast<int>(voice_choices_.size()) &&
                !find_test(state, voice_choices_[index - 1], model_id())) {
                start_voice_test();
            } else {
                show_voice_device();
            }
        }
        return true;
    case kVoiceLanguageId:
    case kVoiceItnId:
        return true;
    case kVoiceActionId:
        if (notification != BN_CLICKED) return true;
        if (voice_busy_ == VoiceBusy::kDownloading) {
            if (voice_cancel_) voice_cancel_->store(true);
        } else if (voice_busy_ == VoiceBusy::kNone) {
            voice::InstalledModel installed;
            const bool have = voice::find_installed(&installed);
            if (!have || (voice_remote_known_ && voice_remote_.version > installed.version)) {
                start_voice_install();
            } else {
                start_voice_check();
            }
        }
        return true;
    case kVoiceRemoveId:
        if (notification == BN_CLICKED && voice_busy_ == VoiceBusy::kNone) remove_voice();
        return true;
    default:
        return false;
    }
}

bool EditorApp::handle_voice_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kVoiceCheckedMessage: {
        std::unique_ptr<VoiceChecked> checked(reinterpret_cast<VoiceChecked*>(lparam));
        voice_busy_ = VoiceBusy::kNone;
        if (checked->result.status == update::Status::kOk) {
            voice_remote_ = checked->result.manifest;
            voice_remote_known_ = true;
        } else {
            voice_note_ = checked->result.status == update::Status::kInvalid ? tr("learning.failed_invalid")
                                                                              : tr("learning.failed_network");
        }
        show_voice_state();
        return true;
    }
    case kVoiceProgressMessage:
        voice_done_mb_ = static_cast<std::uint64_t>(wparam);
        voice_total_mb_ = static_cast<std::uint64_t>(lparam);
        show_voice_state();
        return true;
    case kVoiceInstalledMessage: {
        std::unique_ptr<VoiceInstalled> done(reinterpret_cast<VoiceInstalled*>(lparam));
        voice_busy_ = VoiceBusy::kNone;
        voice_cancel_.reset();
        voice_remote_known_ = false;
        if (done->status == update::Status::kOk) {
            voice_note_.clear();
            set_check(hVoiceEnable_, true);  // downloaded to be used: on (saved with the others)
            // Another model: the cards are offered and timed again.
            const int index = combo_index(hVoiceDevice_);
            const std::string chosen = index > 0 && index <= static_cast<int>(voice_choices_.size())
                                           ? voice_choices_[index - 1].key
                                           : std::string();
            fill_voice_devices();
            int again = 0;
            for (std::size_t i = 0; i < voice_choices_.size(); ++i) {
                if (voice_choices_[i].key == chosen) again = static_cast<int>(i) + 1;
            }
            combo_set_index(hVoiceDevice_, again);
            voice_cpu_ms_ = 0;
            show_voice_state();
            start_voice_test();  // also shows that the model runs
            return true;
        } else {
            voice_note_ = !done->error.empty() ? done->error
                          : done->status == update::Status::kCancelled ? tr("learning.cancelled")
                          : done->status == update::Status::kDisk      ? tr("voice.failed_disk")
                          : done->status == update::Status::kInvalid   ? tr("learning.failed_invalid")
                                                                        : tr("learning.failed_network");
        }
        show_voice_state();
        return true;
    }
    case kVoiceTestedMessage: {
        std::unique_ptr<VoiceTested> tested(reinterpret_cast<VoiceTested*>(lparam));
        voice_busy_ = VoiceBusy::kNone;
        show_voice_state();
        if (!tested->card) {
            voice_cpu_ms_ = tested->cpu_ms;
            if (tested->cpu_ms > 0) {
                show_voice_device();
            } else {
                show_voice_device_hint(tr("voice.model_failed"), true);
            }
            return true;
        }
        remember_test(*tested);
        if (tested->gpu_ms > 0) {  // it ran: the choice stays
            show_voice_device();
            return true;
        }
        // The model does not run on this card: back to the CPU, and the card is no longer offered.
        const auto it = std::find_if(voice_choices_.begin(), voice_choices_.end(),
                                     [&](const GpuAdapter& a) { return a.key == tested->adapter.key; });
        if (it != voice_choices_.end()) {
            SendMessageW(hVoiceDevice_, CB_DELETESTRING, static_cast<WPARAM>(it - voice_choices_.begin() + 1), 0);
            voice_choices_.erase(it);
        }
        combo_set_index(hVoiceDevice_, 0);
        show_voice_device();
        MessageBoxW(hwnd_, tr("voice.device_failed"), tr("window.title"), MB_OK | MB_ICONINFORMATION);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
