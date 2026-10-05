// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "server_app.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

#include <cxxime/candidate.h>
#include <cxxime/experience_log.h>
#include <cxxime/data_path.h>
#include <cxxime/dictionary_manifest.h>
#include <cxxime/ime_menu.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/logging.h>
#include <cxxime/settings_launcher.h>
#include <cxxime/ui_protocol.h>

#include "ipc_response_builder.h"
#include "lexicon_control_handler.h"
#include "user_backup_service.h"

namespace {

constexpr UINT kPrepareConfigMessage = WM_APP + 1;
constexpr UINT kCommitConfigMessage = WM_APP + 2;
constexpr UINT kCancelConfigMessage = WM_APP + 3;

std::optional<cxxime::SettingsPanel> settings_panel_for_ui_command(
    const cxxime::UiCommand& command) {
    if (command.type == cxxime::UiCommandType::kOpenDictionary) {
        return cxxime::SettingsPanel::kDictionary;
    }
    return std::nullopt;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), &result[0], length) != length) {
        return {};
    }
    return result;
}

const wchar_t* config_failure_message(ConfigStoreFailure failure) {
    switch (failure) {
    case ConfigStoreFailure::kBaseConfig:
        return L"默认配置加载失败。";
    case ConfigStoreFailure::kUserConfig:
        return L"用户配置加载失败。";
    case ConfigStoreFailure::kThemes:
        return L"主题配置加载失败。请检查 themes.json 的格式和主题内容。";
    case ConfigStoreFailure::kNone:
    default:
        return L"配置加载失败。";
    }
}

bool response_copy_field(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0 || src.size() >= dst_size) {
        return false;
    }
    memcpy(dst, src.c_str(), src.size() + 1);
    return true;
}

} // namespace

