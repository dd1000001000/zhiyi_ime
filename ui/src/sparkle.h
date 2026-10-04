// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Laya recommendation mark: a four-point star with a small companion star in the space reserved
// after the recommended candidate (CandidateRect::mark_rect). For kSparkleDurationMs after the
// recommendation appears the stars twinkle (size / opacity pulse), then they stay still.
#ifndef CXXIME_UI_SPARKLE_H_
#define CXXIME_UI_SPARKLE_H_

#include <windows.h>

#include <array>
#include <cmath>

namespace cxxime {

constexpr unsigned kSparkleDurationMs = 1200;
constexpr unsigned kSparkleFrameMs = 40;

struct SparkleStar {
    float cx = 0, cy = 0, r = 0;  // center and outer radius (pixels)
    float alpha = 1;              // 0..1
};

// t_seconds < 0: steady state.
inline std::array<SparkleStar, 2> sparkle_stars(const RECT& mark, float t_seconds) {
    const float w = static_cast<float>(mark.right - mark.left);
    const float h = static_cast<float>(mark.bottom - mark.top);
    const float r = (std::min)(w * 0.46f, h * 0.27f);
    const float kPi = 3.14159265f;
    float main_scale = 1.0f, side_alpha = 0.85f, side_scale = 1.0f;
    if (t_seconds >= 0) {
        const float wave = std::sin(2 * kPi * 2.2f * t_seconds);
        main_scale = 0.78f + 0.22f * wave;
        side_alpha = 0.5f - 0.5f * wave;
        side_scale = 0.7f + 0.3f * (1 - wave) * 0.5f;
    }
    SparkleStar main;
    main.cx = mark.left + w * 0.55f;
    main.cy = mark.top + r + 1;
    main.r = r * main_scale;
    SparkleStar side;
    side.cx = mark.left + w * 0.18f;
    side.cy = main.cy + r * 1.25f;
    side.r = r * 0.45f * side_scale;
    side.alpha = side_alpha;
    return {main, side};
}

struct SparklePoint {
    float x = 0, y = 0;
};

// Outer and inner vertices, alternating, starting at the top.
inline std::array<SparklePoint, 8> sparkle_points(const SparkleStar& s) {
    std::array<SparklePoint, 8> p{};
    const float inner = s.r * 0.3f;
    for (int i = 0; i < 8; ++i) {
        const float a = -1.5707963f + i * 0.78539816f;
        const float rr = (i % 2 == 0) ? s.r : inner;
        p[i].x = s.cx + rr * std::cos(a);
        p[i].y = s.cy + rr * std::sin(a);
    }
    return p;
}

// Gold on the normal background, light gold on the highlight color.
inline COLORREF sparkle_color(bool highlighted) {
    return highlighted ? RGB(255, 226, 122) : RGB(245, 180, 0);
}

} // namespace cxxime

#endif // CXXIME_UI_SPARKLE_H_
