// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The shortcuts read one way (docs/key-gestures.md, cxxime::KeyGestureTracker): every key down
// and up the text service gets goes through the tracker, from the key event sink, the preserved
// keys, a system hotkey and the translation key hook; a timer runs while a shortcut is held. Only
// the voice key acts on what comes out (voice.key_mode: pressed to start and again to stop, or
// held while speaking); the other shortcuts go on as before.
#include "text_service.h"

#include <cxxime/key_event.h>

#include "config_coordinator.h"

namespace {

// GestureBinding ids. The switch key combinations use their slot (c_guidPreservedKey* order).
constexpr int kGestureVoice = 4;          // the voice key combination (slot 4)
constexpr int kGestureVoiceModifier = 5;  // a tapped Shift / Ctrl / Alt bound to toggle_voice
constexpr int kGestureModifier = 6;       // a tapped Shift / Ctrl / Alt with another binding
constexpr int kGesturePageUp = 7;
constexpr int kGesturePageDown = 8;
constexpr int kGestureSymbol = 9;         // \ (twice: 、)
constexpr int kGestureTranslation = 10;   // Ctrl+0..9 in learning mode

constexpr UINT kKeyGestureTickMs = 30;

// The keys an ascii_composer.switch_key name stands for (Caps Lock and the Windows keys keep
// their own ways).
std::vector<uint32_t> modifier_keys(const std::string& name) {
    if (name == "Shift_L") return {VK_LSHIFT};
    if (name == "Shift_R") return {VK_RSHIFT};
    if (name == "Shift") return {VK_LSHIFT, VK_RSHIFT};
    if (name == "Control_L") return {VK_LCONTROL};
    if (name == "Control_R") return {VK_RCONTROL};
    if (name == "Control") return {VK_LCONTROL, VK_RCONTROL};
    if (name == "Alt_L") return {VK_LMENU};
    if (name == "Alt_R") return {VK_RMENU};
    if (name == "Alt") return {VK_LMENU, VK_RMENU};
    return {};
}

// What a tapped modifier does (ascii_composer.cc parse_style); "noop", "append" and unknown
// values do nothing.
bool modifier_switches(const std::string& style) {
    for (const char* known : {"inline_ascii", "code", "candidate", "clear", "set_ascii_mode",
                              "unset_ascii_mode", "toggle_style", "toggle_punct", "toggle_shape",
                              "toggle_voice"}) {
        if (style == known) return true;
    }
    return false;
}

uint32_t page_key_vk(char key) {
    switch (key) {
    case '-': return VK_OEM_MINUS;
    case ',': return VK_OEM_COMMA;
    case '[': return VK_OEM_4;
    case '=': return VK_OEM_PLUS;
    case '.': return VK_OEM_PERIOD;
    case ']': return VK_OEM_6;
    default: return 0;
    }
}

}  // namespace

void TextService::_configure_key_gestures() {
    std::vector<cxxime::GestureBinding> bindings;
    const bool voice_hold = _config.voice_key_mode == "hold";
    for (int slot = 0; slot < 5; ++slot) {
        const cxxime::KeyboardShortcut key = _switch_key(slot);  // none for voice input off
        if (!key.enabled()) continue;
        cxxime::GestureBinding b;
        b.id = slot;
        b.combination = key;
        b.hold = slot == kGestureVoice && voice_hold;
        bindings.push_back(b);
    }
    for (const auto& [name, style] : _config.ascii_switch_key) {
        if (!modifier_switches(style)) continue;
        const bool voice = style == "toggle_voice";
        if (voice && !_config.voice_enable) continue;  // does nothing then (issue #29)
        for (uint32_t vk : modifier_keys(name)) {
            cxxime::GestureBinding b;
            b.id = voice ? kGestureVoiceModifier : kGestureModifier;
            b.kind = cxxime::GestureKeyKind::kModifier;
            b.key = vk;
            b.hold = voice && voice_hold;
            bindings.push_back(b);
        }
    }
    for (const auto& [id, key] : {std::pair{kGesturePageUp, _config.page_up_key},
                                  std::pair{kGesturePageDown, _config.page_down_key}}) {
        if (!page_key_vk(key)) continue;
        cxxime::GestureBinding b;
        b.id = id;
        b.kind = cxxime::GestureKeyKind::kKey;
        b.key = page_key_vk(key);
        b.while_composing = true;  // otherwise the key types itself
        bindings.push_back(b);
    }
    {
        cxxime::GestureBinding b;  // the second \ while the first one is shown
        b.id = kGestureSymbol;
        b.kind = cxxime::GestureKeyKind::kKey;
        b.key = VK_OEM_5;
        b.while_composing = true;
        bindings.push_back(b);
    }
    if (!_config.chinese_gloss_target.empty() || !_config.english_gloss_target.empty()) {
        cxxime::GestureBinding b;
        b.id = kGestureTranslation;
        b.kind = cxxime::GestureKeyKind::kCtrlDigit;
        b.while_composing = true;
        bindings.push_back(b);
    }
    _keyGestures.configure(std::move(bindings), static_cast<uint32_t>(_config.shortcut_hold_ms));
    _voiceHoldArmed = false;
    _update_key_gesture_timer();
}

