// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input in the text service (docs/voice-input.md). The server records and recognizes; the
// voice key's answer says recording started (IPCResponse::voice_state), and from then on the
// text service asks every 100 ms where it is (VOICE_STATUS) until the text comes, which it shows
// underlined in the input box as a composition of its own: Backspace or Esc delete it, Enter
// keeps it, any other key keeps it and then does what it does. (Classic Win32 programs get the
// text at once.) The input box losing the focus drops the recording.
#include "text_service.h"

#include <cstring>

#include "config_coordinator.h"
#include "text_before_caret.h"

namespace {

constexpr UINT kVoicePollMs = 100;

}  // namespace

void TextService::_start_voice_poll() {
    if (!_configWindow) return;
    _voiceTargetGeneration = _uiTargetGeneration;
    if (!_voicePolling) {
        _voicePolling = SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_VOICE_POLL, kVoicePollMs,
                                 nullptr) != 0;
    }
}

void TextService::_stop_voice_poll(bool cancel) {
    if (!_voicePolling) return;
    _voicePolling = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_VOICE_POLL);
    if (cancel && _sessionId && _client.is_connected()) _client.voice_cancel(_sessionId);
}

void TextService::_poll_voice() {
    if (!_voicePolling) return;
    // Another input box (or none): the recording is dropped.
    if (!_activated || !_sessionId || !_inputFocused || _uiTargetGeneration != _voiceTargetGeneration) {
        _stop_voice_poll(true);
        return;
    }
    cxxime::IPCResponse response = {};
    if (!_client.voice_status(_sessionId, response)) {
        _stop_voice_poll(false);
        return;
    }
    if (response.voice_state == cxxime::kVoiceListening ||
        response.voice_state == cxxime::kVoiceRecognizing) {
        return;
    }
    _stop_voice_poll(false);
    if (response.voice_state != cxxime::kVoiceDone) return;
    const std::size_t length = strnlen_s(response.voice_text, sizeof(response.voice_text));
    const std::wstring text = utf8_to_wstring(std::string(response.voice_text, length).c_str());
    if (!text.empty()) _show_voice_text(text);
}

void TextService::_show_voice_text(const std::wstring& text) {
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return;
    _hide_candidate_window("hide:voice_text");
    // Classic Win32 programs (Sublime Text) reach TSF through the IMM layer, and cancel a
    // composition, text and all, on a click or when they lose the focus; even text committed
    // just before the click is gone (issue #14). There the text goes in at once.
    if (context_is_imm_document(context)) {
        _enqueue_event_trace("voice_text", "imm_insert");
        if (_composition) _end_composition(context);
        insert_text(text, true);
        context->Release();
        return;
    }
    // (A program that defers the edit still shows the text: the composition comes a moment later.)
    const HRESULT result = update_composition(context, text, text.size(), true, TF_ES_SYNC);
    if (SUCCEEDED(result)) {
        _voiceText = text;
        _voiceComposition = true;
    } else {
        // No composition in this program: the text goes in as it is.
        _enqueue_event_trace("voice_text", "composition_failed", true);
        if (_composition) _end_composition(context);
        insert_text(text, true);
    }
    context->Release();
}

void TextService::_commit_voice_text(ITfContext* context) {
    if (!_voiceComposition) return;
    _voiceComposition = false;
    const std::wstring text = std::move(_voiceText);
    _voiceText.clear();
    ITfContext* own = context ? nullptr : _current_edit_context_for_composition();
    ITfContext* target = context ? context : own;
    if (target && _composition) _commit_text(target, text, true);
    _composing = false;
    clear_applied_inline_composition_text();
    if (own) own->Release();
}

void TextService::_delete_voice_text(ITfContext* context) {
    if (!_voiceComposition) return;
    _voiceComposition = false;
    _voiceText.clear();
    ITfContext* own = context ? nullptr : _current_edit_context_for_composition();
    ITfContext* target = context ? context : own;
    if (target && _composition) {
        update_composition(target, L"", 0);
        _end_composition(target);
    }
    _composing = false;
    clear_applied_inline_composition_text();
    if (own) own->Release();
}

bool TextService::_handle_voice_text_key(ITfContext* context, WPARAM key) {
    if (!_voiceComposition) return false;
    const uint32_t modifiers = _get_modifiers() & (0x01 | 0x02 | 0x04);
    if (modifiers == 0 && (key == VK_BACK || key == VK_ESCAPE)) {
        _delete_voice_text(context);
        return true;
    }
    if (modifiers == 0 && key == VK_RETURN) {
        _commit_voice_text(context);  // kept, without a new line (or a message being sent)
        return true;
    }
    _commit_voice_text(context);  // kept; the key then does what it does
    return false;
}
