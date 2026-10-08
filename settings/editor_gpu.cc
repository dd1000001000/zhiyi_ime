// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// "Device" row on the General page (docs/settings-guide.md): the recommendation model can run
// on a graphics card (laya.device). The row is shown only when this computer has a card that
// may run it (gpu_adapters.h). Choosing a card runs a speed test here: the model runs on the
// card and on the CPU in turn, and the hint shows both times, in the warning color when the
// card is slower (the choice stays). The result is remembered per card and driver in
// laya-gpu.json in local_data_dir() (machine-specific, so not in the user data that backups
// carry); a card the model cannot run on goes back to the CPU and is not offered
// again until its driver changes.

#include "editor_app.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <thread>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/laya_rerank.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

constexpr UINT kGpuTestedMessage = WM_APP + 60;  // lParam: GpuTestDone*

struct GpuTestDone {
    GpuAdapter adapter;
    LayaGpuTest result;
};

std::wstring gpu_state_path() { return utf8_to_wstr(local_data_path("laya-gpu.json")); }

nlohmann::json read_gpu_state() {
    std::ifstream file(gpu_state_path());
    nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (!state.is_object() || !state.contains("tests") || !state["tests"].is_array()) {
        state = {{"tests", nlohmann::json::array()}};
    }
    return state;
}

// The recorded test of this card with this driver, or nullptr.
const nlohmann::json* find_test(const nlohmann::json& state, const GpuAdapter& adapter) {
    for (const auto& test : state["tests"]) {
        if (test.is_object() && test.value("key", "") == adapter.key &&
            test.value("driver", std::uint64_t{0}) == adapter.driver_version) {
            return &test;
        }
    }
    return nullptr;
}

void remember_test(const GpuAdapter& adapter, const LayaGpuTest& result) {
    nlohmann::json state = read_gpu_state();
    auto& tests = state["tests"];
    tests.erase(std::remove_if(tests.begin(), tests.end(),
                               [&](const nlohmann::json& test) {
                                   return test.is_object() && test.value("key", "") == adapter.key;
                               }),
                tests.end());
    tests.push_back({{"key", adapter.key},
                     {"name", adapter.name},
                     {"driver", adapter.driver_version},
                     {"faster", result.faster},
                     {"gpu_ms", result.gpu_ms},
                     {"cpu_ms", result.cpu_ms}});
    const std::wstring path = gpu_state_path();
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
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

std::wstring with_values(const wchar_t* text, const std::wstring& name, const std::wstring& count) {
    std::wstring out = text;
    const std::pair<const wchar_t*, const std::wstring*> values[] = {{L"{0}", &name}, {L"{1}", &count}};
    for (const auto& [mark, value] : values) {
        const size_t at = out.find(mark);
        if (at != std::wstring::npos) out.replace(at, 3, *value);
    }
    return out;
}

}  // namespace

// "CPU（AMD Ryzen 9 9950X …）", "CPU（2 × Intel Xeon …）" for two sockets (one choice: the CPU
// provider uses all of them); "GPU（…）", or "GPU 1（…）", "GPU 2（…）" when there are several.
void EditorApp::fill_device_combo() {
    const CpuInfo cpu = cpu_info();
    const std::wstring cpu_name = cpu.name.empty() ? L"?" : utf8_to_wstr(cpu.name);
    combo_add(hDevice_, (cpu.packages > 1
                             ? with_values(tr("general.device_cpus"), cpu_name, std::to_wstring(cpu.packages))
                             : with_values(tr("general.device_cpu"), cpu_name, L""))
                            .c_str());
    for (size_t i = 0; i < gpu_choices_.size(); ++i) {
        const std::wstring name = utf8_to_wstr(gpu_choices_[i].name);
        combo_add(hDevice_, (gpu_choices_.size() > 1
                                 ? with_values(tr("general.device_gpu_n"), name, std::to_wstring(i + 1))
                                 : with_values(tr("general.device_gpu"), name, L""))
                                .c_str());
    }
}

// Cards the model ran on stay offered, slower ones too (the hint warns); a card it could not
// run on is not offered again until its driver changes.
void EditorApp::load_gpu_choices() {
    gpu_choices_.clear();
    const nlohmann::json state = read_gpu_state();
    for (GpuAdapter& adapter : list_gpu_adapters()) {
        const nlohmann::json* test = find_test(state, adapter);
        if (!test || test->value("gpu_ms", -1.0) > 0) {
            gpu_choices_.push_back(std::move(adapter));
        }
    }
}

