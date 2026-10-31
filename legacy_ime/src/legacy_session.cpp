// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "legacy_session.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include <cxxime/host_trace.h>

#include "legacy_trace.h"

namespace cxxime_legacy {
namespace {

std::mutex g_sessions_mutex;
std::map<HIMC, std::shared_ptr<LegacyImeSession>> g_sessions;

DWORD conversion_mode_from_status(const cxxime::ImeStatus& status) {
    DWORD conversion = 0;
    if (status.chinese_mode() && !status.caps_lock()) {
        conversion |= IME_CMODE_NATIVE;
    }
    if (status.full_shape()) {
        conversion |= IME_CMODE_FULLSHAPE;
    }
    return conversion;
}

void ensure_default_input_forms(LPINPUTCONTEXT input_context) {
    if (!input_context) {
        return;
    }

    if ((input_context->fdwInit & INIT_COMPFORM) == 0) {
        input_context->cfCompForm = {};
        input_context->cfCompForm.dwStyle = CFS_DEFAULT;
        input_context->fdwInit |= INIT_COMPFORM;
    }

    for (DWORD i = 0; i < ARRAYSIZE(input_context->cfCandForm); ++i) {
        input_context->cfCandForm[i].dwIndex = i;
    }

    if ((input_context->fdwInit & INIT_CONVERSION) == 0) {
        input_context->fdwConversion = IME_CMODE_NATIVE;
        input_context->fdwInit |= INIT_CONVERSION;
    }
    if ((input_context->fdwInit & INIT_SENTENCE) == 0) {
        input_context->fdwSentence = IME_SMODE_NONE;
        input_context->fdwInit |= INIT_SENTENCE;
    }
}

} // namespace

LegacyImeSession::LegacyImeSession(HIMC himc) : himc_(himc) {}

LegacyImeSession::~LegacyImeSession() {
    client_.disconnect();
}

void LegacyImeSession::select(bool selected) {
    if (selected) {
        initialize_input_context();
        ensure_session();
        return;
    }

    clear_context();
    end_server_session();
    finalize_input_context();
}

void LegacyImeSession::set_active(bool active) {
    if (active) {
        initialize_input_context();
        ensure_session();
        if (session_id_ != 0) {
            client_.focus_in(session_id_);
        }
        return;
    }

    clear_context();
    if (session_id_ != 0) {
        client_.focus_out(session_id_);
    }
}

bool LegacyImeSession::process_key(UINT key_code, LPARAM key_data, const BYTE* key_state) {
    host_trace_session_.begin_input(key_code, key_data);
    last_engine_calls_ = 0;
    // A switch key may be only the right Shift / Ctrl: the engine gets the side.
    key_code = cxxime::modifier_key_with_side(key_code, static_cast<uint32_t>(key_data));
    const bool was_composing = composing_;
    uint32_t modifiers = modifiers_from_key_state(key_state);
    const bool is_key_up = is_key_up_from_key_data(key_data);
    if (key_code == VK_CAPITAL && !is_key_up) {
        if (GetKeyState(VK_CAPITAL) & 0x0001) {
            modifiers |= 0x08;
        } else {
            modifiers &= ~0x08;
        }
    }

    cxxime::IPCResponse response = {};
    if (!send_key(key_code, modifiers, is_key_up, &response)) {
        return false;
    }
    const bool eaten = should_eat_response(response, key_code, was_composing);
    if (!composing_ && !candidate_open_) {
        host_trace_session_.reset_composition();
    }
    return eaten;
}

bool LegacyImeSession::tap_alt(uint32_t vk) {
    host_trace_session_.begin_input(vk, 0);
    last_engine_calls_ = 0;
    const uint32_t caps = (GetKeyState(VK_CAPITAL) & 0x0001) ? cxxime::kKeyModifierCapsLock : 0;
    cxxime::IPCResponse down = {};
    cxxime::IPCResponse up = {};
    if (!send_key(vk, cxxime::kKeyModifierAlt | caps, false, &down) ||
        !send_key(vk, caps, true, &up) || down.status != cxxime::IPCStatus::OK ||
        up.status != cxxime::IPCStatus::OK) {
        return false;
    }
    return up.key_handled || up.ime_status.flags != down.ime_status.flags ||
           up.ime_status.input_mode != down.ime_status.input_mode ||
           up.voice_state != down.voice_state;
}

bool LegacyImeSession::process_alt_combination(const cxxime::HookedKey& key) {
    host_trace_session_.begin_input(key.vk, static_cast<LPARAM>(key.key_data));
    last_engine_calls_ = 0;
    const uint32_t caps = (GetKeyState(VK_CAPITAL) & 0x0001) ? cxxime::kKeyModifierCapsLock : 0;
    cxxime::IPCResponse response = {};
    return send_key(key.vk, key.modifiers | caps, key.up, &response) && response.key_handled;
}

bool LegacyImeSession::send_key(uint32_t key_code, uint32_t modifiers, bool is_key_up,
                                cxxime::IPCResponse* out) {
    if (!ImmGetOpenStatus(himc_) || !ensure_session()) {
        return false;
    }
    cxxime::IPCResponse& response = *out;
    ++last_engine_calls_;
    if (!client_.process_key(session_id_, key_code, modifiers, response, is_key_up) ||
        response.status == cxxime::IPCStatus::ERR_INVALID_SESSION) {
        end_server_session();
        if (!ensure_session()) {
            return false;
        }
        response = {};
        ++last_engine_calls_;
        if (!client_.process_key(session_id_, key_code, modifiers, response, is_key_up)) {
            return false;
        }
    }

    const bool has_visible_state = response.composing || response.preedit[0] ||
                                   response.commit_text[0] || response.candidate_count > 0;
    if (has_visible_state) {
        host_trace_session_.ensure_composition();
    }
    trace_legacy_response(
        host_trace_session_.input_id(), host_trace_session_.composition_id(),
        session_id_, key_code,
        static_cast<int>(response.status), response.composing, strlen(response.preedit),
        strlen(response.commit_text), response.candidate_count, response.highlighted);
    apply_response(response);
    return true;
}

void LegacyImeSession::close_candidate_list() {
    write_candidates({}, 0);
    if (candidate_open_) {
        add_ime_message(WM_IME_NOTIFY, IMN_CLOSECANDIDATE, kCandidateListMask);
        candidate_open_ = false;
    }
}

void LegacyImeSession::cancel_composition() {
    if (session_id_ != 0) {
        client_.clear_composition(session_id_);
    }
    clear_context();
}

void LegacyImeSession::complete_composition() {
    if (session_id_ == 0) {
        clear_context();
        return;
    }

    cxxime::IPCResponse response = {};
    if (client_.commit_composition(session_id_, response)) {
        apply_response(response);
    } else {
        clear_context();
    }
}

void LegacyImeSession::select_candidate(DWORD candidate_index) {
    if (session_id_ == 0 || last_candidates_.empty()) {
        return;
    }

    const DWORD clamped = std::min<DWORD>(
        candidate_index, static_cast<DWORD>(last_candidates_.size() - 1));
    cxxime::IPCResponse response = {};
    if (client_.select_candidate(session_id_, static_cast<int>(clamped), response)) {
        apply_response(response);
    }
}

void LegacyImeSession::set_candidate_page_start(DWORD page_start) {
    if (last_candidates_.empty()) {
        candidate_page_start_ = 0;
        return;
    }

    candidate_page_start_ = std::min<DWORD>(
        page_start, static_cast<DWORD>(last_candidates_.size() - 1));
    rewrite_last_candidates(true);
}

void LegacyImeSession::set_candidate_page_size(DWORD page_size) {
    candidate_page_size_ = std::max<DWORD>(
        1, std::min<DWORD>(page_size, static_cast<DWORD>(cxxime::kCandidateCapacity)));
    if (!last_candidates_.empty()) {
        rewrite_last_candidates(true);
    }
}

void LegacyImeSession::handle_open_status_changed() {
    if (!ImmGetOpenStatus(himc_)) {
        if (session_id_ != 0) {
            client_.clear_composition(session_id_);
        }
        clear_context();
    }
}

void LegacyImeSession::clear_context() {
    write_composition({}, {});
    write_candidates({}, 0);

    if (composing_) {
        add_ime_message(WM_IME_ENDCOMPOSITION, 0, 0);
    }
    if (candidate_open_) {
        add_ime_message(WM_IME_NOTIFY, IMN_CLOSECANDIDATE, kCandidateListMask);
    }
    composing_ = false;
    candidate_open_ = false;
}

bool LegacyImeSession::ensure_session() {
    if (session_id_ != 0 && client_.is_connected()) {
        return true;
    }

    session_id_ = 0;
    if (!client_.connect(cxxime::IPC_PIPE_BASE_NAME, 250)) {
        launch_server();
        Sleep(350);
        if (!client_.connect(cxxime::IPC_PIPE_BASE_NAME, 1200)) {
            return false;
        }
    }

    if (!client_.start_session(session_id_)) {
        client_.disconnect();
        session_id_ = 0;
        return false;
    }

    client_.focus_in(session_id_);
    cxxime::IPCResponse response = {};
    if (client_.sync_caps_lock(session_id_, (GetKeyState(VK_CAPITAL) & 0x0001) != 0, response) &&
        response.status == cxxime::IPCStatus::OK) {
        apply_response(response);
    }
    return true;
}

void LegacyImeSession::end_server_session() {
    if (session_id_ != 0) {
        client_.focus_out(session_id_);
        client_.end_session(session_id_);
        session_id_ = 0;
    }
    client_.disconnect();
}

void LegacyImeSession::initialize_input_context() {
    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    input_context->fOpen = TRUE;
    ensure_default_input_forms(input_context);
    resize_imcc(input_context->hCompStr, sizeof(CompositionBuffer));
    resize_imcc(input_context->hCandInfo, sizeof(CANDIDATEINFO));
    caps_lock_ = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    ImmUnlockIMC(himc_);
}

void LegacyImeSession::finalize_input_context() {
    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    input_context->fOpen = FALSE;
    ImmUnlockIMC(himc_);
}

void LegacyImeSession::sync_status_to_input_context(const cxxime::ImeStatus& status) {
    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    const DWORD next_conversion = conversion_mode_from_status(status);
    const bool conversion_changed = input_context->fdwConversion != next_conversion;
    input_context->fOpen = TRUE;
    input_context->fdwConversion = next_conversion;
    input_context->fdwSentence = IME_SMODE_NONE;
    input_context->fdwInit |= INIT_CONVERSION | INIT_SENTENCE;
    ImmUnlockIMC(himc_);

    if (conversion_changed) {
        add_ime_message(WM_IME_NOTIFY, IMN_SETCONVERSIONMODE, 0);
    }
}

void LegacyImeSession::apply_response(const cxxime::IPCResponse& response) {
    if (response.status != cxxime::IPCStatus::OK) {
        return;
    }

    caps_lock_ = response.ime_status.caps_lock();
    sync_status_to_input_context(response.ime_status);

    const std::wstring commit = utf8_to_wide(response.commit_text);
    if (!commit.empty()) {
        commit_text(commit);
    }

    const std::wstring preedit = utf8_to_wide(response.preedit);
    std::vector<std::wstring> candidates;
    candidates.reserve(std::min<uint32_t>(
        response.candidate_count, static_cast<uint32_t>(cxxime::kCandidateCapacity)));
    for (uint32_t i = 0;
         i < response.candidate_count && i < cxxime::kCandidateCapacity; ++i) {
        candidates.push_back(utf8_to_wide(response.candidates[i]));
    }

    if (response.composing || !preedit.empty() || !candidates.empty()) {
        update_composition(preedit, candidates, response.highlighted);
    } else if (commit.empty() && (composing_ || candidate_open_)) {
        clear_context();
    }
}

void LegacyImeSession::update_composition(const std::wstring& preedit,
                                          const std::vector<std::wstring>& candidates,
                                          uint32_t highlighted) {
    write_composition(truncate_text(preedit), {});
    write_candidates(candidates, highlighted);

    trace_legacy_candidate_signal(host_trace_session_.input_id(),
                                        host_trace_session_.composition_id(), preedit.size(),
                                        candidates.size(), highlighted, candidate_open_);

    if (!composing_) {
        add_ime_message(WM_IME_STARTCOMPOSITION, 0, 0);
        composing_ = true;
    }
    add_ime_message(WM_IME_COMPOSITION, 0, kCompositionFlags);

    if (!candidates.empty()) {
        add_ime_message(WM_IME_NOTIFY,
                        candidate_open_ ? IMN_CHANGECANDIDATE : IMN_OPENCANDIDATE,
                        kCandidateListMask);
        candidate_open_ = true;
    } else if (candidate_open_) {
        add_ime_message(WM_IME_NOTIFY, IMN_CLOSECANDIDATE, kCandidateListMask);
        candidate_open_ = false;
    }
}

void LegacyImeSession::commit_text(const std::wstring& text) {
    write_composition({}, truncate_text(text));
    write_candidates({}, 0);

    add_ime_message(WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_RESULTCLAUSE);
    if (composing_) {
        add_ime_message(WM_IME_ENDCOMPOSITION, 0, 0);
    }
    if (candidate_open_) {
        add_ime_message(WM_IME_NOTIFY, IMN_CLOSECANDIDATE, kCandidateListMask);
    }
    composing_ = false;
    candidate_open_ = false;
}

void LegacyImeSession::write_composition(std::wstring preedit, std::wstring result) {
    preedit = truncate_text(std::move(preedit));
    result = truncate_text(std::move(result));

    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    if (!resize_imcc(input_context->hCompStr, sizeof(CompositionBuffer))) {
        ImmUnlockIMC(himc_);
        return;
    }

    auto* buffer = static_cast<CompositionBuffer*>(ImmLockIMCC(input_context->hCompStr));
    if (!buffer) {
        ImmUnlockIMC(himc_);
        return;
    }

    std::memset(buffer, 0, sizeof(*buffer));
    fill_composition_string(buffer->cs, preedit, result);
    if (!preedit.empty()) {
        std::memset(buffer->comp_attr, ATTR_INPUT, preedit.size());
        buffer->comp_clause[0] = 0;
        buffer->comp_clause[1] = static_cast<DWORD>(preedit.size());
        std::memcpy(buffer->comp_str, preedit.c_str(),
                    (preedit.size() + 1) * sizeof(WCHAR));
    }
    if (!result.empty()) {
        buffer->result_clause[0] = 0;
        buffer->result_clause[1] = static_cast<DWORD>(result.size());
        std::memcpy(buffer->result_str, result.c_str(),
                    (result.size() + 1) * sizeof(WCHAR));
    }

    ImmUnlockIMCC(input_context->hCompStr);
    ImmUnlockIMC(himc_);

    trace_legacy_imm_write(
        himc_, host_trace_session_.input_id(), host_trace_session_.composition_id(),
        preedit, result);
}

void LegacyImeSession::write_candidates(const std::vector<std::wstring>& raw_candidates,
                                        uint32_t highlighted) {
    last_candidates_.clear();
    const size_t count =
        std::min<size_t>(raw_candidates.size(), cxxime::kCandidateCapacity);
    last_candidates_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        last_candidates_.push_back(truncate_text(raw_candidates[i]));
    }

