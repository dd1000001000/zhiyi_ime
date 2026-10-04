// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Laya recommendation look: a four-point star with curved sides at the top-right corner of the
// recommended candidate (CandidateRect::mark_rect), in a blue-to-purple gradient, and a
// blue-to-purple wash over the candidate (the highlight itself when it is highlighted). For
// kSparkleDurationMs after the recommendation appears the star pops in with a turn and twinkles
// once, then stays still.
#ifndef CXXIME_UI_SPARKLE_H_
#define CXXIME_UI_SPARKLE_H_

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <cxxime/render_context.h>

namespace cxxime {

constexpr unsigned kSparkleDurationMs = 900;
constexpr unsigned kSparkleFrameMs = 30;

// Gradient ends: blue (bottom-left) to purple (top-right).
constexpr Color kRecommendBlue{66, 133, 255, 255};
constexpr Color kRecommendPurple{156, 104, 255, 255};
// The highlight of a recommended candidate: a little deeper, for white text.
constexpr Color kRecommendHighlightBlue{52, 112, 240, 255};
constexpr Color kRecommendHighlightPurple{128, 82, 236, 255};

// The wash over a recommended candidate that is not highlighted (0..1 opacity).
inline float recommend_wash_opacity(const Color& background) {
    const float luma = 0.2126f * background.r + 0.7152f * background.g + 0.0722f * background.b;
    return luma < 128.0f ? 0.26f : 0.13f;
}

struct SparkleStar {
    float cx = 0, cy = 0, r = 0;  // center and tip radius (pixels)
    float angle = 0;              // rotation (radians)
};

// t_seconds < 0: steady state.
inline SparkleStar sparkle_star(const RECT& mark, float t_seconds) {
    const float w = static_cast<float>(mark.right - mark.left);
    const float h = static_cast<float>(mark.bottom - mark.top);
    const float r = (std::min)(w * 0.56f, h * 0.27f);
    SparkleStar star;
    star.cx = mark.left + w * 0.5f;
    star.cy = mark.top + r + h * 0.04f;
    float scale = 1.0f;
    if (t_seconds >= 0) {
        // Pop in (ease-out-back) while turning a quarter, then one soft twinkle.
        constexpr float kPop = 0.42f;
        const float p = (std::min)(t_seconds / kPop, 1.0f) - 1.0f;
        scale = 0.25f + 0.75f * (1.0f + 2.70158f * p * p * p + 1.70158f * p * p);
        star.angle = p * 1.5707963f * 0.5f;
        if (t_seconds > kPop) {
            const float q = (std::min)((t_seconds - kPop) / (kSparkleDurationMs / 1000.0f - kPop),
                                       1.0f);
            scale *= 1.0f + 0.12f * std::sin(3.14159265f * q);
        }
    }
    star.r = r * scale;
    return star;
}

struct SparklePoint {
    float x = 0, y = 0;
};

// The outline as 4 cubic Bézier segments: start at tip 0, then (control 1, control 2, next tip)
// for each segment. The sides curve in toward the center.
inline std::array<SparklePoint, 13> sparkle_curve(const SparkleStar& s) {
    std::array<SparklePoint, 13> p{};
    auto at = [&](float along_tip, int tip, float along_next, int next) {
        const float a = s.angle - 1.5707963f + tip * 1.5707963f;
        const float b = s.angle - 1.5707963f + next * 1.5707963f;
        return SparklePoint{s.cx + s.r * (along_tip * std::cos(a) + along_next * std::cos(b)),
                            s.cy + s.r * (along_tip * std::sin(a) + along_next * std::sin(b))};
    };
    p[0] = at(1, 0, 0, 1);
    for (int k = 0; k < 4; ++k) {
        const int next = (k + 1) % 4;
        p[1 + k * 3] = at(0.22f, k, 0.07f, next);
        p[2 + k * 3] = at(0.07f, k, 0.22f, next);
        p[3 + k * 3] = at(1, next, 0, k);
    }
    return p;
}

// The curve flattened to a polygon (GDI).
inline std::array<POINT, 4 * 8> sparkle_polygon(const SparkleStar& s) {
    const auto c = sparkle_curve(s);
    std::array<POINT, 4 * 8> out{};
    for (int k = 0; k < 4; ++k) {
        const SparklePoint& p0 = c[k * 3];
        const SparklePoint& p1 = c[k * 3 + 1];
        const SparklePoint& p2 = c[k * 3 + 2];
        const SparklePoint& p3 = c[k * 3 + 3];
        for (int i = 0; i < 8; ++i) {
            const float t = i / 8.0f, u = 1 - t;
            const float x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x +
                            t * t * t * p3.x;
            const float y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y +
                            t * t * t * p3.y;
            out[k * 8 + i] = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
        }
    }
    return out;
}

} // namespace cxxime

#endif // CXXIME_UI_SPARKLE_H_
