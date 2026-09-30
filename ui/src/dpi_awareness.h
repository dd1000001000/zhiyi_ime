// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#pragma once

#include <windows.h>

namespace cxxime {

constexpr float dpi_to_scale(UINT dpi) { return static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI; }

class ScopedDpiAwarenessContext {
public:
    explicit ScopedDpiAwarenessContext(DPI_AWARENESS_CONTEXT context)
        : previous_(SetThreadDpiAwarenessContext(context)) {}

    ~ScopedDpiAwarenessContext() {
        if (previous_) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

} // namespace cxxime
