// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The Advanced page: U mode and V mode (docs/uv-mode.md, issue #41), as in Microsoft Pinyin's
// advanced settings. Saved with Apply.

#include "editor_app.h"

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kUModeId = 6600;
constexpr int kVModeId = 6601;

}  // namespace

void EditorApp::create_advanced_panel(HWND panel) {
    RECT client = {};
    GetClientRect(panel, &client);
    const int x0 = kPanelPadLeft;
    const int width = client.right - x0 * 2;
    const int hint_height = S(2 * (kFontPt + 6));

    int y = card_begin(panel, kPanelPadTop, tr("advanced.card"));
    hUMode_ = make_check(kUModeId, tr("advanced.u_mode"), x0, y, width, panel);
    y += kRowH;
    make_hint(tr("advanced.u_mode_hint"), x0, y, width, panel, 2);
    y += hint_height + S(8);
    hVMode_ = make_check(kVModeId, tr("advanced.v_mode"), x0, y, width, panel);
    y += kRowH;
    make_hint(tr("advanced.v_mode_hint"), x0, y, width, panel, 2);
    y += hint_height;
    card_end(panel, y);
    populate_advanced();
}

void EditorApp::populate_advanced() {
    if (!hUMode_) return;
    set_check(hUMode_, config_.advanced.u_mode);
    set_check(hVMode_, config_.advanced.v_mode);
}

void EditorApp::read_advanced() {
    if (!hUMode_) return;
    config_.advanced.u_mode = get_check(hUMode_);
    config_.advanced.v_mode = get_check(hVMode_);
}

}  // namespace settings
}  // namespace cxxime
