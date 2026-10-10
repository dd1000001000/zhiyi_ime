// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/ascii_composer.h>

#include <optional>
#include <utility>

#include <windows.h>

#include <cxxime/config.h>
#include <cxxime/context.h>
#include <cxxime/key_event.h>
#include <cxxime/logging.h>

namespace cxxime {

static bool is_shift_key(uint32_t vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

static bool is_ctrl_key(uint32_t vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
}

static bool is_alt_key(uint32_t vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

static bool is_win_key(uint32_t vk) {
    return vk == VK_LWIN || vk == VK_RWIN;
}

static AsciiModeSwitchStyle parse_style(const std::string& s) {
    if (s == "inline_ascii")     return AsciiModeSwitchStyle::INLINE_ASCII;
    if (s == "code")             return AsciiModeSwitchStyle::CODE;
    if (s == "candidate")        return AsciiModeSwitchStyle::CANDIDATE;
    if (s == "clear")            return AsciiModeSwitchStyle::CLEAR;
    if (s == "append")           return AsciiModeSwitchStyle::APPEND;
    if (s == "set_ascii_mode")   return AsciiModeSwitchStyle::SET_ASCII_MODE;
    if (s == "unset_ascii_mode") return AsciiModeSwitchStyle::UNSET_ASCII_MODE;
    if (s == "toggle_style")     return AsciiModeSwitchStyle::TOGGLE_STYLE;
    if (s == "toggle_punct")     return AsciiModeSwitchStyle::TOGGLE_PUNCT;
    if (s == "toggle_shape")     return AsciiModeSwitchStyle::TOGGLE_SHAPE;
    return AsciiModeSwitchStyle::NOOP;
}

void AsciiComposer::load_config(const Config& config) {
    bindings_.clear();
    for (const auto& [key, value] : config.ascii_switch_key) {
        uint32_t vk = 0;
        if (key == "Shift_L")       vk = VK_LSHIFT;
        else if (key == "Shift_R")  vk = VK_RSHIFT;
        else if (key == "Shift")    vk = VK_SHIFT;
        else if (key == "Control_L") vk = VK_LCONTROL;
        else if (key == "Control_R") vk = VK_RCONTROL;
        else if (key == "Control")  vk = VK_CONTROL;
        else if (key == "Caps_Lock") vk = VK_CAPITAL;
        else if (key == "Alt_L")    vk = VK_LMENU;
        else if (key == "Alt_R")    vk = VK_RMENU;
        else if (key == "Alt")      vk = VK_MENU;
        else if (key == "Super_L")  vk = VK_LWIN;
        else if (key == "Super_R")  vk = VK_RWIN;
        if (vk == 0)
            continue;

        auto style = parse_style(value);

        // CapsLock downgrade: inline_ascii / set_ascii_mode / unset_ascii_mode
        // are incompatible with CapsLock's toggle nature — downgrade to clear
        if (vk == VK_CAPITAL) {
            if (style == AsciiModeSwitchStyle::INLINE_ASCII ||
                style == AsciiModeSwitchStyle::SET_ASCII_MODE ||
                style == AsciiModeSwitchStyle::UNSET_ASCII_MODE ||
                style == AsciiModeSwitchStyle::TOGGLE_STYLE ||
                style == AsciiModeSwitchStyle::TOGGLE_PUNCT ||
                style == AsciiModeSwitchStyle::TOGGLE_SHAPE) {
                style = AsciiModeSwitchStyle::CLEAR;
            }
        }

        bindings_[vk] = style;
    }
}

bool AsciiComposer::process_key(uint32_t key_code, bool is_key_up, Context& ctx, bool caps_lock) {
    CXXIME_LOG(L"AsciiComposer::process_key: vk=%u, is_key_up=%d, composing=%d",
               key_code, is_key_up, ctx.is_composing());

    // Multiple modifier keys pressed simultaneously — reset, no toggle
    int pressed_count = (shift_pressed_ ? 1 : 0) + (ctrl_pressed_ ? 1 : 0) +
                        (alt_pressed_ ? 1 : 0) + (win_pressed_ ? 1 : 0);
    if (pressed_count > 1) {
        CXXIME_LOG(L"AsciiComposer::process_key: multiple modifiers pressed, resetting");
        shift_pressed_ = false;
        ctrl_pressed_ = false;
        alt_pressed_ = false;
        win_pressed_ = false;
        return false;
    }

    // CapsLock is an uppercase ASCII overlay: on key down, when the LED turns
    // on, enter ASCII mode and remember the previous CxxIME mode; when it turns
    // off, restore that previous mode.
    // Cancel any pending modifier toggle (e.g., Shift held then CapsLock pressed).
    if (key_code == VK_CAPITAL) {
        shift_pressed_ = false;
        ctrl_pressed_ = false;
        alt_pressed_ = false;
        win_pressed_ = false;

        if (!is_key_up) {
            auto style = get_binding(VK_CAPITAL);
            if (style == AsciiModeSwitchStyle::NOOP)
                return false;
            apply_caps_lock_overlay(caps_lock, ctx);
        }
        return false;
    }

    bool is_shift = is_shift_key(key_code);
    bool is_ctrl  = is_ctrl_key(key_code);
    bool is_alt   = is_alt_key(key_code);
    bool is_win   = is_win_key(key_code);

    if (is_shift || is_ctrl || is_alt || is_win) {
        if (caps_lock_overlay_active_ && caps_lock) {
            shift_pressed_ = false;
            ctrl_pressed_ = false;
            alt_pressed_ = false;
            win_pressed_ = false;
            return false;
        }

        if (is_key_up) {
            CXXIME_LOG(L"AsciiComposer::process_key: modifier key up, shift_pressed_=%d", shift_pressed_);
            if (shift_pressed_ || ctrl_pressed_ || alt_pressed_ || win_pressed_) {
                const AsciiModeSwitchStyle style = get_binding(key_code);
                if (style == AsciiModeSwitchStyle::TOGGLE_STYLE ||
                    style == AsciiModeSwitchStyle::TOGGLE_PUNCT ||
                    style == AsciiModeSwitchStyle::TOGGLE_SHAPE) {
                    // A quick tap only: Ctrl held for Ctrl+click or Ctrl+wheel is no switch.
                    constexpr auto kMaxTap = std::chrono::milliseconds(500);
                    if (std::chrono::steady_clock::now() - modifier_down_time_ <= kMaxTap) {
                        tap_toggle_ = style;
                    }
                    shift_pressed_ = false;
                    ctrl_pressed_ = false;
                    alt_pressed_ = false;
                    win_pressed_ = false;
                    return false;
                }
                const bool binding_applied = style != AsciiModeSwitchStyle::NOOP &&
                                             style != AsciiModeSwitchStyle::APPEND;
                if (binding_applied) {
                    CXXIME_LOG(L"AsciiComposer::process_key: calling toggle_mode");
                    toggle_mode(key_code, ctx);
                }
                shift_pressed_ = false;
                ctrl_pressed_ = false;
                alt_pressed_ = false;
                win_pressed_ = false;
                return binding_applied;
            }
        } else {
            CXXIME_LOG(L"AsciiComposer::process_key: modifier key down");
            if (!shift_pressed_ && !ctrl_pressed_ && !alt_pressed_ && !win_pressed_) {
                modifier_down_time_ = std::chrono::steady_clock::now();
                if (is_shift) shift_pressed_ = true;
                if (is_ctrl)  ctrl_pressed_ = true;
                if (is_alt)   alt_pressed_ = true;
                if (is_win)   win_pressed_ = true;
            }
        }
        return false;
    }

    // Non-modifier key: cancel pending modifier toggle
    shift_pressed_ = false;
    ctrl_pressed_ = false;
    alt_pressed_ = false;
    win_pressed_ = false;
    return false;
}

InlineAsciiResult AsciiComposer::process_inline_ascii_composition(const KeyEvent& event,
                                                                  Context& ctx,
                                                                  bool chinese_mode) {
    if (event.is_key_up || event.is_ctrl() || event.is_alt()) {
        return InlineAsciiResult::kNotHandled;
    }

    if (ctx.composition_scheme() == CompositionScheme::kInlineAscii) {
        if (event.keycode == VK_ESCAPE) {
            ctx.reset();
            finish_temporary_ascii();
            return InlineAsciiResult::kAccepted;
        }
        if (event.keycode == VK_SPACE) {
            ctx.reset();
            finish_temporary_ascii();
            return InlineAsciiResult::kAccepted;
        }

        const uint64_t revision_before = ctx.preedit_revision();
        if (ctx.edit_preedit(event)) {
            if (!ctx.is_composing()) {
                ctx.reset();
                finish_temporary_ascii();
                return InlineAsciiResult::kAccepted;
            }
            if (ctx.preedit_revision() != revision_before && ctx.restore_composition_origin()) {
                finish_temporary_ascii();
                return InlineAsciiResult::kResumeOrigin;
            }
            return InlineAsciiResult::kAccepted;
        }
        const std::optional<char> normalized = normalize_ascii_key(event);
        if (normalized) {
            ctx.insert_preedit(*normalized);
            ctx.clear_translation();
            ctx.reset_pagination();
            ctx.set_commit_source(CommitSource::kRawCodePreserveCase);
            return InlineAsciiResult::kAccepted;
        }
        return InlineAsciiResult::kNotHandled;
    }

    if (!chinese_mode || ascii_mode_ || ctx.is_composing() || event.is_caps_lock() ||
        !event.is_shift() || !is_letter_key(event.keycode)) {
        return InlineAsciiResult::kNotHandled;
    }

    const std::optional<char> normalized = normalize_ascii_key(event);
    if (!normalized ||
        !ctx.start_composition(CompositionScheme::kInlineAscii, std::string(1, *normalized), 1)) {
        return InlineAsciiResult::kNotHandled;
    }
    ctx.clear_translation();
    ctx.reset_pagination();
    return InlineAsciiResult::kAccepted;
}

AsciiModeSwitchStyle AsciiComposer::get_binding(uint32_t key_code) const {
    auto it = bindings_.find(key_code);
    if (it != bindings_.end())
        return it->second;

    // TSF sends generic VK_SHIFT/VK_CONTROL instead of left/right variants.
    // Fall back to left-key binding if the generic key has no explicit binding.
    if (key_code == VK_SHIFT)
        it = bindings_.find(VK_LSHIFT);
    else if (key_code == VK_CONTROL)
        it = bindings_.find(VK_LCONTROL);
    else if (key_code == VK_MENU)
        it = bindings_.find(VK_LMENU);

    return it != bindings_.end() ? it->second : AsciiModeSwitchStyle::NOOP;
}

void AsciiComposer::toggle_mode(uint32_t key_code, Context& ctx) {
    auto style = get_binding(key_code);
    bool composing = ctx.is_composing();

    CXXIME_LOG(L"AsciiComposer::toggle_mode: vk=%u, style=%d, composing=%d, input='%S'",
               key_code, (int)style, composing, ctx.active_input().c_str());

    switch (style) {
    case AsciiModeSwitchStyle::NOOP:
        return;

    case AsciiModeSwitchStyle::SET_ASCII_MODE:
        if (composing && ctx.composition_scheme() != CompositionScheme::kInlineAscii) {
            ctx.enter_inline_ascii(false);
            ctx.clear_translation();
            ctx.reset_pagination();
        }
        set_ascii_mode_from_switch(true);
        return;

    case AsciiModeSwitchStyle::UNSET_ASCII_MODE:
        set_ascii_mode_from_switch(false);
        return;

    case AsciiModeSwitchStyle::INLINE_ASCII:
        if (composing) {
            if (ctx.composition_scheme() != CompositionScheme::kInlineAscii) {
                ctx.enter_inline_ascii(true);
                ctx.clear_translation();
                ctx.reset_pagination();
                if (!ascii_mode_) {
                    set_ascii_mode_from_switch(true);
                    temporary_ascii_ = true;
                }
            }
            return;
        }
        set_ascii_mode_from_switch(!ascii_mode_);
        return;

    case AsciiModeSwitchStyle::CODE:
        if (composing) {
            commit_raw_composition(ctx);
        } else {
            CXXIME_LOG(L"AsciiComposer::toggle_mode: CODE, not composing");
        }
        set_ascii_mode_from_switch(!ascii_mode_);
        return;

    case AsciiModeSwitchStyle::CLEAR:
        if (composing)
            ctx.reset();
        set_ascii_mode_from_switch(!ascii_mode_);
        return;

    case AsciiModeSwitchStyle::CANDIDATE:
        if (composing) {
            commit_candidate_or_raw(ctx);
        }
        set_ascii_mode_from_switch(!ascii_mode_);
        return;

    case AsciiModeSwitchStyle::APPEND:
        // CapsLock in append mode does nothing on its own —
        // letter handling is deferred to PinyinProcessor.
        return;
    }
}

void AsciiComposer::toggle_by_shortcut(Context& ctx) {
    if (ctx.is_composing()) {
        commit_raw_composition(ctx);
    }
    set_ascii_mode_from_switch(!ascii_mode_);
}

void AsciiComposer::finish_temporary_ascii() {
    if (temporary_ascii_) {
        set_ascii_mode_from_switch(false);
    }
}

void AsciiComposer::commit_raw_composition(Context& ctx) {
    ctx.finalize_raw(CommitSource::kRawCodePreserveCase);
}

void AsciiComposer::commit_candidate_or_raw(Context& ctx) {
    auto committed = ctx.commit_with_source();
    ctx.committed_text = std::move(committed.first);
    ctx.set_commit_source(committed.second == CommitSource::kRawCode
                              ? CommitSource::kRawCodePreserveCase
                              : committed.second);
}

void AsciiComposer::set_ascii_mode_from_switch(bool mode) {
    ascii_mode_ = mode;
    temporary_ascii_ = false;
    caps_lock_overlay_active_ = false;
}

void AsciiComposer::apply_caps_lock_overlay(bool caps_lock, Context& ctx) {
    auto style = get_binding(VK_CAPITAL);
    if (style == AsciiModeSwitchStyle::NOOP)
        return;

    if (caps_lock) {
        if (!caps_lock_overlay_active_) {
            ascii_mode_before_caps_lock_ = ascii_mode_;
            caps_lock_overlay_active_ = true;
        }
        if (!ascii_mode_) {
            bool composing = ctx.is_composing();
            switch (style) {
            case AsciiModeSwitchStyle::APPEND:
                if (composing)
                    return;
                break;
            case AsciiModeSwitchStyle::CODE:
                if (composing) {
                    commit_raw_composition(ctx);
                }
                break;
            case AsciiModeSwitchStyle::CLEAR:
                if (composing)
                    ctx.reset();
                break;
            case AsciiModeSwitchStyle::CANDIDATE:
                if (composing) {
                    commit_candidate_or_raw(ctx);
                }
                break;
            default:
                break;
            }
        }
        ascii_mode_ = true;
        temporary_ascii_ = false;
        caps_lock_overlay_active_ = true;
        return;
    }

    if (caps_lock_overlay_active_) {
        ascii_mode_ = ascii_mode_before_caps_lock_;
        temporary_ascii_ = false;
        caps_lock_overlay_active_ = false;
    }
}

} // namespace cxxime