    if (last_candidates_.empty()) {
        last_highlighted_ = 0;
        candidate_page_start_ = 0;
    } else {
        last_highlighted_ = std::min<uint32_t>(
            highlighted, static_cast<uint32_t>(last_candidates_.size() - 1));
        if (candidate_page_start_ >= last_candidates_.size()) {
            candidate_page_start_ = 0;
        }
    }
    rewrite_last_candidates(false);

    trace_legacy_candidate_snapshot(
        himc_, host_trace_session_.input_id(), host_trace_session_.composition_id(),
        last_candidates_, last_highlighted_,
        candidate_page_start_, candidate_page_size_);
}

void LegacyImeSession::rewrite_last_candidates(bool notify_change) {
    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    const size_t count = last_candidates_.size();
    if (count == 0) {
        if (resize_imcc(input_context->hCandInfo, sizeof(CANDIDATEINFO))) {
            auto* info = static_cast<CANDIDATEINFO*>(ImmLockIMCC(input_context->hCandInfo));
            if (info) {
                std::memset(info, 0, sizeof(CANDIDATEINFO));
                info->dwSize = sizeof(CANDIDATEINFO);
                ImmUnlockIMCC(input_context->hCandInfo);
            }
        }
        ImmUnlockIMC(himc_);
        return;
    }

    DWORD strings_bytes = 0;
    for (const auto& candidate : last_candidates_) {
        strings_bytes += static_cast<DWORD>((candidate.size() + 1) * sizeof(WCHAR));
    }

    const DWORD list_offset = sizeof(CANDIDATEINFO);
    const DWORD list_header =
        static_cast<DWORD>(offsetof(CANDIDATELIST, dwOffset) + count * sizeof(DWORD));
    const DWORD strings_offset = align4(list_offset + list_header);
    const DWORD total_bytes = strings_offset + strings_bytes;

    if (!resize_imcc(input_context->hCandInfo, total_bytes)) {
        ImmUnlockIMC(himc_);
        return;
    }

    auto* info = static_cast<CANDIDATEINFO*>(ImmLockIMCC(input_context->hCandInfo));
    if (!info) {
        ImmUnlockIMC(himc_);
        return;
    }

    std::memset(info, 0, total_bytes);
    info->dwSize = total_bytes;
    info->dwCount = 1;
    info->dwOffset[kCandidateListIndex] = list_offset;

    auto* list = reinterpret_cast<CANDIDATELIST*>(
        reinterpret_cast<BYTE*>(info) + list_offset);
    list->dwSize = total_bytes - list_offset;
    list->dwStyle = IME_CAND_UNKNOWN;
    list->dwCount = static_cast<DWORD>(count);
    list->dwSelection = std::min<DWORD>(last_highlighted_, static_cast<DWORD>(count - 1));
    list->dwPageStart = std::min<DWORD>(candidate_page_start_, static_cast<DWORD>(count - 1));
    const DWORD remaining =
        static_cast<DWORD>(count) - std::min<DWORD>(list->dwPageStart, static_cast<DWORD>(count));
    list->dwPageSize = std::max<DWORD>(
        1, std::min<DWORD>(candidate_page_size_, remaining));

    BYTE* cursor = reinterpret_cast<BYTE*>(info) + strings_offset;
    for (size_t i = 0; i < count; ++i) {
        list->dwOffset[i] =
            static_cast<DWORD>(cursor - reinterpret_cast<BYTE*>(list));
        const DWORD bytes = static_cast<DWORD>((last_candidates_[i].size() + 1) * sizeof(WCHAR));
        std::memcpy(cursor, last_candidates_[i].c_str(), bytes);
        cursor += bytes;
    }

    ImmUnlockIMCC(input_context->hCandInfo);
    ImmUnlockIMC(himc_);

    if (notify_change && candidate_open_) {
        add_ime_message(WM_IME_NOTIFY, IMN_CHANGECANDIDATE, kCandidateListMask);
    }
}

