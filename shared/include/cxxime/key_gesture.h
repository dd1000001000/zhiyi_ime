// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// One way to read a press of any shortcut (docs/key-gestures.md): the switch key combinations,
// the tapped Shift / Ctrl / Alt, the page keys, \ and Ctrl+0..9. The text service feeds every key
// down and up it gets (and a timer while a key is held); out come presses, taps and long presses.
// A shortcut without a long-press action does what it did before (the text service passes its
// key on as before); one with it (the voice key) gets kHoldStart / kHoldEnd.
//
// While a shortcut is held another key ends it:
// - a held Shift / Ctrl / Alt: any other key makes a combination with it (Ctrl+C, Ctrl+Space), so
//   it is as if it was not pressed: a started long press is cancelled (kCancel), and the other
//   key goes on as the combination it makes;
// - a held combination, page key, \ or Ctrl+digit: another shortcut ends it as a release before
//   the other one starts (kHoldEnd kOtherShortcut, or kTap when it was not held long enough);
//   other keys leave it alone.
#ifndef CXXIME_KEY_GESTURE_H_
#define CXXIME_KEY_GESTURE_H_

#include <cstdint>
#include <functional>
#include <vector>

#include <cxxime/keyboard_shortcut.h>

namespace cxxime {

enum class GestureKeyKind : uint8_t {
    kCombination,  // a switch key combination: F1-F11, Shift+Space, Ctrl / Alt with a key
    kModifier,     // Shift / Ctrl / Alt pressed alone (one side: VK_LSHIFT ...)
    kKey,          // a key pressed without Ctrl / Alt (the page keys, \)
    kCtrlDigit,    // Ctrl+0..9 (learning mode: the translation)
};

struct GestureBinding {
    int id = 0;  // the caller's
    GestureKeyKind kind = GestureKeyKind::kCombination;
    KeyboardShortcut combination;  // kCombination
    uint32_t key = 0;              // kModifier, kKey
    bool hold = false;             // has a long-press action: kHoldStart / kHoldEnd
    bool while_composing = false;  // a shortcut only while composing (page keys, \, Ctrl+digit)
};

enum class GestureType : uint8_t {
    kPress,      // a combination, key or Ctrl+digit went down (what it does happens now)
    kRepeat,     // the auto-repeat of a held key or Ctrl+digit (page keys page on)
    kTap,        // released before the long press began (or ended by another shortcut so)
    kHoldStart,  // held for hold_ms with nothing else pressed (only bindings with `hold`)
    kHoldEnd,    // a long press ended (see `end`)
    kCancel,     // a held Shift / Ctrl / Alt whose long press began became a combination
};

enum class GestureEnd : uint8_t {
    kNone,
    kReleased,       // the key went up
    kOtherShortcut,  // another shortcut was pressed
    kFocusLost,      // the input box lost the focus
};

struct Gesture {
    int id = 0;
    GestureType type = GestureType::kPress;
    GestureEnd end = GestureEnd::kNone;
    uint32_t key = 0;  // the key pressed (for kCtrlDigit the digit)
};

class KeyGestureTracker {
public:
    void configure(std::vector<GestureBinding> bindings, uint32_t hold_ms);
    // A key down (auto-repeat included) or up as the program got it. `modifiers` are
    // kKeyModifier* held at the time; `composing` makes page keys, \ and Ctrl+digit shortcuts.
    std::vector<Gesture> key(uint32_t vk, bool up, uint32_t modifiers, bool composing,
                             uint64_t now_ms);
    // Called every few ms while active(): long presses begin, and a key whose key-up never came
    // (a system hotkey can keep it) ends when `held` says it is up.
    std::vector<Gesture> tick(uint64_t now_ms, const std::function<bool(uint32_t)>& held);
    // The input box lost the focus: every press ends.
    std::vector<Gesture> focus_lost();
    void clear() { pressed_.clear(); }

    bool active() const { return !pressed_.empty(); }
    // The binding id the key's current press belongs to, -1 when none.
    int owner(uint32_t vk) const;
    uint32_t hold_ms() const { return hold_ms_; }

private:
    struct Pressed {
        std::size_t binding = 0;
        uint32_t key = 0;
        uint64_t down_ms = 0;
        bool held = false;  // kHoldStart sent
        bool done = false;  // ended (combined, interrupted): nothing more until its key-up
    };

    int match(uint32_t vk, uint32_t modifiers, bool composing) const;
    void release(std::size_t index, std::vector<Gesture>* out);
    void end_by_other_shortcut(std::vector<Gesture>* out);

    std::vector<GestureBinding> bindings_;
    std::vector<Pressed> pressed_;
    uint32_t hold_ms_ = 400;
};

}  // namespace cxxime

#endif  // CXXIME_KEY_GESTURE_H_
