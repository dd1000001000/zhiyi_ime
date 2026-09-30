// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_WINDOW_POSITION_H_
#define CXXIME_WINDOW_POSITION_H_

#include <vector>

#include <windows.h>

namespace cxxime {

enum class CandidatePlacementSide {
    Unset,
    Below,
    Above,
};

struct CandidateWindowPlacement {
    POINT position = {};
    CandidatePlacementSide side = CandidatePlacementSide::Unset;
};

// A negative width or height is treated as zero.
POINT clamp_window_position_to_work_area(int x, int y, int width, int height,
                                         const RECT& work_area);

// Keep single-screen drags visible; allow multi-screen drags across ordinary edges.
// Conflicting reserved edges resolve into the pointer's target work area, independent of order.
POINT constrain_status_drag_position(int x, int y, int width, int height,
                                     const std::vector<MONITORINFO>& monitors,
                                     const RECT& target_work_area);

CandidateWindowPlacement calculate_candidate_window_position(
    const RECT& caret_rect, int width, int height, int caret_gap,
    const RECT& monitor_rect, CandidatePlacementSide previous_side);

bool logical_screen_rect_to_physical(HWND window, const RECT& source, RECT* result);
bool rect_covers_monitor(const RECT& rect, const RECT& monitor_rect);
bool is_fullscreen_window(HWND window);

} // namespace cxxime

#endif // CXXIME_WINDOW_POSITION_H_
