// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_ASCII_COMPOSER_H_
#define CXXIME_ASCII_COMPOSER_H_

#include <cstdint>
#include <string>
#include <unordered_map>

namespace cxxime {

class Context;
struct KeyEvent;
struct Config;

enum class AsciiModeSwitchStyle {
    NOOP,
    INLINE_ASCII,
    CODE,          // commit raw buffer + toggle
    CLEAR,
    SET_ASCII_MODE,
    UNSET_ASCII_MODE,
    CANDIDATE,     // commit highlighted/first candidate + toggle
    APPEND,        // CapsLock modifies letters in buffer, no mode switch
};

enum class InlineAsciiResult {
    kNotHandled,
    kAccepted,
    kResumeOrigin,
};

class AsciiComposer {
public:
    void load_config(const Config& config);

    // Process a key event for modifier tracking and mode toggle.
    // Returns true when a configured modifier binding is applied on key-up.
    // caps_lock is accepted for existing call sites; mode switching is based on key events.
    bool process_key(uint32_t key_code, bool is_key_up, Context& ctx, bool caps_lock = false);
    InlineAsciiResult process_inline_ascii_composition(const KeyEvent& event, Context& ctx,
                                                       bool chinese_mode);

    bool is_ascii_mode() const { return ascii_mode_; }
    void set_ascii_mode(bool mode) {
        ascii_mode_ = mode;
        temporary_ascii_ = false;
        caps_lock_overlay_active_ = false;
    }
    bool is_temporary_ascii() const { return temporary_ascii_; }
    // Chinese/English switch shortcut (Config::ascii_toggle_shortcut): like tapping a switch
    // modifier with the "code" style, the letters typed so far are committed as they are.
    void toggle_by_shortcut(Context& ctx);
    void finish_temporary_ascii();
    void sync_caps_lock(bool caps_lock, Context& ctx) { apply_caps_lock_overlay(caps_lock, ctx); }

    AsciiModeSwitchStyle get_binding(uint32_t key_code) const;

private:
    void toggle_mode(uint32_t key_code, Context& ctx);
    void set_ascii_mode_from_switch(bool mode);
    void apply_caps_lock_overlay(bool caps_lock, Context& ctx);
    static void commit_raw_composition(Context& ctx);
    static void commit_candidate_or_raw(Context& ctx);

    std::unordered_map<uint32_t, AsciiModeSwitchStyle> bindings_;
    bool ascii_mode_ = false;
    bool temporary_ascii_ = false;
    bool caps_lock_overlay_active_ = false;
    bool ascii_mode_before_caps_lock_ = false;
    bool shift_pressed_ = false;
    bool ctrl_pressed_ = false;
    bool alt_pressed_ = false;
    bool win_pressed_ = false;

};

} // namespace cxxime

#endif // CXXIME_ASCII_COMPOSER_H_