bool TextService::_feed_key_gesture(uint32_t vk, bool up) {
    const int owner_before = _keyGestures.owner(vk);
    _handle_key_gestures(
        _keyGestures.key(vk, up, _get_modifiers(), _composing, GetTickCount64()));
    _update_key_gesture_timer();
    return (up ? owner_before : _keyGestures.owner(vk)) == kGestureVoice;
}

void TextService::_end_key_gestures() {
    _handle_key_gestures(_keyGestures.focus_lost());
    _update_key_gesture_timer();
}

void TextService::_tick_key_gestures() {
    if (!_activated || !_inputFocused) {
        _end_key_gestures();
        return;
    }
    const UINT translation_key = _translationKeyDown;  // taken by the hook: no key state
    _handle_key_gestures(_keyGestures.tick(GetTickCount64(), [translation_key](uint32_t vk) {
        return (translation_key != 0 && vk == translation_key) ||
               (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }));
    _update_key_gesture_timer();
}

void TextService::_update_key_gesture_timer() {
    if (!_configWindow) return;
    if (_keyGestures.active() && !_keyGestureTimer) {
        _keyGestureTimer = SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_KEY_GESTURE,
                                    kKeyGestureTickMs, nullptr) != 0;
    } else if (!_keyGestures.active() && _keyGestureTimer) {
        KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_KEY_GESTURE);
        _keyGestureTimer = false;
    }
}

// The voice key (voice.key_mode). Pressed to start and again to stop: a press of the combination
// or a tap of the Shift / Ctrl / Alt starts or stops recording, as before. Held while speaking:
// the combination starts at its press and a Shift / Ctrl / Alt once held for shortcuts.hold_ms;
// the release (or another shortcut) ends it and the speech is recognized. A press of the
// combination shorter than that, or a held Shift / Ctrl / Alt that became a combination (Ctrl+C),
// drops the recording without a word.
void TextService::_handle_key_gestures(const std::vector<cxxime::Gesture>& gestures) {
    using cxxime::GestureEnd;
    using cxxime::GestureType;
    const bool hold = _config.voice_key_mode == "hold";
    for (const cxxime::Gesture& g : gestures) {
        if (g.id != kGestureVoice && g.id != kGestureVoiceModifier) {
            continue;  // the other shortcuts: their keys go on to the engine as before
        }
        const bool combination = g.id == kGestureVoice;
        if (!hold) {
            if ((combination && g.type == GestureType::kPress) ||
                (!combination && g.type == GestureType::kTap)) {
                _send_voice_key(cxxime::kVoiceKeyPress);
            }
            continue;
        }
        if ((combination && g.type == GestureType::kPress) ||
            (!combination && g.type == GestureType::kHoldStart)) {
            // Started now (a press while recording stops it instead).
            _voiceHoldArmed = _send_voice_key(cxxime::kVoiceKeyPress) == cxxime::kVoiceListening;
        } else if (g.type == GestureType::kHoldEnd) {
            if (_voiceHoldArmed && g.end != GestureEnd::kFocusLost) {
                _send_voice_key(cxxime::kVoiceKeyRelease);
            }
            _voiceHoldArmed = false;
        } else if (g.type == GestureType::kTap || g.type == GestureType::kCancel) {
            if (_voiceHoldArmed) _send_voice_key(cxxime::kVoiceKeyCancel);
            _voiceHoldArmed = false;
        }
    }
}

uint32_t TextService::_send_voice_key(uint32_t code) {
    if (!_ensure_ipc_session()) return cxxime::kVoiceIdle;
    cxxime::IPCResponse response = {};
    if (!_client.process_key(_sessionId, code, _get_modifiers() & cxxime::kKeyModifierCapsLock,
                             response) ||
        response.status != cxxime::IPCStatus::OK) {
        return cxxime::kVoiceIdle;
    }
    ITfContext* context = _current_edit_context_for_composition();
    BOOL eaten = FALSE;
    _apply_engine_response(context, response, &eaten);  // a composition is committed as typed
    if (context) context->Release();
    return response.voice_state;
}