void LegacyImeSession::add_ime_message(UINT message, WPARAM wparam, LPARAM lparam) {
    LPINPUTCONTEXT input_context = ImmLockIMC(himc_);
    if (!input_context) {
        return;
    }

    const DWORD old_count = input_context->dwNumMsgBuf;
    const DWORD new_count = old_count + 1;
    const DWORD bytes = new_count * sizeof(TRANSMSG);
    if (!resize_imcc(input_context->hMsgBuf, bytes)) {
        ImmUnlockIMC(himc_);
        return;
    }

    auto* messages = static_cast<TRANSMSG*>(ImmLockIMCC(input_context->hMsgBuf));
    if (!messages) {
        ImmUnlockIMC(himc_);
        return;
    }

    messages[old_count].message = message;
    messages[old_count].wParam = wparam;
    messages[old_count].lParam = lparam;
    input_context->dwNumMsgBuf = new_count;

    ImmUnlockIMCC(input_context->hMsgBuf);
    ImmUnlockIMC(himc_);
    const BOOL generated = ImmGenerateMessage(himc_);
    trace_legacy_imm_message(himc_, host_trace_session_.input_id(),
                                   host_trace_session_.composition_id(), message,
                                   wparam, lparam, generated != FALSE);
}

std::shared_ptr<LegacyImeSession> find_session(HIMC himc, bool create) {
    if (!himc) {
        return {};
    }

    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_sessions.find(himc);
    if (it != g_sessions.end()) {
        return it->second;
    }

    if (!create) {
        return {};
    }

    auto session = std::make_shared<LegacyImeSession>(himc);
    g_sessions.emplace(himc, session);
    return session;
}

void remove_session(HIMC himc) {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    g_sessions.erase(himc);
}

void clear_sessions(bool notify_server) {
    std::map<HIMC, std::shared_ptr<LegacyImeSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        sessions.swap(g_sessions);
    }

    if (notify_server) {
        for (auto& entry : sessions) {
            if (entry.second) {
                entry.second->select(false);
            }
        }
    }
}

} // namespace cxxime_legacy
