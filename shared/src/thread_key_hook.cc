// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/thread_key_hook.h>

#include <map>
#include <memory>
#include <mutex>

#include <windows.h>

#include <cxxime/key_event.h>

namespace cxxime {

namespace {

struct ThreadHook {
    HHOOK hook = nullptr;
    std::shared_ptr<HookedKeyHandler> handler;
};

std::mutex g_hooks_mutex;
std::map<DWORD, ThreadHook> g_hooks;  // by thread id

bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

LRESULT CALLBACK thread_key_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wparam, lparam);
    std::shared_ptr<HookedKeyHandler> handler;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        const auto found = g_hooks.find(GetCurrentThreadId());
        if (found != g_hooks.end()) handler = found->second.handler;
    }
    if (!handler) return CallNextHookEx(nullptr, code, wparam, lparam);

    const uint32_t flags = static_cast<uint32_t>(lparam);
    HookedKey key;
    key.key_data = flags;
    key.up = (flags & (1u << 31)) != 0;
    key.repeat = !key.up && (flags & (1u << 30)) != 0;
    key.vk = modifier_key_with_side(static_cast<uint32_t>(wparam), flags);
    if (key_down(VK_SHIFT)) key.modifiers |= kKeyModifierShift;
    if (key_down(VK_CONTROL)) key.modifiers |= kKeyModifierControl;
    if (key_down(VK_MENU)) key.modifiers |= kKeyModifierAlt;
    // WM_SYSKEYDOWN / WM_SYSKEYUP: Alt itself, or a key with Alt held (the context code), both
    // without Ctrl; Ctrl+Alt keys are ordinary key messages, which input methods get.
    const bool alt_context = (flags & (1u << 29)) != 0;
    key.system = (is_alt_key(key.vk) || alt_context) && !key_down(VK_CONTROL);
    // The physical state: the thread may have missed the other Alt's key-up (Alt+Tab).
    if (key.vk == VK_LMENU) key.other_alt = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    if (key.vk == VK_RMENU) key.other_alt = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
    if ((*handler)(key)) return 1;  // the program does not get it
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

HINSTANCE this_module() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thread_key_hook_proc), &module);
    return module;
}

}  // namespace

bool install_thread_key_hook(HookedKeyHandler handler) {
    const DWORD thread = GetCurrentThreadId();
    auto shared = std::make_shared<HookedKeyHandler>(std::move(handler));
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    ThreadHook& entry = g_hooks[thread];
    if (!entry.hook) {
        entry.hook = SetWindowsHookExW(WH_KEYBOARD, thread_key_hook_proc, this_module(), thread);
        if (!entry.hook) {
            g_hooks.erase(thread);
            return false;
        }
    }
    entry.handler = std::move(shared);
    return true;
}

void remove_thread_key_hook() {
    HHOOK hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        const auto found = g_hooks.find(GetCurrentThreadId());
        if (found == g_hooks.end()) return;
        hook = found->second.hook;
        g_hooks.erase(found);
    }
    if (hook) UnhookWindowsHookEx(hook);
}

bool thread_key_hook_installed() {
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    return g_hooks.count(GetCurrentThreadId()) != 0;
}

void remove_all_thread_key_hooks() {
    std::map<DWORD, ThreadHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        hooks.swap(g_hooks);
    }
    for (const auto& [thread, entry] : hooks) {
        if (entry.hook) UnhookWindowsHookEx(entry.hook);
    }
}

bool is_alt_key(uint32_t vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }

uint32_t AltTap::key(const HookedKey& key) {
    if (is_alt_key(key.vk)) {
        if (!key.up) {
            // Its own bit shows already; another modifier held makes a combination (AltGr is
            // Ctrl + right Alt), and so does the other Alt.
            if (!key.repeat) {
                const bool alone = (key.modifiers & ~kKeyModifierAlt) == 0 && !key.other_alt;
                down_ = alone ? key.vk : 0;
            }
            return 0;
        }
        const uint32_t tapped = down_ == key.vk ? key.vk : 0;
        down_ = 0;
        return tapped;
    }
    if (!key.up) down_ = 0;  // another key in between: a combination
    return 0;
}

}  // namespace cxxime
