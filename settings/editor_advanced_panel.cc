// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "editor_app.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <initializer_list>

#include <json.hpp>

#include <cxxime/data_path.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

void load_preset_int(const nlohmann::json& object, const char* key, int& value) {
    if (object.contains(key) && object[key].is_number()) {
        value = object[key].get<int>();
    }
}

void load_layout_config_from_json(const nlohmann::json& object, LayoutConfig& layout) {
    load_preset_int(object, "min_width", layout.min_width);
    load_preset_int(object, "max_width", layout.max_width);
    load_preset_int(object, "max_height", layout.max_height);
    load_preset_int(object, "margin_x", layout.margin_x);
    load_preset_int(object, "margin_y", layout.margin_y);
    load_preset_int(object, "spacing", layout.spacing);
    load_preset_int(object, "candidate_spacing", layout.candidate_spacing);
    load_preset_int(object, "hilite_spacing", layout.hilite_spacing);
    load_preset_int(object, "hilite_padding_x", layout.hilite_padding_x);
    load_preset_int(object, "hilite_padding_y", layout.hilite_padding_y);
    load_preset_int(object, "round_corner", layout.round_corner);
    load_preset_int(object, "round_corner_ex", layout.round_corner_ex);
    load_preset_int(object, "label_font_point", layout.label_font_point);
    load_preset_int(object, "border_width", layout.border_width);
    load_preset_int(object, "preedit_highlight_padding_x",
        layout.preedit_highlight_padding_x);
    load_preset_int(object, "preedit_highlight_padding_y",
        layout.preedit_highlight_padding_y);
    load_preset_int(object, "preedit_confirmed_gap", layout.preedit_confirmed_gap);
    load_preset_int(object, "preedit_boundary_gap", layout.preedit_boundary_gap);
    load_preset_int(object, "preedit_highlight_corner", layout.preedit_highlight_corner);
    load_preset_int(object, "preedit_highlight_border_width",
                     layout.preedit_highlight_border_width);
}

LayoutConfig built_in_candidate_layout_preset(const char* preset, bool vertical,
                                              const LayoutConfig& default_layout) {
    LayoutConfig layout = default_layout;
    if (std::strcmp(preset, "recommended") == 0) {
        layout = LayoutConfig{};
        layout.min_width = 160;
        layout.max_width = 0;
        layout.max_height = 0;
        if (vertical) {
            layout.margin_x = 12;
            layout.margin_y = 10;
            layout.spacing = 8;
            layout.candidate_spacing = 4;
        } else {
            layout.margin_x = 14;
            layout.margin_y = 12;
            layout.spacing = 10;
            layout.candidate_spacing = 13;
        }
        layout.hilite_spacing = 4;
        layout.hilite_padding_x = 6;
        layout.hilite_padding_y = vertical ? 2 : 3;
        layout.round_corner = 5;
        layout.round_corner_ex = 5;
        layout.border_width = 1;
        layout.label_font_point = 0;
        layout.preedit_highlight_padding_x = 5;
        layout.preedit_highlight_padding_y = vertical ? 2 : 3;
        layout.preedit_confirmed_gap = 7;
        layout.preedit_boundary_gap = 1;
        layout.preedit_highlight_corner = 4;
        layout.preedit_highlight_border_width = 1;
        return layout;
    }

    if (vertical) {
        layout.margin_x = 10;
        layout.margin_y = 8;
        layout.spacing = 6;
        layout.candidate_spacing = 2;
    }
    return layout;
}

LayoutConfig load_candidate_layout_preset(const char* preset, bool vertical,
                                          const LayoutConfig& default_layout) {
    LayoutConfig layout = built_in_candidate_layout_preset(preset, vertical, default_layout);

    std::ifstream file(data_path("settings_presets.json"));
    if (!file.is_open()) {
        return layout;
    }

    try {
        nlohmann::json json = nlohmann::json::parse(file);
        const char* direction = vertical ? "vertical" : "horizontal";
        if (!json.contains("candidate_window") || !json["candidate_window"].is_object()) {
            return layout;
        }
        auto& candidate_window = json["candidate_window"];
        if (!candidate_window.contains("layout_presets") ||
            !candidate_window["layout_presets"].is_object()) {
            return layout;
        }
        auto& presets = candidate_window["layout_presets"];
        if (!presets.contains(preset) || !presets[preset].is_object()) {
            return layout;
        }
        auto& preset_object = presets[preset];
        if (!preset_object.contains(direction) || !preset_object[direction].is_object()) {
            return layout;
        }

        load_layout_config_from_json(preset_object[direction], layout);
    } catch (const nlohmann::json::exception&) {
    }
    return layout;
}

} // namespace

