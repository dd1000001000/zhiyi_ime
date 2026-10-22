// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "edit_target.h"

#include <algorithm>

#include <imm.h>

namespace {

bool is_valid_rect(const RECT& rc) {
    return (rc.left != 0 || rc.top != 0) && rc.right >= rc.left && rc.bottom >= rc.top;
}

void normalize_rect_size(RECT* rc) {
    if (rc->right < rc->left) {
        std::swap(rc->left, rc->right);
    }
    if (rc->bottom < rc->top) {
        std::swap(rc->top, rc->bottom);
    }
    if (rc->right == rc->left) {
        rc->right = rc->left + 1;
    }
    if (rc->bottom - rc->top <= 2) {
        rc->bottom = rc->top + 20;
    }
}

bool rect_primary_point_in_rect(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.left <= outer.right && inner.top >= outer.top &&
           inner.top <= outer.bottom;
}

bool rect_has_area(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool caret_origin_in_view(const RECT& view_rect, const RECT& caret_rect) {
    return rect_has_area(view_rect) && is_valid_rect(caret_rect) &&
           caret_rect.left >= view_rect.left && caret_rect.left < view_rect.right &&
           caret_rect.top >= view_rect.top && caret_rect.top < view_rect.bottom;
}

bool same_root_window(HWND a, HWND b) {
    if (!a || !b) {
        return false;
    }
    if (a == b || IsChild(a, b) || IsChild(b, a)) {
        return true;
    }
    HWND root_a = GetAncestor(a, GA_ROOT);
    HWND root_b = GetAncestor(b, GA_ROOT);
    return root_a && root_a == root_b;
}

bool map_client_rect_to_screen(HWND hwnd, const RECT& raw, RECT* mapped) {
    if (!hwnd || !mapped) {
        return false;
    }
    RECT client = {};
    if (!GetClientRect(hwnd, &client)) {
        return false;
    }
    if (raw.left < client.left || raw.left > client.right || raw.top < client.top ||
        raw.top > client.bottom) {
        return false;
    }
    POINT points[2] = {
        {raw.left, raw.top},
        {raw.right, raw.bottom}
    };
    if (!MapWindowPoints(hwnd, nullptr, points, 2)) {
        return false;
    }
    SetRect(mapped, points[0].x, points[0].y, points[1].x, points[1].y);
    normalize_rect_size(mapped);
    return is_valid_rect(*mapped);
}

} // namespace

namespace cxxime_tsf {

bool CaretViewportTracker::remember(std::uint64_t target_generation, const RECT& view_rect,
                                    const RECT& caret_rect) {
    if (!caret_origin_in_view(view_rect, caret_rect)) {
        return false;
    }
    target_generation_ = target_generation;
    view_rect_ = view_rect;
    caret_rect_ = caret_rect;
    valid_ = true;
    return true;
}

CaretViewportFallback CaretViewportTracker::resolve(std::uint64_t target_generation,
                                                    const RECT* view_rect, const RECT* logical_rect,
                                                    bool clipped, RECT* caret_rect) const {
    if (!caret_rect) {
        return CaretViewportFallback::None;
    }

    const bool has_anchor = valid_ && target_generation_ == target_generation;
    RECT anchor = {};
    if (has_anchor) {
        anchor = caret_rect_;
        if (view_rect && rect_has_area(*view_rect) && rect_has_area(view_rect_)) {
            OffsetRect(&anchor, view_rect->left - view_rect_.left,
                       view_rect->top - view_rect_.top);
        }
    }

    if (view_rect && logical_rect && !clipped && rect_has_area(*view_rect) &&
        is_valid_rect(*logical_rect)) {
        const bool vertically_hidden =
            logical_rect->bottom <= view_rect->top || logical_rect->top >= view_rect->bottom;
        const bool horizontal_position_available =
            logical_rect->left >= view_rect->left && logical_rect->left < view_rect->right;
        if (vertically_hidden && horizontal_position_available) {
            if (has_anchor) {
                OffsetRect(&anchor, logical_rect->left - anchor.left, 0);
                *caret_rect = anchor;
                return CaretViewportFallback::Projected;
            }

            RECT boundary = *logical_rect;
            normalize_rect_size(&boundary);
            const LONG caret_height = boundary.bottom - boundary.top;
            const LONG view_height = view_rect->bottom - view_rect->top;
            if (caret_height <= view_height) {
                const LONG edge = logical_rect->top >= view_rect->bottom
                                      ? view_rect->bottom - boundary.bottom
                                      : view_rect->top - boundary.top;
                OffsetRect(&boundary, 0, edge);
                *caret_rect = boundary;
                return CaretViewportFallback::Boundary;
            }
        }
    }

    if (!has_anchor) {
        return CaretViewportFallback::None;
    }
    *caret_rect = anchor;
    return CaretViewportFallback::Anchor;
}

bool map_fallback_caret_rect(HWND caret_window, POINT caret, RECT* rect) {
    if (!caret_window || !rect || !ClientToScreen(caret_window, &caret)) {
        return false;
    }
    OffsetRect(rect, caret.x - rect->left, caret.y - rect->top);
    return true;
}

bool map_current_thread_caret_rect(HWND foreground, RECT* rect, TextExtRectTrace* trace) {
    if (!rect || !foreground) {
        return false;
    }
    GUITHREADINFO gui = {sizeof(gui)};
    const bool gui_ok = GetGUIThreadInfo(GetCurrentThreadId(), &gui) != FALSE;
    if (trace) {
        trace->gui_info_ok = gui_ok;
        trace->caret_hwnd = gui_ok ? gui.hwndCaret : nullptr;
    }
    if (!gui_ok || !gui.hwndCaret || !same_root_window(foreground, gui.hwndCaret)) {
        if (trace) {
            // The old fallback could use GetCaretPos even without a confirmed owner.
            trace->caret_pos_queried = true;
            trace->caret_pos_ok = GetCaretPos(&trace->caret_pos) != FALSE;
        }
        return false;
    }
    POINT caret = {};
    const bool caret_ok = GetCaretPos(&caret) != FALSE;
    if (trace) {
        trace->caret_pos_queried = true;
        trace->caret_pos_ok = caret_ok;
        trace->caret_pos = caret;
    }
    const bool mapped = caret_ok && map_fallback_caret_rect(gui.hwndCaret, caret, rect);
    if (trace) {
        trace->caret_map_ok = mapped;
    }
    return mapped;
}

bool resolve_native_caret_rect(HWND foreground, RECT* out) {
    if (!out) {
        return false;
    }
    GUITHREADINFO gti = {sizeof(gti)};
    DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (foreground_thread && GetGUIThreadInfo(foreground_thread, &gti) && gti.hwndCaret &&
        GetAncestor(gti.hwndCaret, GA_ROOT) != gti.hwndCaret &&
        same_root_window(foreground, gti.hwndCaret)) {
        RECT rc = gti.rcCaret;
        POINT points[2] = {
            {rc.left, rc.top},
            {rc.right, rc.bottom}
        };
        MapWindowPoints(gti.hwndCaret, nullptr, points, 2);
        SetRect(&rc, points[0].x, points[0].y, points[1].x, points[1].y);
        normalize_rect_size(&rc);
        if (is_valid_rect(rc)) {
            *out = rc;
            return true;
        }
    }

    RECT rc = {0, 0, 1, 20};
    if (map_current_thread_caret_rect(foreground, &rc) && is_valid_rect(rc)) {
        *out = rc;
        return true;
    }
    return false;
}

bool normalize_text_ext_rect(HWND view_hwnd, HWND foreground, RECT* rc,
                             TextExtRectTrace* trace) {
    if (!rc || !is_valid_rect(*rc)) {
        if (trace) {
            trace->branch = "invalid_rect";
        }
        return false;
    }
    RECT foreground_rect = {};
    bool has_foreground_rect = foreground && GetWindowRect(foreground, &foreground_rect);
    normalize_rect_size(rc);

    if (has_foreground_rect && rect_primary_point_in_rect(foreground_rect, *rc)) {
        if (trace) {
            trace->branch = "foreground_screen";
        }
        return true;
    }
    RECT mapped = {};
    if (map_client_rect_to_screen(view_hwnd, *rc, &mapped)) {
        if (!has_foreground_rect || rect_primary_point_in_rect(foreground_rect, mapped)) {
            *rc = mapped;
            if (trace) {
                trace->branch = "view_client";
            }
            return true;
        }
    }
    if (has_foreground_rect && trace) {
        trace->focus_hwnd = GetFocus();
    }
    if (has_foreground_rect && map_current_thread_caret_rect(foreground, rc, trace)) {
        normalize_rect_size(rc);
        if (trace) {
            trace->branch = "native_caret";
        }
        return is_valid_rect(*rc);
    }
    const bool on_monitor = MonitorFromRect(rc, MONITOR_DEFAULTTONULL) != nullptr;
    if (trace) {
        trace->branch = on_monitor ? "monitor" : "unresolved";
    }
    return on_monitor;
}

bool imm_candidate_caret_rect(HWND hwnd, RECT* out, TextExtRectTrace* trace) {
    if (!hwnd || !out) {
        return false;
    }
    HIMC himc = ImmGetContext(hwnd);
    if (trace) {
        trace->imm_context = himc != nullptr;
    }
    if (!himc) {
        return false;
    }
    CANDIDATEFORM form = {};
    const bool read = ImmGetCandidateWindow(himc, 0, &form) != FALSE;
    ImmReleaseContext(hwnd, himc);
    if (trace) {
        trace->imm_read = read;
        trace->imm_style = read ? form.dwStyle : 0;
    }
    if (!read) {
        return false;
    }
    // In the client area of the window: the text to keep clear, else a caret just above the
    // point where the window's top-left goes.
    RECT client = {};
    if ((form.dwStyle & CFS_EXCLUDE) && rect_has_area(form.rcArea)) {
        client = form.rcArea;
    } else if (form.dwStyle & (CFS_EXCLUDE | CFS_CANDIDATEPOS)) {
        SetRect(&client, form.ptCurrentPos.x, form.ptCurrentPos.y - 20, form.ptCurrentPos.x + 1,
                form.ptCurrentPos.y);
    } else {
        return false;
    }
    POINT points[2] = {
        {client.left, client.top},
        {client.right, client.bottom}
    };
    MapWindowPoints(hwnd, nullptr, points, 2);
    RECT rect = {};
    SetRect(&rect, points[0].x, points[0].y, points[1].x, points[1].y);
    normalize_rect_size(&rect);
    RECT window = {};
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!is_valid_rect(rect) || !GetWindowRect(root ? root : hwnd, &window) ||
        !rect_primary_point_in_rect(window, rect)) {
        return false;
    }
    if (trace) {
        trace->imm_rect = rect;
    }
    *out = rect;
    return true;
}

bool foreground_caret_rect(HWND foreground, RECT* out, HWND* focus, TextExtRectTrace* trace) {
    if (!foreground || !out) {
        return false;
    }
    GUITHREADINFO gui = {sizeof(gui)};
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    if (!thread || !GetGUIThreadInfo(thread, &gui)) {
        return false;
    }
    if (focus) {
        *focus = gui.hwndFocus;
    }
    if (trace) {
        trace->foreground_caret_hwnd = gui.hwndCaret;
        trace->foreground_focus_hwnd = gui.hwndFocus;
        trace->foreground_caret = gui.rcCaret;
    }
    if (!gui.hwndCaret || !same_root_window(foreground, gui.hwndCaret)) {
        return false;
    }
    POINT points[2] = {
        {gui.rcCaret.left, gui.rcCaret.top},
        {gui.rcCaret.right, gui.rcCaret.bottom}
    };
    MapWindowPoints(gui.hwndCaret, nullptr, points, 2);
    RECT rect = {};
    SetRect(&rect, points[0].x, points[0].y, points[1].x, points[1].y);
    normalize_rect_size(&rect);
    RECT window = {};
    if (!is_valid_rect(rect) || !GetWindowRect(foreground, &window) ||
        !rect_primary_point_in_rect(window, rect)) {
        return false;
    }
    *out = rect;
    return true;
}

bool text_rect_is_outside_view(HRESULT screen_rect_hr, const RECT& screen_rect,
    HRESULT text_rect_hr, const RECT& text_rect, bool text_clipped) {
    const bool screen_rect_valid = SUCCEEDED(screen_rect_hr) &&
                                   screen_rect.right > screen_rect.left &&
                                   screen_rect.bottom > screen_rect.top;
    return SUCCEEDED(text_rect_hr) && screen_rect_valid && !text_clipped &&
           (text_rect.right <= screen_rect.left || text_rect.left >= screen_rect.right ||
            text_rect.bottom <= screen_rect.top || text_rect.top >= screen_rect.bottom);
}

bool text_rect_is_placeholder(const RECT& view_rect, const RECT& text_rect) {
    const bool valid_view =
        view_rect.right > view_rect.left && view_rect.bottom > view_rect.top;
    const bool valid_text =
        text_rect.right > text_rect.left && text_rect.bottom > text_rect.top;
    if (!valid_view || !valid_text || text_rect.right - text_rect.left > 2 ||
        view_rect.right - view_rect.left <= 100 ||
        view_rect.bottom - view_rect.top <= 100) {
        return false;
    }

    constexpr LONG kBoundaryTolerance = 2;
    const bool at_view_origin =
        text_rect.left >= view_rect.left - kBoundaryTolerance &&
        text_rect.left <= view_rect.left + kBoundaryTolerance &&
        text_rect.top >= view_rect.top - kBoundaryTolerance &&
        text_rect.top <= view_rect.top + kBoundaryTolerance;
    return at_view_origin;
}

bool text_rect_requires_composition_refresh(const RECT& view_rect,
    const RECT& text_rect) {
    const bool valid_view =
        view_rect.right > view_rect.left && view_rect.bottom > view_rect.top;
    const bool valid_text =
        text_rect.right >= text_rect.left && text_rect.bottom >= text_rect.top;
    if (!valid_view || !valid_text || text_rect.right - text_rect.left > 2 ||
        view_rect.right - view_rect.left <= 100 ||
        view_rect.bottom - view_rect.top <= 100) {
        return false;
    }

    constexpr LONG kBoundaryTolerance = 2;
    const bool at_view_origin =
        text_rect.left >= view_rect.left - kBoundaryTolerance &&
        text_rect.left <= view_rect.left + kBoundaryTolerance &&
        text_rect.top >= view_rect.top - kBoundaryTolerance &&
        text_rect.top <= view_rect.top + kBoundaryTolerance;
    // Qt can expose an uninitialized selection just beyond a full-screen view's right edge.
    const bool outside_right_boundary =
        text_rect.left >= view_rect.right &&
        text_rect.left <= view_rect.right + kBoundaryTolerance &&
        text_rect.right > view_rect.right &&
        text_rect.top >= view_rect.top &&
        text_rect.top < view_rect.bottom;
    return at_view_origin || outside_right_boundary;
}
} // namespace cxxime_tsf
