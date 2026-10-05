// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_IPC_CLIENT_H_
#define CXXIME_IPC_CLIENT_H_

#include <cstdint>
#include <string>

#include <cxxime/ipc_protocol.h>
#include <cxxime/pipe_names.h>
#include <cxxime/settings_route.h>

namespace cxxime {

struct CandidateSelectionCallResult {
    bool transport_success = false;
    IPCStatus status = IPCStatus::ERR_ENGINE_PROCESS_FAILED;
    IPCResponse response = {};
};

class IpcClient {
public:
    IpcClient() = default;
    ~IpcClient();

    bool connect(const std::wstring& pipe_name = IPC_PIPE_BASE_NAME, int timeout_ms = 3000);
    void disconnect();
    bool is_connected() const;
    bool ensure_connected();

    bool send_request(const IPCRequest& request, IPCResponse& response);

    // High-level methods
    bool start_session(uint32_t& session_id, uint64_t client_capabilities = 0);
    bool end_session(uint32_t session_id);
    bool process_key(uint32_t session_id, uint32_t key_code, uint32_t modifiers,
                     IPCResponse& response, bool is_key_up = false,
                     CandidateUiContext candidate_ui = {});
    bool select_candidate(uint32_t session_id, int index, IPCResponse& response);
    CandidateSelectionCallResult select_candidate_with_revision(uint32_t session_id, int index,
                                                                uint64_t candidate_revision);
    bool commit_composition(uint32_t session_id, IPCResponse& response);
    bool clear_composition(uint32_t session_id);
    bool focus_in(uint32_t session_id);
    bool set_input_target(uint32_t session_id, const std::string& app,
                          const std::string& window_title);
    bool focus_out(uint32_t session_id);

    bool toggle_chinese(uint32_t session_id, IPCResponse& response);
    bool set_chinese_mode(uint32_t session_id, bool chinese_mode, IPCResponse& response);
    bool toggle_shape(uint32_t session_id, IPCResponse& response);
    bool toggle_punct(uint32_t session_id, IPCResponse& response);
    bool switch_input_mode(uint32_t session_id, InputMode mode, IPCResponse& response);
    // The pinyin style (english_style false: initials on/off) or the English style
    // (english_style true: word completion on/off).
    bool set_input_style(uint32_t session_id, bool english_style, bool value,
                         IPCResponse& response);
    bool get_status(uint32_t session_id, IPCResponse& response);
    bool sync_caps_lock(uint32_t session_id, bool caps_lock, IPCResponse& response);
    bool ping(IPCResponse* response = nullptr);
    bool search_candidates(const std::string& query, IPCResponse& response);
    bool set_search_result(const std::string& query, const std::string& result);
    bool open_settings(uint32_t session_id, SettingsPanel panel);

    int64_t last_ipc_us() const { return last_ipc_us_; }

private:
    bool try_reconnect();

    void* pipe_handle_ = nullptr;
    std::wstring pipe_name_;
    int timeout_ms_ = 3000;

    int64_t last_ipc_us_ = 0;
};

} // namespace cxxime

#endif // CXXIME_IPC_CLIENT_H_
