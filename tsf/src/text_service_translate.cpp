// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Selection translation: the translate key (Ctrl+C+C) takes the text selected in the input box
// (read_selection_text), the server's offline model translates it (IPCCommand::TRANSLATE_SELECTION,
// polled with TRANSLATE_STATUS). The result is shown in a candidate window of its own — the
// translation as the candidate, the selected text as its note — or, with translate.direct_insert,
// typed over the selection.
#include "text_service.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <cxxime/candidate_presentation.h>
#include <cxxime/candidate_window.h>

#include "config_coordinator.h"
#include "edit_session.h"
#include "text_before_caret.h"

namespace {

constexpr UINT kTranslatePollMs = 100;
constexpr size_t kMaxSelectionChars = 1000;

std::string selection_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

std::wstring selection_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// The selected text, from the clipboard: the first press of the translate key (Ctrl+C) copied
// it, and many input boxes (classic Win32 edits) cannot be read through TSF at all.
bool read_clipboard_text(std::wstring* text) {
    if (!text || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(data))) {
            *text = locked;
            GlobalUnlock(data);
            ok = !text->empty();
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace

bool TextService::_handle_translate_key(WPARAM key) {
    const cxxime::KeyboardShortcut& sc = _config.translate_selection_shortcut;
    if (!sc.enabled() || key != sc.virtual_key) return false;
    if ((_get_modifiers() & cxxime::kShortcutModifierMask) != sc.modifiers) return false;

    constexpr ULONGLONG kDoubleTapMs = 350;
    constexpr ULONGLONG kPairMs = 50;  // OnTestKeyDown and OnKeyDown report one press
    const ULONGLONG now = GetTickCount64();
    if (now - _lastTranslateKeySeen < kPairMs) return false;  // the other half of one press
    _lastTranslateKeySeen = now;

    const bool second =
        _lastTranslateCopyTick != 0 && now - _lastTranslateCopyTick <= kDoubleTapMs;
    if (!second) {
        _lastTranslateCopyTick = now;  // the first press: the application copies
        return false;
    }
    _lastTranslateCopyTick = 0;
    _translateKeyDown = static_cast<UINT>(key);
    // Read the selection and translate outside the key callback: reading it needs a synchronous
    // edit session, which cannot be nested inside one.
    PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_TRANSLATE_SELECTION, 0, 0);
    return true;
}

void TextService::_process_selection_translate() {
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return;
    std::wstring selection;
    TextBeforeCaret read =
        read_selection_text(context, _clientId, kMaxSelectionChars, &selection);
    if (read != TextBeforeCaret::kRead || selection.empty()) {
        // Fall back to the clipboard (the first Ctrl+C copied the selection): an input box TSF
        // cannot read still translates.
        if (read_clipboard_text(&selection)) read = TextBeforeCaret::kRead;
    }
    context->Release();
    if (read != TextBeforeCaret::kRead || selection.empty()) return;

    if (!_ensure_ipc_session()) return;
    cxxime::IPCResponse response = {};
    if (!_client.translate_selection(_sessionId, selection_to_utf8(selection), response)) return;
    _translateSource = selection;
    _start_translation_poll();
}

void TextService::_start_translation_poll() {
    if (!_configWindow) return;
    _translationTargetGeneration = _uiTargetGeneration;
    if (!_translationPolling) {
        _translationPolling = SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_TRANSLATE_POLL,
                                       kTranslatePollMs, nullptr) != 0;
    }
}

void TextService::_stop_translation_poll() {
    if (!_translationPolling) return;
    _translationPolling = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_TRANSLATE_POLL);
}

void TextService::_poll_translation() {
    if (!_translationPolling) return;
    // Another input box (or none): the request is dropped.
    if (!_activated || !_sessionId || !_inputFocused ||
        _uiTargetGeneration != _translationTargetGeneration) {
        _stop_translation_poll();
        return;
    }
    cxxime::IPCResponse response = {};
    if (!_client.translate_status(_sessionId, response)) {
        _stop_translation_poll();
        return;
    }
    if (response.translate_state == cxxime::kTranslateTranslating) return;

    _stop_translation_poll();
    if (response.translate_state != cxxime::kTranslateDone) return;
    const std::size_t length =
        strnlen_s(response.translate_text, sizeof(response.translate_text));
    const std::wstring translation =
        selection_from_utf8(std::string(response.translate_text, length));
    if (translation.empty()) return;

    if (_config.translate_direct_insert) {
        insert_text(translation, true);  // replaces the selection
    } else {
        // The cached caret may be stale (nothing typed): ask for it now, like the candidate
        // window does, so the window opens at the selection.
        RECT caret = _caretRect;
        if (ITfContext* context = _current_edit_context_for_composition()) {
            EditSession* session = new (std::nothrow) EditSession(this, context);
            if (session) {
                session->set_action(EditSession::Action::QUERY_CARET);
                HRESULT edit_result = E_FAIL;
                if (SUCCEEDED(context->RequestEditSession(
                        _clientId, session, TF_ES_READ | TF_ES_SYNC, &edit_result)) &&
                    SUCCEEDED(edit_result)) {
                    RECT queried = {};
                    if (session->get_caret_rect(queried)) caret = queried;
                }
                session->Release();
            }
            context->Release();
        }
        _present_translate_window(translation, _translateSource, caret);
    }
}

void TextService::_present_translate_window(const std::wstring& translation,
                                            const std::wstring& source, const RECT& caret) {
    HWND owner = reinterpret_cast<HWND>(_effectiveEditTarget.view_window);
    if (!owner || !IsWindow(owner)) owner = GetFocus();
    if (!owner || !IsWindow(owner)) return;

    if (!_translateWindow) {
        _translateWindow = std::make_unique<cxxime::CandidateWindow>();
        if (!_translateWindow->create(owner, _config) ||
            !_translateWindow->owner_matches(owner)) {
            _translateWindow.reset();
            return;
        }
    } else if (!_translateWindow->ensure_created(owner)) {
        return;
    }
    _translateWindow->set_layout(_config.layout);
    _translateWindow->set_page_info(1, 1);
    _translateWindow->set_preedit(std::string(), size_t{0}, size_t{0}, size_t{0}, size_t{0}, false);

    cxxime::CandidatePresentationPage page;
    page.page_size = 1;
    page.highlighted = 0;
    cxxime::CandidatePresentationItem item;
    item.text = selection_to_utf8(translation);
    item.gloss = selection_to_utf8(source);
    page.items.push_back(std::move(item));

    _translateWindow->move_to_caret(caret);
    _translateWindow->update(page);
    _translateWindow->show();
}

void TextService::_hide_translate_window() {
    if (_translateWindow) _translateWindow->hide();
}
