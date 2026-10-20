// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/key_gesture.h>

#include <algorithm>

#include <cxxime/key_event.h>

namespace cxxime {

namespace {

constexpr uint32_t kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12;
constexpr uint32_t kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLControl = 0xA2, kVkRControl = 0xA3,
                   kVkLMenu = 0xA4, kVkRMenu = 0xA5;

// The modifier bit a Shift / Ctrl / Alt key sets, 0 for other keys.
uint32_t modifier_bit(uint32_t vk) {
    switch (vk) {
    case kVkShift: case kVkLShift: case kVkRShift: return kKeyModifierShift;
    case kVkControl: case kVkLControl: case kVkRControl: return kKeyModifierControl;
    case kVkMenu: case kVkLMenu: case kVkRMenu: return kKeyModifierAlt;
    default: return 0;
    }
}

}  // namespace

void KeyGestureTracker::configure(std::vector<GestureBinding> bindings, uint32_t hold_ms) {
    bindings_ = std::move(bindings);
    hold_ms_ = hold_ms;
    pressed_.clear();
}

int KeyGestureTracker::owner(uint32_t vk) const {
    for (const Pressed& p : pressed_) {
        if (p.key == vk) return bindings_[p.binding].id;
    }
    return -1;
}

int KeyGestureTracker::match(uint32_t vk, uint32_t modifiers, bool composing) const {
    const uint32_t mods = modifiers & kShortcutModifierMask;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const GestureBinding& b = bindings_[i];
        if (b.while_composing && !composing) continue;
        switch (b.kind) {
        case GestureKeyKind::kCombination: {
            KeyEvent event;
            event.keycode = vk;
            event.modifiers = mods;
            if (b.combination.matches(event)) return static_cast<int>(i);
            break;
        }
        case GestureKeyKind::kModifier:
            // Alone: no other modifier held (its own bit may already show).
            if (vk == b.key && (mods & ~modifier_bit(vk)) == 0) return static_cast<int>(i);
            break;
        case GestureKeyKind::kKey:
            if (vk == b.key && (mods & (kKeyModifierControl | kKeyModifierAlt)) == 0) {
                return static_cast<int>(i);
            }
            break;
        case GestureKeyKind::kCtrlDigit:
            if (vk >= '0' && vk <= '9' && mods == kKeyModifierControl) return static_cast<int>(i);
            break;
        }
    }
    return -1;
}

void KeyGestureTracker::release(std::size_t index, std::vector<Gesture>* out) {
    const Pressed p = pressed_[index];
    pressed_.erase(pressed_.begin() + static_cast<std::ptrdiff_t>(index));
    if (p.done) return;
    const GestureBinding& b = bindings_[p.binding];
    if (p.held) {
        out->push_back({b.id, GestureType::kHoldEnd, GestureEnd::kReleased, p.key});
        return;
    }
    if (b.kind == GestureKeyKind::kModifier) {
        // A tap of Shift / Ctrl / Alt is a shortcut too: a held combination ends first.
        end_by_other_shortcut(out);
    }
    out->push_back({b.id, GestureType::kTap, GestureEnd::kReleased, p.key});
}

void KeyGestureTracker::end_by_other_shortcut(std::vector<Gesture>* out) {
    for (Pressed& p : pressed_) {
        if (p.done || bindings_[p.binding].kind == GestureKeyKind::kModifier) continue;
        p.done = true;
        out->push_back({bindings_[p.binding].id, p.held ? GestureType::kHoldEnd : GestureType::kTap,
                        GestureEnd::kOtherShortcut, p.key});
    }
}

std::vector<Gesture> KeyGestureTracker::key(uint32_t vk, bool up, uint32_t modifiers,
                                            bool composing, uint64_t now_ms) {
    std::vector<Gesture> out;
    auto found = std::find_if(pressed_.begin(), pressed_.end(),
                              [vk](const Pressed& p) { return p.key == vk; });
    if (up) {
        if (found != pressed_.end()) {
            release(static_cast<std::size_t>(found - pressed_.begin()), &out);
        }
        return out;
    }
    if (found != pressed_.end()) {
        // Auto-repeat: page keys page on (and so do \ and Ctrl+digit, as before).
        const GestureBinding& b = bindings_[found->binding];
        if (!found->done &&
            (b.kind == GestureKeyKind::kKey || b.kind == GestureKeyKind::kCtrlDigit)) {
            out.push_back({b.id, GestureType::kRepeat, GestureEnd::kNone, vk});
        }
        return out;
    }

    // Another key went down: a held Shift / Ctrl / Alt makes a combination with it.
    for (Pressed& p : pressed_) {
        if (p.done || bindings_[p.binding].kind != GestureKeyKind::kModifier) continue;
        p.done = true;
        if (p.held) {
            out.push_back({bindings_[p.binding].id, GestureType::kCancel, GestureEnd::kNone,
                           p.key});
        }
    }
    const int index = match(vk, modifiers, composing);
    if (index < 0) return out;
    const GestureBinding& b = bindings_[static_cast<std::size_t>(index)];
    if (b.kind != GestureKeyKind::kModifier) {
        // A shortcut pressed: a held one ends as released. (A tapped modifier ends it at its
        // key-up, once it is known to be a tap.)
        end_by_other_shortcut(&out);
        out.push_back({b.id, GestureType::kPress, GestureEnd::kNone, vk});
    }
    pressed_.push_back({static_cast<std::size_t>(index), vk, now_ms, false, false});
    return out;
}

std::vector<Gesture> KeyGestureTracker::tick(uint64_t now_ms,
                                             const std::function<bool(uint32_t)>& held) {
    std::vector<Gesture> out;
    for (std::size_t i = 0; i < pressed_.size();) {
        if (held && !held(pressed_[i].key)) {
            release(i, &out);  // the key-up was not seen
            continue;
        }
        ++i;
    }
    for (Pressed& p : pressed_) {
        const GestureBinding& b = bindings_[p.binding];
        if (p.done || p.held || !b.hold || now_ms - p.down_ms < hold_ms_) continue;
        p.held = true;
        out.push_back({b.id, GestureType::kHoldStart, GestureEnd::kNone, p.key});
    }
    return out;
}

std::vector<Gesture> KeyGestureTracker::focus_lost() {
    std::vector<Gesture> out;
    for (const Pressed& p : pressed_) {
        if (!p.done && p.held) {
            out.push_back({bindings_[p.binding].id, GestureType::kHoldEnd, GestureEnd::kFocusLost,
                           p.key});
        }
    }
    pressed_.clear();
    return out;
}

}  // namespace cxxime