bool ServerApp::initialize(const std::string& dict_path, const std::string& config_path) {
    std::string resolved_dict = dict_path.empty() ? cxxime::data_path("pinyin.dict.bin") : dict_path;
    std::string cfg = config_path.empty() ? cxxime::data_path("default.json") : config_path;
    std::string user_config = cxxime::user_data_path("default.json");
    std::string themes = cxxime::data_path("themes.json");
    config_path_ = cfg;

    CXXIME_LOG(L"Dictionary path: %S", resolved_dict.c_str());
    CXXIME_LOG(L"Config path: %S", config_path_.c_str());

    std::shared_ptr<const cxxime::Config> initial_config;
    unsigned long config_error = ERROR_SUCCESS;
    ConfigStoreFailure config_failure = ConfigStoreFailure::kNone;
    if (!config_store_.initialize(config_path_, user_config, themes, &initial_config, &config_error,
                                  &config_failure)) {
        CXXIME_LOG(L"ServerApp: configuration initialization failed source=%d error=%lu",
                   static_cast<int>(config_failure), config_error);
        std::wstring msg = config_failure_message(config_failure);
        msg += L"\n\n默认配置: ";
        msg += utf8_to_wide(config_path_);
        msg += L"\n用户配置: ";
        msg += utf8_to_wide(user_config);
        msg += L"\n主题文件: ";
        msg += utf8_to_wide(themes);
        msg += L"\n错误代码: ";
        msg += std::to_wstring(config_error);
        MessageBoxW(nullptr, msg.c_str(), L"Zhiyi IME Server", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!session_mgr_.initialize(resolved_dict, initial_config)) {
        CXXIME_LOG(L"%s", L"ServerApp: input engine initialization failed");
        std::wstring msg = L"输入引擎初始化失败。\n\n";
        msg += L"Dict: ";
        msg += utf8_to_wide(resolved_dict);
        msg += L"\nConfig: ";
        msg += utf8_to_wide(config_path_);
        msg += L"\nData dir: ";
        std::string dd = cxxime::data_dir();
        msg += utf8_to_wide(dd);
        msg += L"\nUser data dir: ";
        std::string udd = cxxime::user_data_dir();
        msg += utf8_to_wide(udd);
        MessageBoxW(nullptr, msg.c_str(), L"Zhiyi IME Server", MB_OK | MB_ICONERROR);
        return false;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"ZhiyiIMEServerClass";
    RegisterClassExW(&wc);

    // A hidden top-level window receives session shutdown broadcasts; HWND_MESSAGE does not.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZhiyiIMEServerClass",
                            L"Zhiyi IME Server", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandle(nullptr), this);
    if (!hwnd_)
        return false;

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (!input_method_hotkey_.initialize(hwnd_, initial_config->activate_ime_shortcut)) {
        CXXIME_LOG(L"%s", L"activate_ime_hotkey event=initialize result=0");
    }

    const HWND config_window = hwnd_;
    if (!config_writer_.start(
            &config_store_,
            [this, config_window](const std::shared_ptr<const cxxime::Config>& config) {
                if (!session_mgr_.apply_config(config)) {
                    SendMessageW(config_window, kCancelConfigMessage, 0, 0);
                    CXXIME_LOG(L"%s", L"ServerApp: prepared config commit failed");
                    return;
                }
                SendMessageW(config_window, kCommitConfigMessage, 0, 0);
                ui_presentation_controller_.update_config(config);
                control_server_.publish_snapshot(config->to_runtime_json());
            },
            [this, config_window](const std::shared_ptr<const cxxime::Config>& config,
                                  unsigned long* error_code) {
                if (!session_mgr_.prepare_config(config, error_code)) {
                    return false;
                }
                if (SendMessageW(config_window, kPrepareConfigMessage,
                                 reinterpret_cast<WPARAM>(error_code),
                                 reinterpret_cast<LPARAM>(config.get())) != 0) {
                    return true;
                }
                session_mgr_.cancel_prepared_config();
                return false;
            },
            [this, config_window]() {
                session_mgr_.cancel_prepared_config();
                SendMessageW(config_window, kCancelConfigMessage, 0, 0);
            })) {
        MessageBoxW(nullptr, L"Failed to start config writer.", L"Zhiyi IME Server",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    std::string initial_config_json = initial_config->to_runtime_json();
    if (initial_config_json.empty() ||
        !control_server_.start(
            initial_config_json,
            [this](cxxime::UserConfigMutationKind kind, const std::string& payload,
                   std::string* config_json, unsigned long* error_code) {
                return config_writer_.submit(kind, payload, config_json, error_code);
            },
            [this](cxxime::ControlMessageType request_type, const std::string& payload,
                   cxxime::ControlMessageType* response_type,
                   std::string* response_payload) {
                if (!response_type) {
                    return false;
                }
                if (request_type == cxxime::ControlMessageType::kLexiconRequest) {
                    *response_type = cxxime::ControlMessageType::kLexiconResult;
                    return handle_lexicon_control_request(
                        session_mgr_, payload, response_payload);
                }
                if (request_type == cxxime::ControlMessageType::kUserBackupRequest) {
                    *response_type = cxxime::ControlMessageType::kUserBackupResult;
                    UserBackupService backup(&session_mgr_, &config_writer_);
                    return backup.handle_request(payload, response_payload);
                }
                return false;
            })) {
        config_writer_.stop();
        MessageBoxW(nullptr, L"Failed to start config control server.",
                    L"Zhiyi IME Server", MB_OK | MB_ICONERROR);
        return false;
    }

    session_mgr_.set_config_patch_handler([this](const std::string& merge_patch_json) {
        if (!config_writer_.enqueue_patch(merge_patch_json)) {
            CXXIME_LOG(L"%s", L"config_write event=enqueue_internal result=0");
        }
    });

    ipc_server_.set_handler([this](const cxxime::IPCRequest& req) { return handle_request(req); });
    ipc_server_.set_disconnect_handler(
        [this](uint32_t session_id) { session_mgr_.destroy_session(session_id); });
    if (!ipc_server_.start(cxxime::IPC_PIPE_BASE_NAME)) {
        session_mgr_.set_config_patch_handler({});
        config_writer_.stop();
        control_server_.stop();
        MessageBoxW(nullptr, L"Failed to start IPC server.", L"Zhiyi IME Server", MB_OK | MB_ICONERROR);
        return false;
    }

    const bool ui_controller_started = ui_presentation_controller_.start(
        initial_config,
        [this](cxxime::UiEndpointId endpoint, const cxxime::UiCommand& command) {
            const auto settings_panel = settings_panel_for_ui_command(command);
            if (settings_panel) {
                cxxime::open_settings(*settings_panel);
                return;
            }
            ui_presentation_router_.send_command(endpoint, command);
        });
    if (!ui_controller_started) {
        CXXIME_LOG(L"%s", L"ui_presentation event=start_controller result=degraded");
    } else if (!ui_presentation_router_.start(
                   [this](cxxime::UiEndpointId endpoint,
                          const cxxime::UiPresentationSnapshot* snapshot,
                          std::uint64_t candidate_placement_cycle,
                          std::uint64_t router_revision) {
                       ui_presentation_controller_.present(endpoint, snapshot,
                                                           candidate_placement_cycle,
                                                           router_revision);
                   })) {
        CXXIME_LOG(L"%s", L"ui_presentation event=start_channel result=degraded");
        ui_presentation_controller_.stop();
    }
    if (!system_lifecycle_monitor_.start(
            hwnd_, [this](SystemLifecycleMonitor::Event event) {
                ui_presentation_router_.reconcile_system_ui(
                    event == SystemLifecycleMonitor::Event::kSessionResumed);
            })) {
        CXXIME_LOG(L"%s", L"system_lifecycle event=start result=degraded");
    }

    std::string manifest_path = cxxime::dictionary_manifest_path_for_dict(resolved_dict);
    if (!dictionary_monitor_.start({manifest_path}, [this]() {
            return session_mgr_.reload_dictionaries() == cxxime::IPCStatus::OK;
        })) {
        CXXIME_LOG(L"%s", L"DictionaryMonitor: start failed");
    }

    diagnostic_log_maintenance_.start();
    if (!candidate_preference_saver_.start(
            [this](bool force) { return session_mgr_.save_candidate_preferences(force); })) {
        CXXIME_LOG(L"%s", L"candidate_preference save worker start failed");
        return false;
    }

    return true;
}

void ServerApp::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ServerApp::prepare_user_data_shutdown() {
    if (user_data_shutdown_prepared_) {
        return;
    }
    dictionary_monitor_.stop();
    if (!session_mgr_.freeze_and_stop_composition_learning()) {
        CXXIME_LOG(L"%s", L"composition_learning shutdown flush failed");
    }
    cxxime::ExperienceLog::instance().flush();
    if (!session_mgr_.freeze_and_save_candidate_preferences()) {
        CXXIME_LOG(L"%s", L"candidate_preference shutdown flush failed");
    }
    candidate_preference_saver_.stop();
    user_data_shutdown_prepared_ = true;
}

void ServerApp::finalize() {
    diagnostic_log_maintenance_.stop();
    dictionary_monitor_.stop();
    system_lifecycle_monitor_.stop();
    ui_presentation_router_.stop();
    ui_presentation_controller_.stop();
    ipc_server_.stop();
    control_server_.stop();
    prepare_user_data_shutdown();
    session_mgr_.set_config_patch_handler({});
    config_writer_.stop();
    input_method_hotkey_.shutdown();

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

cxxime::IPCResponse ServerApp::handle_request(const cxxime::IPCRequest& request) {
    CXXIME_LOG(L"handle_request: cmd=%u, session=%u",
               (uint32_t)request.command, request.session_id);

    cxxime::IPCResponse response = {};
    memset(&response, 0, sizeof(response));
    response.status = cxxime::IPCStatus::OK;

    switch (request.command) {
    case cxxime::IPCCommand::PING:
        response.status = cxxime::IPCStatus::OK;
        response.server_process_id = GetCurrentProcessId();
        break;

    case cxxime::IPCCommand::SEARCH_CANDIDATES: {
        const size_t query_length = strnlen_s(request.search_query,
                                              sizeof(request.search_query));
        if (query_length == sizeof(request.search_query)) {
            response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
            break;
        }
        const std::string query(request.search_query, query_length);
        const auto page = session_mgr_.search_candidates(query);
        response.candidate_count = static_cast<uint32_t>(
            (std::min)(page.candidates.size(), static_cast<size_t>(cxxime::kCandidateCapacity)));
        const int returned_end = page.page_offset + static_cast<int>(response.candidate_count);
        response.candidate_known_count = static_cast<uint32_t>(
            (std::max)(page.extent.known_count, returned_end));
        response.candidate_extent_state = page.extent.state;
        response.candidate_extent_complete = page.extent.complete ? 1u : 0u;
        response.candidate_total = static_cast<uint32_t>((std::max)(
            cxxime::legacy_candidate_total(page.extent, returned_end), 0));
        response.page_current = static_cast<uint32_t>(page.page_index + 1);
        const int page_size = page.page_size > 0 ? page.page_size : 10;
        response.page_total = response.candidate_total > 0
                ? static_cast<uint32_t>((response.candidate_total + page_size - 1) / page_size)
                : 1;
        if (cxxime::candidate_extent_may_continue(page.extent, returned_end)) {
            response.page_total = (std::max)(response.page_total, response.page_current + 1);
        }
        response.highlighted = page.highlighted >= 0
            ? static_cast<uint32_t>(page.highlighted)
            : 0;
        for (uint32_t i = 0; i < response.candidate_count; ++i) {
            if (!cxxime::candidate_text_fits(page.candidates[i].text) ||
                !response_copy_field(response.candidates[i], sizeof(response.candidates[i]),
                                     page.candidates[i].text)) {
                response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
                break;
            }
        }
        break;
    }

    case cxxime::IPCCommand::SEARCH_CANDIDATE_RESULT: {
        const size_t query_length = strnlen_s(request.search_query,
                                              sizeof(request.search_query));
        const size_t result_length = strnlen_s(request.search_result,
                                               sizeof(request.search_result));
        if (query_length == sizeof(request.search_query) ||
            result_length == sizeof(request.search_result)) {
            response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
            break;
        }
        response.status = session_mgr_.record_search_result(
            std::string(request.search_query, query_length),
            std::string(request.search_result, result_length))
            ? cxxime::IPCStatus::OK
            : cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        break;
    }

    case cxxime::IPCCommand::START_SESSION: {
        const uint32_t id = session_mgr_.create_session(request.client_capabilities);
        if (id == 0) {
            response.status = cxxime::IPCStatus::ERR_ENGINE_NOT_INITIALIZED;
            response.highlighted = 0;
        } else {
            response.highlighted = id;
            response.candidate_revision =
                (request.client_capabilities & cxxime::kClientCapabilitySegmentedSelection) != 0
                ? 1
                : 0;
        }
        CXXIME_LOG(L"START_SESSION: new session=%u capabilities=0x%llx", id,
                   static_cast<unsigned long long>(request.client_capabilities));
        break;
    }

    case cxxime::IPCCommand::END_SESSION:
        session_mgr_.destroy_session(request.session_id);
        CXXIME_LOG(L"END_SESSION: session=%u", request.session_id);
        break;

    case cxxime::IPCCommand::PROCESS_KEY: {
        cxxime::KeyEvent event;
        event.keycode = request.key_code;
        event.modifiers = request.modifiers;
        event.is_key_up = request.is_key_up != 0;

        const std::uint32_t server_visible_count =
            request.candidate_ui.presenter == cxxime::CandidateUiContext::Presenter::SERVER
                ? ui_presentation_controller_.visible_candidate_count(
                      request.session_id, request.candidate_ui)
                : 0;
        const std::uint32_t visible_candidate_count =
            cxxime::candidate_ui_visible_count(request.candidate_ui, server_visible_count);
        auto r =
            session_mgr_.process_key(request.session_id, event, visible_candidate_count);

        fill_process_response(r, &response);
        response.key_handled =
            r.result == cxxime::ProcessResult::SWITCH_INPUT_MODE ||
            r.result == cxxime::ProcessResult::INPUT_MODE_SHORTCUT_HANDLED ||
            r.result == cxxime::ProcessResult::TOGGLE_ENGLISH_STYLE ||
            r.result == cxxime::ProcessResult::TOGGLE_PINYIN_STYLE;

        if (r.status == cxxime::IPCStatus::OK &&
            r.result == cxxime::ProcessResult::REJECTED) {
            // If the engine rejected the key but cleared the composition
            // (e.g. CapsLock with "clear" style), return OK so the TSF
            // client can clean up its composition state.
            if (r.composing) {
                response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
            }
        }
        break;
    }

    case cxxime::IPCCommand::SELECT_CANDIDATE: {
        const auto r = session_mgr_.select_candidate(
            request.session_id, request.candidate_index, request.candidate_revision);
        fill_process_response(r, &response);
        break;
    }

    case cxxime::IPCCommand::COMMIT_COMPOSITION: {
        const auto r = session_mgr_.commit_composition(request.session_id);
        fill_process_response(r, &response);
        break;
    }

    case cxxime::IPCCommand::CLEAR_COMPOSITION: {
        const auto r = session_mgr_.clear_composition(request.session_id);
        fill_process_response(r, &response);
        break;
    }

    case cxxime::IPCCommand::FOCUS_IN: {
        const auto r = session_mgr_.focus_in(request.session_id);
        fill_process_response(r, &response);
        break;
    }

    case cxxime::IPCCommand::SET_INPUT_TARGET: {
        const std::string app(request.search_query,
                              strnlen_s(request.search_query, sizeof(request.search_query)));
        const std::string title(request.search_result,
                                strnlen_s(request.search_result, sizeof(request.search_result)));
        response.status = session_mgr_.set_input_target(request.session_id, app, title)
                              ? cxxime::IPCStatus::OK
                              : cxxime::IPCStatus::ERR_INVALID_SESSION;
        break;
    }

    case cxxime::IPCCommand::FOCUS_OUT: {
        const auto r = session_mgr_.focus_out(request.session_id);
        fill_process_response(r, &response);
        break;
    }

    case cxxime::IPCCommand::TOGGLE_CHINESE: {
        auto [status, ime_status] = session_mgr_.toggle_chinese(request.session_id);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.ime_status = ime_status;
        response.ascii_mode = !ime_status.chinese_mode();
        break;
    }

    case cxxime::IPCCommand::SET_CHINESE_MODE: {
        const bool chinese_mode = request.candidate_index != 0;
        const auto result = session_mgr_.set_chinese_mode(request.session_id, chinese_mode);
        fill_process_response(result, &response);
        break;
    }

    case cxxime::IPCCommand::TOGGLE_SHAPE: {
        auto [status, ime_status] = session_mgr_.toggle_shape(request.session_id);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::TOGGLE_PUNCT: {
        auto [status, ime_status] = session_mgr_.toggle_punct(request.session_id);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::SWITCH_INPUT_MODE: {
        const auto mode = static_cast<cxxime::InputMode>(request.candidate_index);
        auto [status, ime_status] = session_mgr_.switch_input_mode(request.session_id, mode);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::SET_INPUT_STYLE: {
        auto [status, ime_status] = session_mgr_.set_input_style(
            request.session_id, (request.candidate_index & 1u) != 0,
            (request.candidate_index & 2u) != 0);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::GET_STATUS: {
        auto [status, ime_status] = session_mgr_.get_ime_status(request.session_id);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.status = cxxime::IPCStatus::OK;
        response.ascii_mode = !ime_status.chinese_mode();
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::SYNC_CAPS_LOCK: {
        auto [status, ime_status] =
            session_mgr_.sync_caps_lock(request.session_id, (request.modifiers & 0x08) != 0);
        if (status != cxxime::IPCStatus::OK) {
            response.status = status;
            break;
        }
        response.status = cxxime::IPCStatus::OK;
        response.ascii_mode = !ime_status.chinese_mode();
        response.ime_status = ime_status;
        break;
    }

    case cxxime::IPCCommand::OPEN_SETTINGS: {
        const auto panel = static_cast<cxxime::SettingsPanel>(request.candidate_index);
        if (panel != cxxime::SettingsPanel::kInput &&
            panel != cxxime::SettingsPanel::kDictionary) {
            response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
            break;
        }
        response.status = cxxime::open_settings(panel)
                              ? cxxime::IPCStatus::OK
                              : cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
        break;
    }

    default:
        response.status = cxxime::IPCStatus::ERR_UNKNOWN_COMMAND;
        break;
    }

    return response;
}

LRESULT CALLBACK ServerApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ServerApp* app = reinterpret_cast<ServerApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app) {
        const std::optional<LRESULT> lifecycle_result =
            app->system_lifecycle_monitor_.handle_message(msg, wp, lp);
        if (lifecycle_result) {
            return *lifecycle_result;
        }
    }
    if (msg == kPrepareConfigMessage) {
        auto* error_code = reinterpret_cast<unsigned long*>(wp);
        const auto* config = reinterpret_cast<const cxxime::Config*>(lp);
        return app && config &&
            app->input_method_hotkey_.prepare_update(config->activate_ime_shortcut, error_code);
    }
    if (msg == kCommitConfigMessage) {
        return app && app->input_method_hotkey_.commit_update();
    }
    if (msg == kCancelConfigMessage) {
        if (app) {
            app->input_method_hotkey_.cancel_update();
        }
        return 0;
    }
    if (msg == WM_HOTKEY && app && app->input_method_hotkey_.handle(wp)) {
        return 0;
    }
    if (msg == WM_QUERYENDSESSION) {
        return TRUE;
    }
    if (msg == WM_ENDSESSION && wp && app) {
        app->prepare_user_data_shutdown();
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (app) {
            app->prepare_user_data_shutdown();
        }
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (app) {
            app->hwnd_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
