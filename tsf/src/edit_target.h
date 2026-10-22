// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_EDIT_TARGET_H_
#define CXXIME_TSF_EDIT_TARGET_H_

#include <cstdint>

#include <windows.h>
#include <msctf.h>

namespace cxxime_tsf {

struct TextExtRectTrace {
    RECT raw = {};
    RECT result = {};
    HWND view_hwnd = nullptr;
    HWND foreground_hwnd = nullptr;
    HWND caret_hwnd = nullptr;
    HWND focus_hwnd = nullptr;
    POINT caret_pos = {};
    HRESULT text_ext_hr = E_UNEXPECTED;
    const char* branch = "text_ext_failed";
    bool clipped = false;
    bool resolved = false;
    bool gui_info_ok = false;
    bool caret_pos_queried = false;
    bool caret_pos_ok = false;
    bool caret_map_ok = false;
    // Issue #24, a caret with no height: the other places tried (alternatives = they were).
    bool alternatives = false;
    HRESULT composition_hr = E_UNEXPECTED;  // GetTextExt of the whole composition
    RECT composition_rect = {};
    HWND foreground_caret_hwnd = nullptr;   // the foreground thread's caret and focus
    HWND foreground_focus_hwnd = nullptr;
    RECT foreground_caret = {};
    bool imm_context = false;
    // imm_candidate_caret_rect: the program's CANDIDATEFORM (read, its style, the caret used).
    bool imm_read = false;
    DWORD imm_style = 0;
    RECT imm_rect = {};
};

enum class CaretViewportFallback : uint8_t {
    None = 0,
    Anchor,
    Projected,
    Boundary,
};

class CaretViewportTracker {
public:
    bool remember(std::uint64_t target_generation,
                  const RECT& view_rect,
                  const RECT& caret_rect);
    CaretViewportFallback resolve(std::uint64_t target_generation,
                                  const RECT* view_rect,
                                  const RECT* logical_rect,
                                  bool clipped,
                                  RECT* caret_rect) const;

private:
    std::uint64_t target_generation_ = 0;
    RECT view_rect_ = {};
    RECT caret_rect_ = {};
    bool valid_ = false;
};

bool text_rect_is_outside_view(HRESULT screen_rect_hr, const RECT& screen_rect,
    HRESULT text_rect_hr, const RECT& text_rect, bool text_clipped);
bool text_rect_is_placeholder(const RECT& view_rect, const RECT& text_rect);
bool text_rect_requires_composition_refresh(const RECT& view_rect, const RECT& text_rect);
bool map_fallback_caret_rect(HWND caret_window, POINT caret, RECT* rect);
bool map_current_thread_caret_rect(HWND foreground, RECT* rect,
                                   TextExtRectTrace* trace = nullptr);
bool resolve_native_caret_rect(HWND foreground, RECT* rect);
bool normalize_text_ext_rect(HWND view_hwnd, HWND foreground, RECT* rect,
                             TextExtRectTrace* trace = nullptr);
// Where an IMM program asked for the candidate window (ImmSetCandidateWindow: a position, or
// the text the window must not cover), as a screen caret rect the window goes below. False
// when the program set none (most do not) or it lies outside `hwnd`'s top-level window.
// Issue #24: Age of Empires II DE gives TSF a caret with no height at its window's bottom-right
// corner and its chat box only this way.
bool imm_candidate_caret_rect(HWND hwnd, RECT* rect, TextExtRectTrace* trace = nullptr);
// The system caret of `foreground`'s thread, on any of its windows (a game's single top-level
// window too), in screen coordinates; false when it has none inside the window. `focus` gets
// that thread's focus window when given.
bool foreground_caret_rect(HWND foreground, RECT* rect, HWND* focus = nullptr,
                           TextExtRectTrace* trace = nullptr);

} // namespace cxxime_tsf

#endif // CXXIME_TSF_EDIT_TARGET_H_