// The hint under the row: the measured times, in the warning color when the card is slower
// than the CPU (the choice stays; the user decides).
void EditorApp::show_device_result(double gpu_ms, double cpu_ms) {
    device_warning_ = gpu_ms >= cpu_ms;
    SetWindowTextW(hDeviceHint_, format_ms(tr(device_warning_ ? "general.device_slower" : "general.device_result"),
                                           gpu_ms, cpu_ms).c_str());
    InvalidateRect(hDeviceHint_, nullptr, TRUE);
}

void EditorApp::show_device_hint(const wchar_t* text) {
    device_warning_ = false;
    SetWindowTextW(hDeviceHint_, text);
    InvalidateRect(hDeviceHint_, nullptr, TRUE);
}

void EditorApp::populate_device() {
    if (!hDevice_) return;
    int index = 0;  // the CPU
    for (size_t i = 0; i < gpu_choices_.size(); ++i) {
        if (gpu_choices_[i].key == config_.laya.device) index = static_cast<int>(i) + 1;
    }
    combo_set_index(hDevice_, index);
    // A card's recorded speed, when it is the one in use.
    const nlohmann::json state = read_gpu_state();
    const nlohmann::json* test = index > 0 ? find_test(state, gpu_choices_[index - 1]) : nullptr;
    if (test) {
        show_device_result(test->value("gpu_ms", 0.0), test->value("cpu_ms", 0.0));
    } else {
        show_device_hint(tr("general.device_hint"));
    }
}

std::string EditorApp::selected_device() const {
    if (!hDevice_) return config_.laya.device;  // no row: keep what the file says
    const int index = combo_index(hDevice_);
    return index > 0 && index <= static_cast<int>(gpu_choices_.size()) ? gpu_choices_[index - 1].key
                                                                       : std::string();
}

void EditorApp::on_device_selected() {
    const int index = combo_index(hDevice_);
    if (index <= 0 || index > static_cast<int>(gpu_choices_.size())) {
        show_device_hint(tr("general.device_hint"));
        return;
    }
    const GpuAdapter adapter = gpu_choices_[index - 1];
    const nlohmann::json state = read_gpu_state();
    if (const nlohmann::json* test = find_test(state, adapter)) {  // tested with this driver
        show_device_result(test->value("gpu_ms", 0.0), test->value("cpu_ms", 0.0));
        return;
    }
    gpu_testing_ = true;
    update_enabled_controls();
    show_device_hint(tr("general.device_testing"));
    read_controls(false);
    const Config config = config_;
    const HWND window = hwnd_;
    std::thread([window, config, adapter] {
        auto* done = new GpuTestDone{adapter, LayaRerank::instance().test_gpu(config, adapter.key)};
        if (!PostMessageW(window, kGpuTestedMessage, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

bool EditorApp::handle_gpu_message(UINT message, WPARAM, LPARAM lparam) {
    if (message != kGpuTestedMessage) return false;
    std::unique_ptr<GpuTestDone> done(reinterpret_cast<GpuTestDone*>(lparam));
    gpu_testing_ = false;
    remember_test(done->adapter, done->result);
    if (done->result.gpu_ms > 0 && done->result.cpu_ms > 0) {  // it ran: the choice stays
        show_device_result(done->result.gpu_ms, done->result.cpu_ms);
        update_enabled_controls();
        return true;
    }
    // The model does not run on this card: back to the CPU, and the card is no longer offered.
    const auto it = std::find_if(gpu_choices_.begin(), gpu_choices_.end(), [&](const GpuAdapter& a) {
        return a.key == done->adapter.key;
    });
    if (it != gpu_choices_.end() && hDevice_) {
        SendMessageW(hDevice_, CB_DELETESTRING, static_cast<WPARAM>(it - gpu_choices_.begin() + 1), 0);
        gpu_choices_.erase(it);
    }
    if (hDevice_) combo_set_index(hDevice_, 0);
    show_device_hint(gpu_choices_.empty() ? tr("general.device_none") : tr("general.device_hint"));
    update_enabled_controls();
    MessageBoxW(hwnd_, tr("general.device_failed"), tr("window.title"), MB_OK | MB_ICONINFORMATION);
    return true;
}

}  // namespace settings
}  // namespace cxxime
