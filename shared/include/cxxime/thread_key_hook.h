// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The keys Windows keeps from input methods (docs/key-gestures.md): Alt, and the keys typed
// while Alt is held without Ctrl (WM_SYSKEYDOWN / WM_SYSKEYUP). Neither TSF nor IMM hands them
// to the input method, so a tapped Alt switch key (ascii_composer.switch_key Alt_L / Alt_R), and
// in IMM programs an Alt switch key combination, would never act. Both input method modules
// install a thread keyboard hook (WH_KEYBOARD) on the program's thread while they are active:
// it is called as the program takes each key from its queue, so no other program's key passes
// through it. It sees every key of the thread; the modules act only on the system keys and
// leave the others to their own key paths, so each key is read once.
#ifndef CXXIME_THREAD_KEY_HOOK_H_
#define CXXIME_THREAD_KEY_HOOK_H_

#include <cstdint>
#include <functional>

namespace cxxime {

struct HookedKey {
    uint32_t vk = 0;         // Shift / Ctrl / Alt with their side (VK_LSHIFT ... VK_RMENU)
    bool up = false;
    bool repeat = false;     // the auto-repeat of a held key
    bool system = false;     // kept from input methods: Alt, or a key typed with Alt held
    uint32_t modifiers = 0;  // kKeyModifierShift / Control / Alt held at the time
    bool other_alt = false;  // an Alt key: the other Alt is held too
    uint32_t key_data = 0;   // the key message's lParam (repeat count, scan code, flags)
};

// Runs on the program's thread inside its GetMessage / PeekMessage, where IMM calls
// ImeProcessKey: it must not block (TSF posts what changes the text, as no edit session can be
// had here). True: the program does not get the key.
using HookedKeyHandler = std::function<bool(const HookedKey&)>;

// The calling thread's hook; installing again replaces its handler. False when Windows refused.
bool install_thread_key_hook(HookedKeyHandler handler);
void remove_thread_key_hook();
bool thread_key_hook_installed();  // on the calling thread
// Before the module unloads: every thread's hook.
void remove_all_thread_key_hooks();

bool is_alt_key(uint32_t vk);

// A tap of Alt: pressed and released with no other key in between and no other modifier held.
// Fed every key of the thread (not only the system keys: Ctrl pressed while Alt is held is not
// one).
class AltTap {
public:
    // The Alt key (VK_LMENU / VK_RMENU) tapped when `key` is its key-up, else 0.
    uint32_t key(const HookedKey& key);
    void reset() { down_ = 0; }

private:
    uint32_t down_ = 0;  // the Alt pressed alone, until another key
};

}  // namespace cxxime

#endif  // CXXIME_THREAD_KEY_HOOK_H_