void EditorApp::create_advanced_layout_panel(HWND panel) {
    const int top = kPanelPadTop;
    const wchar_t* names[] = {
        L"最小宽度:",   L"最大宽度:", L"最大高度:",     L"水平边距:",     L"垂直边距:",
        L"预编辑间距:", L"候选间距:", L"高亮横向留白:", L"高亮纵向留白:", L"高亮内部间距:",
        L"高亮圆角:",   L"窗口圆角:", L"边框宽度:",
        L"预编辑横向留白:", L"预编辑纵向留白:", L"确认前缀间距:", L"音节边界间距:",
        L"预编辑圆角:", L"预编辑边框:",
    };
    RECT panel_rect = {};
    GetClientRect(panel, &panel_rect);
    const int panel_width = static_cast<int>(panel_rect.right - panel_rect.left);
    const int column_width =
        (std::max)(S(150), (panel_width - kPanelPadLeft * 2) / 3);
    const int edit_width = S(44);
    const int column_right_padding = S(8);
    const int maximum_label_width =
        column_width - edit_width - S(8) - column_right_padding;
    const int label_widths[] = {
        (std::min)(S(88), maximum_label_width),
        (std::min)(S(96), maximum_label_width),
        maximum_label_width,
    };
    SetWindowSubclass(panel, PanelForwardProc, 2000, reinterpret_cast<DWORD_PTR>(hwnd_));

    for (int i = 0; i < 19; ++i) {
        int column = i < 13 ? i / 7 : 2;
        int row = i < 13 ? i % 7 : i - 13;
        int x = kPanelPadLeft + column * column_width;
        int y = top + row * kRowH;
        int control_x = make_aligned_label(names[i], x, label_widths[column], y, panel);
        const int control_id = i < 13 ? 1200 + i : 1230 + i - 13;
        hCandEdits_[i] = make_edit(control_id, control_x, y, edit_width, panel);
    }

    const int preset_y = top + kRowH * 7;
    const int control_x =
        make_aligned_label(L"布局方案:", kPanelPadLeft, S(90), preset_y, panel);
    hCandRecommendBtn_ = CreateWindowExW(
        0, L"BUTTON", L"应用推荐布局", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        control_x, preset_y, S(110), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1220)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(hCandRecommendBtn_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);

    const int preview_y = preset_y + kRowH;
    const int preview_x =
        make_aligned_label(L"候选预览:", kPanelPadLeft, S(90), preview_y, panel);
    hCandPreviewScenarios_[1] = make_combo(1236, preview_x, preview_y, S(125), panel);
    for (const wchar_t* scenario : {L"拼音整词", L"拼音分段", L"确认前缀", L"五笔编码",
                                    L"混输五笔"}) {
        combo_add(hCandPreviewScenarios_[1], scenario);
    }
    combo_set_index(hCandPreviewScenarios_[1], 1);
    hCandPreviewBtns_[1] = CreateWindowExW(
        0, L"BUTTON", L"打开预览", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        preview_x + S(133), preview_y, S(110), kCtrlH, panel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1222)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(hCandPreviewBtns_[1], WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
}

bool EditorApp::handle_advanced_layout_command(int control_id, int notification) {
    if (control_id == 1220 && notification == BN_CLICKED) {
        apply_candidate_control(control_id);
        return true;
    }
    if (control_id == 1236 && notification == CBN_SELCHANGE) {
        combo_set_index(hCandPreviewScenarios_[0], combo_index(hCandPreviewScenarios_[1]));
        update_cand_preview();
        return true;
    }
    if (notification == EN_CHANGE &&
        ((control_id >= 1200 && control_id <= 1212) ||
         (control_id >= 1230 && control_id <= 1235))) {
        if (!updatingCandControls_) {
            sync_candidate_controls_from_edits();
            update_cand_preview();
        }
        return true;
    }
    return false;
}

LayoutConfig EditorApp::candidate_layout_from_edits() const {
    LayoutConfig layout = config_.layout_config;
    layout.min_width = std::clamp(get_edit_int(hCandEdits_[0]), 0, 4096);
    layout.max_width = std::clamp(get_edit_int(hCandEdits_[1]), 0, 4096);
    layout.max_height = std::clamp(get_edit_int(hCandEdits_[2]), 0, 4096);
    layout.margin_x = std::clamp(get_edit_int(hCandEdits_[3]), 0, 256);
    layout.margin_y = std::clamp(get_edit_int(hCandEdits_[4]), 0, 256);
    layout.spacing = std::clamp(get_edit_int(hCandEdits_[5]), 0, 256);
    layout.candidate_spacing = std::clamp(get_edit_int(hCandEdits_[6]), 0, 256);
    layout.hilite_padding_x = std::clamp(get_edit_int(hCandEdits_[7]), 0, 256);
    layout.hilite_padding_y = std::clamp(get_edit_int(hCandEdits_[8]), 0, 256);
    layout.hilite_spacing = std::clamp(get_edit_int(hCandEdits_[9]), 0, 256);
    layout.round_corner = std::clamp(get_edit_int(hCandEdits_[10]), 0, 256);
    layout.round_corner_ex = std::clamp(get_edit_int(hCandEdits_[11]), 0, 256);
    layout.border_width = std::clamp(get_edit_int(hCandEdits_[12]), 0, 32);
    layout.preedit_highlight_padding_x =
        std::clamp(get_edit_int(hCandEdits_[13]), 0, 32);
    layout.preedit_highlight_padding_y =
        std::clamp(get_edit_int(hCandEdits_[14]), 0, 32);
    layout.preedit_confirmed_gap = std::clamp(get_edit_int(hCandEdits_[15]), 0, 32);
    layout.preedit_boundary_gap = std::clamp(get_edit_int(hCandEdits_[16]), 0, 32);
    layout.preedit_highlight_corner = std::clamp(get_edit_int(hCandEdits_[17]), 0, 32);
    layout.preedit_highlight_border_width =
        std::clamp(get_edit_int(hCandEdits_[18]), 0, 8);
    layout.label_font_point = std::clamp(get_edit_int(hLabelFontPt_), 0, 72);
    return layout;
}

void EditorApp::apply_candidate_layout_to_edits(const LayoutConfig& layout) {
    updatingCandControls_ = true;
    set_edit_int(hCandEdits_[0], layout.min_width);
    set_edit_int(hCandEdits_[1], layout.max_width);
    set_edit_int(hCandEdits_[2], layout.max_height);
    set_edit_int(hCandEdits_[3], layout.margin_x);
    set_edit_int(hCandEdits_[4], layout.margin_y);
    set_edit_int(hCandEdits_[5], layout.spacing);
    set_edit_int(hCandEdits_[6], layout.candidate_spacing);
    set_edit_int(hCandEdits_[7], layout.hilite_padding_x);
    set_edit_int(hCandEdits_[8], layout.hilite_padding_y);
    set_edit_int(hCandEdits_[9], layout.hilite_spacing);
    set_edit_int(hCandEdits_[10], layout.round_corner);
    set_edit_int(hCandEdits_[11], layout.round_corner_ex);
    set_edit_int(hCandEdits_[12], layout.border_width);
    set_edit_int(hCandEdits_[13], layout.preedit_highlight_padding_x);
    set_edit_int(hCandEdits_[14], layout.preedit_highlight_padding_y);
    set_edit_int(hCandEdits_[15], layout.preedit_confirmed_gap);
    set_edit_int(hCandEdits_[16], layout.preedit_boundary_gap);
    set_edit_int(hCandEdits_[17], layout.preedit_highlight_corner);
    set_edit_int(hCandEdits_[18], layout.preedit_highlight_border_width);
    set_edit_int(hLabelFontPt_, layout.label_font_point);
    updatingCandControls_ = false;
    sync_candidate_controls_from_edits();
    update_cand_preview();
}

void EditorApp::apply_default_candidate_settings() {
    Config defaults;
    const std::string default_path = data_path("default.json");
    const std::string themes_path = data_path("themes.json");
    if (!defaults.load(default_path) || !defaults.load_themes(themes_path)) {
        MessageBoxW(hwnd_, L"无法重新加载默认配置或主题配置。", L"CxxIME 设置",
                    MB_OK | MB_ICONERROR);
        return;
    }

    config_.font_name = defaults.font_name;
    config_.font_size = defaults.font_size;
    config_.theme = defaults.theme;
    const auto theme = std::find(themeIds_.begin(), themeIds_.end(), defaults.theme);
    if (theme != themeIds_.end()) {
        combo_set_index(hThemeCombo_, static_cast<int>(theme - themeIds_.begin()));
    }

    std::wstring font_name = utf8_to_wstr(defaults.font_name);
    SetWindowTextW(hFontBtn_, font_name.c_str());
    set_edit_int(hFontSize_, defaults.font_size);

    bool horizontal = defaults.layout == "horizontal";
    SendMessageW(hLayoutH_, BM_SETCHECK, horizontal ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hLayoutV_, BM_SETCHECK, horizontal ? BST_UNCHECKED : BST_CHECKED, 0);

    bool d2d = defaults.render_backend == "d2d";
    SendMessageW(hRenderD2D_, BM_SETCHECK, d2d ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hRenderGDI_, BM_SETCHECK, d2d ? BST_UNCHECKED : BST_CHECKED, 0);
    set_check(hStatusWindow_, defaults.status_window.enable);

    apply_candidate_layout_to_edits(defaults.layout_config);
}

void EditorApp::sync_candidate_controls_from_edits() {
    updatingCandControls_ = true;
    LayoutConfig layout = candidate_layout_from_edits();

    bool vertical = SendMessageW(hLayoutV_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int density = 1;
    if (vertical) {
        if (layout.margin_x <= 8 && layout.margin_y <= 7 && layout.candidate_spacing <= 1 &&
            layout.preedit_confirmed_gap <= 4) {
            density = 0;
        } else if (layout.margin_x >= 14 || layout.margin_y >= 11 ||
                   layout.candidate_spacing >= 5 || layout.preedit_confirmed_gap >= 8) {
            density = 2;
        }
    } else {
        if (layout.margin_x <= 9 && layout.margin_y <= 9 && layout.candidate_spacing <= 8 &&
            layout.preedit_confirmed_gap <= 4) {
            density = 0;
        } else if (layout.margin_x >= 15 || layout.margin_y >= 15 ||
                   layout.candidate_spacing >= 14 || layout.preedit_confirmed_gap >= 8) {
            density = 2;
        }
    }
    combo_set_index(hCandDensity_, density);

    int highlight = 1;
    if (layout.hilite_padding_x <= 3 && layout.hilite_padding_y <= 1 &&
        layout.preedit_highlight_padding_x <= 3 &&
        layout.preedit_highlight_padding_y <= 1) {
        highlight = 0;
    } else if (layout.hilite_padding_x >= 7 || layout.hilite_padding_y >= 4 ||
               layout.preedit_highlight_padding_x >= 6 ||
               layout.preedit_highlight_padding_y >= 3) {
        highlight = 2;
    }
    combo_set_index(hCandHighlight_, highlight);

    int corner = 1;
    if (layout.round_corner <= 0 && layout.round_corner_ex <= 0 &&
        layout.preedit_highlight_corner <= 0) {
        corner = 0;
    } else if (layout.round_corner >= 8 || layout.round_corner_ex >= 8 ||
               layout.preedit_highlight_corner >= 6) {
        corner = 2;
    }
    combo_set_index(hCandCorner_, corner);

    int border = layout.border_width <= 0 ? 0 : (layout.border_width >= 2 ? 2 : 1);
    combo_set_index(hCandBorder_, border);
    combo_set_index(hCandWidth_, layout.max_width > 0 ? 1 : 0);
    updatingCandControls_ = false;
}

void EditorApp::apply_candidate_control(int control_id) {
    if (updatingCandControls_) {
        return;
    }

    bool vertical = SendMessageW(hLayoutV_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    LayoutConfig layout = candidate_layout_from_edits();
    switch (control_id) {
    case 1214:
        if (vertical) {
            switch (combo_index(hCandDensity_)) {
            case 0:
                layout.margin_x = 8;
                layout.margin_y = 7;
                layout.spacing = 5;
                layout.candidate_spacing = 1;
                layout.preedit_confirmed_gap = 4;
                layout.preedit_boundary_gap = 1;
                break;
            case 2:
                layout.margin_x = 14;
                layout.margin_y = 11;
                layout.spacing = 8;
                layout.candidate_spacing = 5;
                layout.preedit_confirmed_gap = 8;
                layout.preedit_boundary_gap = 2;
                break;
            default:
                layout.margin_x = 10;
                layout.margin_y = 8;
                layout.spacing = 6;
                layout.candidate_spacing = 2;
                layout.preedit_confirmed_gap = 6;
                layout.preedit_boundary_gap = 1;
                break;
            }
        } else {
            switch (combo_index(hCandDensity_)) {
            case 0:
                layout.margin_x = 8;
                layout.margin_y = 8;
                layout.spacing = 6;
                layout.candidate_spacing = 7;
                layout.preedit_confirmed_gap = 4;
                layout.preedit_boundary_gap = 1;
                break;
            case 2:
                layout.margin_x = 16;
                layout.margin_y = 14;
                layout.spacing = 14;
                layout.candidate_spacing = 16;
                layout.preedit_confirmed_gap = 8;
                layout.preedit_boundary_gap = 2;
                break;
            default:
                layout.margin_x = 12;
                layout.margin_y = 12;
                layout.spacing = 10;
                layout.candidate_spacing = 11;
                layout.preedit_confirmed_gap = 6;
                layout.preedit_boundary_gap = 1;
                break;
            }
        }
        break;
    case 1215:
        switch (combo_index(hCandHighlight_)) {
        case 0:
            layout.hilite_padding_x = 3;
            layout.hilite_padding_y = 1;
            layout.hilite_spacing = 3;
            layout.preedit_highlight_padding_x = 3;
            layout.preedit_highlight_padding_y = 1;
            break;
        case 2:
            layout.hilite_padding_x = 8;
            layout.hilite_padding_y = 4;
            layout.hilite_spacing = 6;
            layout.preedit_highlight_padding_x = 6;
            layout.preedit_highlight_padding_y = 3;
            break;
        default:
            layout.hilite_padding_x = 5;
            layout.hilite_padding_y = 2;
            layout.hilite_spacing = 4;
            layout.preedit_highlight_padding_x = 4;
            layout.preedit_highlight_padding_y = 2;
            break;
        }
        break;
    case 1216:
        switch (combo_index(hCandCorner_)) {
        case 0:
            layout.round_corner = 0;
            layout.round_corner_ex = 0;
            layout.preedit_highlight_corner = 0;
            break;
        case 2:
            layout.round_corner = 10;
            layout.round_corner_ex = 10;
            layout.preedit_highlight_corner = 6;
            break;
        default:
            layout.round_corner = 4;
            layout.round_corner_ex = 4;
            layout.preedit_highlight_corner = 3;
            break;
        }
        break;
    case 1217:
        layout.border_width =
            combo_index(hCandBorder_) == 0 ? 0 : (combo_index(hCandBorder_) == 2 ? 2 : 1);
        break;
    case 1218:
        layout.max_width = combo_index(hCandWidth_) == 1 ? 420 : 0;
        break;
    case 1220:
        layout = load_candidate_layout_preset("recommended", vertical, config_.layout_config);
        break;
    case 1221:
        apply_default_candidate_settings();
        return;
    default:
        return;
    }

    apply_candidate_layout_to_edits(layout);
}

} // namespace settings
} // namespace cxxime
