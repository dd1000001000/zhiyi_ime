// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Win32 native controls settings editor.

#include "editor_app.h"

#include <algorithm>
#include <cstring>

#include <commctrl.h>

#include <cxxime/control_client.h>
#include <cxxime/data_path.h>
#include <cxxime/pinyin_scheme.h>

#include "cxxime_resource_ids.h"
#include "editor_app_internal.h"
#include "lexicon_view_tabs.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

namespace cxxime {
namespace settings {
namespace {

EditorApp* g_app = nullptr;

const wchar_t* kPanelNames[] = {
    L"输入", L"界面", L"高级布局", L"快捷键", L"词库管理", L"特殊符号", L"备份与导入",
    L"故障排查", L"关于"
};
const int kPanelCount = 9;

UINT settings_navigate_message() {
    static const UINT message = RegisterWindowMessageW(cxxime::kSettingsNavigateMessage);
    return message;
}

int settings_panel_index(cxxime::SettingsPanel panel) {
    switch (panel) {
    case cxxime::SettingsPanel::kBackup:
        return 6;
    case cxxime::SettingsPanel::kDiagnostics:
        return 7;
    case cxxime::SettingsPanel::kAbout:
        return 8;
    default:
        return static_cast<int>(panel);
    }
}

} // namespace

int EditorApp::run(HINSTANCE hInst,
                   float dpiScale,
                   cxxime::SettingsPanel initialPanel) {
    g_dpi = dpiScale;
    EditorApp app;
    g_app = &app;
    app.initial_panel_ = initialPanel;

    INITCOMMONCONTROLSEX icc = {
        sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = reinterpret_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_CXXIME), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = reinterpret_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_CXXIME), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = cxxime::kSettingsWindowClass;
    RegisterClassExW(&wc);

    app.hwnd_ = CreateWindowExW(0, cxxime::kSettingsWindowClass, cxxime::kSettingsWindowTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(700), S(450),
                                nullptr, nullptr, hInst, &app);
    if (!app.hwnd_) return 1;
    ShowWindow(app.hwnd_, SW_SHOW);
    UpdateWindow(app.hwnd_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(app.hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}

void EditorApp::create_controls(HWND window) {
    init_layout();
    RECT client_rect;
    GetClientRect(window, &client_rect);

    int right_margin = S(16);
    int bottom_margin = S(12);
    int button_width = S(80);
    int button_height = S(26);
    int button_gap = S(10);
    int footer_height = S(kFontPt + 16);
    int footer_y = client_rect.bottom - bottom_margin - footer_height;
    int button_y = client_rect.bottom - bottom_margin - button_height;
    int panel_y = kPadY;
    int panel_height = button_y - S(8) - panel_y;
    int list_height = footer_y;
    int panel_x = kPadX;
    int panel_width = client_rect.right - kPadX - right_margin;
    int apply_x = client_rect.right - right_margin - button_width;
    int cancel_x = apply_x - button_gap - button_width;
    int save_x = cancel_x - button_gap - button_width;

    hFooterFont_ = CreateFontW(-S(kFontPt + 4), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    hFooter_ = CreateWindowExW(
        0, L"STATIC", L"CxxIME 输入法", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0,
        footer_y, kListW, footer_height, window, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hFooter_, WM_SETFONT, reinterpret_cast<WPARAM>(hFooterFont_), TRUE);

    hList_ = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, kListW, list_height, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
        GetModuleHandle(nullptr), nullptr);
    hListFont_ = CreateFontW(-S(kNavFontPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    SendMessageW(hList_, WM_SETFONT, reinterpret_cast<WPARAM>(hListFont_), TRUE);
    for (int i = 0; i < kPanelCount; ++i) {
        SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kPanelNames[i]));
    }
    SendMessageW(hList_, LB_SETCURSEL, 0, 0);
    InvalidateRect(hList_, nullptr, TRUE);

    for (int i = 0; i < kPanelCount; ++i) {
        hPanels_[i] = CreateWindowExW(
            WS_EX_CONTROLPARENT, L"STATIC", nullptr,
            WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, panel_x, panel_y, panel_width,
            panel_height, window, nullptr, GetModuleHandle(nullptr), nullptr);
    }

    create_input_panel(hPanels_[0]);
    create_candidate_panel(hPanels_[1]);
    create_advanced_layout_panel(hPanels_[2]);
    create_shortcuts_panel(hPanels_[3]);
    create_dictionary_panel(hPanels_[4], panel_width);
    create_symbols_panel(hPanels_[5], panel_width);
    create_backup_panel(hPanels_[6], panel_width);
    create_diagnostics_panel(hPanels_[7]);
    create_about_panel(hPanels_[8], panel_width);

    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    save_x, button_y, button_width, button_height, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(2001)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    cancel_x, button_y, button_width, button_height, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(2002)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    apply_x, button_y, button_width, button_height, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(2003)), nullptr, nullptr);
    for (int id : {2001, 2002, 2003}) {
        HWND button = GetDlgItem(window, id);
        if (button) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
        }
    }
}

void EditorApp::show_panel(int idx) {
    if (idx < 0 || idx >= kPanelCount) {
        return;
    }
    if (hList_ && SendMessageW(hList_, LB_GETCURSEL, 0, 0) != idx) {
        SendMessageW(hList_, LB_SETCURSEL, idx, 0);
    }
    for (int i = 0; i < kPanelCount; ++i)
        ShowWindow(hPanels_[i], (i == idx) ? SW_SHOW : SW_HIDE);
    panel_ = idx;

    if (idx != 1 && idx != 2 && candPreviewVisible_) {
        hide_candidate_preview_window();
    }
    if (idx == 1 || idx == 2)
        update_cand_preview();
    if (idx == 4)
        query_lexicon_entries(false);
    if (idx == 5) {
        load_symbol_categories();
    }
    update_candidate_preview_buttons();
    InvalidateRect(hList_, nullptr, TRUE);
}

void EditorApp::release_fonts() {
    if (hFooterFont_) {
        DeleteObject(hFooterFont_);
    }
    if (hListFont_) {
        DeleteObject(hListFont_);
    }
    if (hAboutTitleFont_) {
        DeleteObject(hAboutTitleFont_);
    }
    if (g_hFont) {
        DeleteObject(g_hFont);
    }
    hFooterFont_ = nullptr;
    hListFont_ = nullptr;
    hAboutTitleFont_ = nullptr;
    g_hFont = nullptr;
}

bool EditorApp::load_config() {
    config_ = {};
    // Load defaults from program directory, then overlay C:\Users\<user>\cxxime\default.json.
    const std::string default_path = cxxime::data_path("default.json");
    const std::string user_path = cxxime::user_data_path("default.json");
    const std::string themes_path = cxxime::data_path("themes.json");
    auto show_load_error = [this](const wchar_t* source, const std::string& path) {
        std::wstring message = source;
        message += L"加载失败。\n\n";
        message += path_for_display(path);
        MessageBoxW(hwnd_, message.c_str(), L"CxxIME 设置", MB_OK | MB_ICONERROR);
    };

    if (!config_.load(default_path)) {
        show_load_error(L"默认配置", default_path);
        return false;
    }

    const std::wstring wide_user_path = path_for_display(user_path);
    const DWORD user_attributes = GetFileAttributesW(wide_user_path.c_str());
    if (user_attributes != INVALID_FILE_ATTRIBUTES) {
        if ((user_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !config_.load_user(user_path)) {
            show_load_error(L"用户配置", user_path);
            return false;
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            show_load_error(L"用户配置", user_path);
            return false;
        }
    }

    if (!config_.load_themes(themes_path)) {
        show_load_error(L"主题配置", themes_path);
        return false;
    }

    // Populate controls
    SendMessageW(hThemeCombo_, CB_RESETCONTENT, 0, 0);
    themeIds_.clear();
    int selected_theme = -1;
    for (const auto& theme_id : config_.preset_color_scheme_order) {
        const auto& colors = config_.preset_color_schemes.at(theme_id);
        std::string label = colors.name.empty()
            ? theme_id : colors.name + " (" + theme_id + ")";
        combo_add(hThemeCombo_, utf8_to_wstr(label).c_str());
        themeIds_.push_back(theme_id);
        if (theme_id == config_.theme) {
            selected_theme = static_cast<int>(themeIds_.size()) - 1;
        }
    }
    combo_set_index(hThemeCombo_, selected_theme >= 0 ? selected_theme : 0);

    std::wstring wfn = utf8_to_wstr(config_.font_name);
    SetWindowTextW(hFontBtn_, wfn.c_str());

    set_edit_int(hFontSize_, config_.font_size);

    bool horiz = (config_.layout == "horizontal");
    SendMessageW(hLayoutH_, BM_SETCHECK, horiz ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hLayoutV_, BM_SETCHECK, horiz ? BST_UNCHECKED : BST_CHECKED, 0);

    bool d2d = (config_.render_backend == "d2d");
    SendMessageW(hRenderD2D_, BM_SETCHECK, d2d ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hRenderGDI_, BM_SETCHECK, d2d ? BST_UNCHECKED : BST_CHECKED, 0);

    set_edit_int(hCandEdits_[0], config_.layout_config.min_width);
    set_edit_int(hCandEdits_[1], config_.layout_config.max_width);
    set_edit_int(hCandEdits_[2], config_.layout_config.max_height);
    set_edit_int(hCandEdits_[3], config_.layout_config.margin_x);
    set_edit_int(hCandEdits_[4], config_.layout_config.margin_y);
    set_edit_int(hCandEdits_[5], config_.layout_config.spacing);
    set_edit_int(hCandEdits_[6], config_.layout_config.candidate_spacing);
    set_edit_int(hCandEdits_[7], config_.layout_config.hilite_padding_x);
    set_edit_int(hCandEdits_[8], config_.layout_config.hilite_padding_y);
    set_edit_int(hCandEdits_[9], config_.layout_config.hilite_spacing);
    set_edit_int(hCandEdits_[10], config_.layout_config.round_corner);
    set_edit_int(hCandEdits_[11], config_.layout_config.round_corner_ex);
    set_edit_int(hCandEdits_[12], config_.layout_config.border_width);
    set_edit_int(hCandEdits_[13], config_.layout_config.preedit_highlight_padding_x);
    set_edit_int(hCandEdits_[14], config_.layout_config.preedit_highlight_padding_y);
    set_edit_int(hCandEdits_[15], config_.layout_config.preedit_confirmed_gap);
    set_edit_int(hCandEdits_[16], config_.layout_config.preedit_boundary_gap);
    set_edit_int(hCandEdits_[17], config_.layout_config.preedit_highlight_corner);
    set_edit_int(hCandEdits_[18], config_.layout_config.preedit_highlight_border_width);
    sync_candidate_controls_from_edits();

    set_edit_int(hPageSize_, config_.page_size);
    set_edit_int(hLabelFontPt_, config_.layout_config.label_font_point);

    set_check(hInlinePreedit_, config_.inline_preedit);
    if (config_.preedit_type == "preview") {
        SendMessageW(hPreeditTypePreview_, BM_SETCHECK, BST_CHECKED, 0);
    } else {
        SendMessageW(hPreeditTypeComposition_, BM_SETCHECK, BST_CHECKED, 0);
    }
    set_check(hFuzzyPinyin_, config_.fuzzy_pinyin);
    set_check(hWubiAutoCommit_, config_.wubi_auto_commit);
    set_check(hWubiCommitFirstOnFifthKey_, config_.wubi_commit_first_on_fifth_key);
    set_check(hWubiRestartOnFifthAfterMiss_, config_.wubi_restart_on_fifth_after_miss);
    set_check(hWubiCodeHint_, config_.wubi_code_hint);
    set_check(hCandidateLearning_, config_.candidate_learning);
    set_check(hInitialEnglishPunct_, !config_.initial_chinese_punct);
    set_check(hInitialFullShape_, config_.initial_full_shape);
    update_preedit_type_enabled();
    set_check(hStatusWindow_, config_.status_window.enable);
    set_check(hStatusAutoDock_, config_.status_window.auto_dock);
    update_status_window_controls_enabled();
    load_diagnostics_controls();

    load_shortcut_controls();
    set_check(hInputModePinyin_, config_.input_mode == 0);
    set_check(hInputModeWubi_, config_.input_mode == 1);
    set_check(hInputModeMixed_, config_.input_mode == 2);
    const std::string pinyin_scheme = cxxime::normalize_pinyin_scheme_id(config_.pinyin_scheme);
    const auto scheme = std::find(pinyinSchemeIds_.begin(), pinyinSchemeIds_.end(), pinyin_scheme);
    combo_set_index(hPinyinScheme_, scheme == pinyinSchemeIds_.end()
                                         ? 0
                                         : static_cast<int>(scheme - pinyinSchemeIds_.begin()));
    update_pinyin_scheme_example();
    const int mixed_candidate_preference =
        config_.mixed_candidate_preference == cxxime::MixedCandidatePreference::kWubi ? 1 : 0;
    combo_set_index(hMixedCandidatePreference_, mixed_candidate_preference);
    update_input_mode_enabled();

    show_panel(settings_panel_index(initial_panel_));
    return true;
}

void EditorApp::readback(HWND) {
    auto& c = config_;
    c.inline_preedit = get_check(hInlinePreedit_);
    c.fuzzy_pinyin = get_check(hFuzzyPinyin_);
    c.wubi_auto_commit = get_check(hWubiAutoCommit_);
    c.wubi_commit_first_on_fifth_key = get_check(hWubiCommitFirstOnFifthKey_);
    c.wubi_restart_on_fifth_after_miss = get_check(hWubiRestartOnFifthAfterMiss_);
    c.wubi_code_hint = get_check(hWubiCodeHint_);
    c.candidate_learning = get_check(hCandidateLearning_);
    c.initial_chinese_punct = !get_check(hInitialEnglishPunct_);
    c.initial_full_shape = get_check(hInitialFullShape_);
    c.page_size = std::clamp(get_edit_int(hPageSize_), 1, 100);
    c.input_mode = get_check(hInputModeMixed_) ? 2 : get_check(hInputModeWubi_) ? 1 : 0;
    c.pinyin_scheme = selected_pinyin_scheme_id();
    c.mixed_candidate_preference = combo_index(hMixedCandidatePreference_) == 1
            ? cxxime::MixedCandidatePreference::kWubi
            : cxxime::MixedCandidatePreference::kAuto;
    c.preedit_type = (SendMessageW(hPreeditTypePreview_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            ? "preview" : "composition";

    c.font_size = std::clamp(get_edit_int(hFontSize_), 8, 72);
    c.layout = (SendMessageW(hLayoutH_, BM_GETCHECK, 0, 0) == BST_CHECKED) ? "horizontal" : "vertical";
    c.render_backend = (SendMessageW(hRenderD2D_, BM_GETCHECK, 0, 0) == BST_CHECKED) ? "d2d" : "gdi";
    c.status_window.enable = get_check(hStatusWindow_);
    c.status_window.auto_dock = get_check(hStatusAutoDock_);
    read_diagnostics_controls();
    c.layout_config = candidate_layout_from_edits();
}

bool EditorApp::save_config() {
    readback(hwnd_);
    config_.theme = selected_theme_id();
    if (!read_shortcut_controls()) {
        return false;
    }

    unsigned long error_code = ERROR_SUCCESS;
    if (!replace_user_config(config_.to_user_json(), nullptr, &error_code)) {
        const wchar_t* message = error_code == ERROR_HOTKEY_ALREADY_REGISTERED
            ? L"全局快捷键已被其他程序占用，配置未保存。"
            : L"后台服务未能保存并应用配置。";
        MessageBoxW(hwnd_, message, L"CxxIME 设置", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

LRESULT CALLBACK EditorApp::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditorApp* a = g_app;
    if (!a) return DefWindowProcW(hwnd, msg, wp, lp);

    const UINT navigate_message = settings_navigate_message();
    if (navigate_message != 0 && msg == navigate_message) {
        a->show_panel(static_cast<int>(wp));
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        a->hwnd_ = hwnd;
        a->create_controls(hwnd);
        if (!a->load_config()) {
            return -1;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        wchar_t txt[64]; GetWindowTextW((HWND)lp, txt, 64);
        if (wcscmp(txt, L"CxxIME 输入法") == 0) {
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        break;
    }
    case kDiagnosticsCompleteMessage:
        if (wp == 0) {
            MessageBoxW(hwnd,
                        L"诊断包导出完成。请检查桌面的 cxxime-diagnostics-*.zip。",
                        L"CxxIME", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd,
                        L"诊断导出已结束，但脚本返回失败。请查看打开的 PowerShell 窗口输出。",
                        L"CxxIME", MB_OK | MB_ICONERROR);
        }
        return 0;
    case kDiagnosticsCleanupCompleteMessage:
        a->handle_diagnostics_cleanup_complete(lp);
        return 0;
    case kLexiconQueryCompleteMessage:
        a->handle_lexicon_query_complete(wp, lp);
        return 0;
    case kLexiconCodeCompleteMessage:
        a->handle_lexicon_code_complete(wp, lp);
        return 0;
    case kLexiconImportCompleteMessage:
        a->handle_lexicon_import_complete(lp);
        return 0;
    case kLexiconBatchAddCompleteMessage:
        a->handle_lexicon_batch_add_complete(lp);
        return 0;
    case kUserBackupCompleteMessage:
        a->handle_backup_complete(lp);
        return 0;
    case WM_TIMER:
        if (wp == kLexiconCodeTimerId) {
            KillTimer(hwnd, kLexiconCodeTimerId);
            a->request_lexicon_code_suggestions();
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        float oldDpi = g_dpi;
        g_dpi = (float)LOWORD(wp) / 96.0f;
        float ratio = g_dpi / oldDpi;
        init_layout();
        HFONT oldFont = g_hFont;
        HFONT oldFooterFont = a->hFooterFont_;
        HFONT oldListFont = a->hListFont_;
        HFONT oldAboutTitleFont = a->hAboutTitleFont_;
        g_hFont = nullptr;
        HFONT newFont = get_font();
        a->hFooterFont_ = CreateFontW(-S(kFontPt + 4), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        a->hListFont_ = CreateFontW(-S(kNavFontPt), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        a->hAboutTitleFont_ = CreateFontW(-S(kFontPt + 2), 0, 0, 0, FW_BOLD, FALSE, FALSE,
                                          FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0,
                                          L"Microsoft YaHei UI");

        RECT* rc = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

        RECT cr; GetClientRect(hwnd, &cr);
        int marginR = S(16);
        int btnW = S(80), btnH = S(26), btnGap = S(10);
        int footerH = S(kFontPt + 16);
        int marginB = S(12);
        int footerY = cr.bottom - marginB - footerH;
        int btnY    = cr.bottom - marginB - btnH;
        int listH   = footerY;
        int panelX  = kPadX, panelY = kPadY;
        int panelW_ = cr.right - kPadX - marginR;
        int panelContH = btnY - S(8) - panelY;

        // Reposition ListBox (recalculated from scratch)
        SetWindowPos(a->hList_, nullptr, 0, 0, kListW, listH, SWP_NOZORDER);
        SendMessageW(a->hList_, WM_SETFONT, (WPARAM)a->hListFont_, TRUE);
        SendMessageW(a->hList_, LB_SETITEMHEIGHT, 0, (LPARAM)S(40));

        // Reposition panel containers
        for (int i = 0; i < kPanelCount; ++i) {
            if (!a->hPanels_[i]) continue;
            SetWindowPos(a->hPanels_[i], nullptr, panelX, panelY, panelW_, panelContH, SWP_NOZORDER);
            // Scale panel children by ratio (preserves relative layout)
            for (HWND c = GetWindow(a->hPanels_[i], GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                RECT cr2; GetWindowRect(c, &cr2);
                POINT cpt = {cr2.left, cr2.top};
                ScreenToClient(a->hPanels_[i], &cpt);
                SetWindowPos(c, nullptr,
                             (int)(cpt.x * ratio + 0.5f), (int)(cpt.y * ratio + 0.5f),
                             (int)((cr2.right - cr2.left) * ratio + 0.5f),
                             (int)((cr2.bottom - cr2.top) * ratio + 0.5f),
                             SWP_NOZORDER);
                SendMessageW(c, WM_SETFONT, (WPARAM)newFont, TRUE);
            }
        }
        if (a->hAboutTitle_) {
            SendMessageW(a->hAboutTitle_, WM_SETFONT, (WPARAM)a->hAboutTitleFont_, TRUE);
        }

        // Reposition footer
        SetWindowPos(a->hFooter_, nullptr, 0, footerY, kListW, footerH, SWP_NOZORDER);
        SendMessageW(a->hFooter_, WM_SETFONT, (WPARAM)a->hFooterFont_, TRUE);

        // Reposition buttons (recalculated from scratch)
        int appX_   = cr.right - marginR - btnW;
        int cancelX_ = appX_ - btnGap - btnW;
        int saveX_   = cancelX_ - btnGap - btnW;
        for (int id : {2001, 2002, 2003}) {
            HWND btn = GetDlgItem(hwnd, id);
            if (!btn) continue;
            int bx = (id == 2001) ? saveX_ : (id == 2002) ? cancelX_ : appX_;
            SetWindowPos(btn, nullptr, bx, btnY, btnW, btnH, SWP_NOZORDER);
            SendMessageW(btn, WM_SETFONT, (WPARAM)newFont, TRUE);
        }
        if (oldFont) {
            DeleteObject(oldFont);
        }
        if (oldFooterFont) {
            DeleteObject(oldFooterFont);
        }
        if (oldListFont) {
            DeleteObject(oldListFont);
        }
        if (oldAboutTitleFont) {
            DeleteObject(oldAboutTitleFont);
        }
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_DESTROY:
        a->lexiconImportToken_.reset();
        a->lexiconImportRunning_ = false;
        a->lexiconBatchAddToken_.reset();
        a->lexiconBatchAddRunning_ = false;
        a->backupToken_.reset();
        a->backupRunning_ = false;
        a->readback(hwnd);
        a->destroy_candidate_preview_window();
        a->release_fonts();
        PostQuitMessage(0);
        return 0;
    case WM_COMMAND: {
        int control_id = LOWORD(wp);
        int notification = HIWORD(wp);
        if (notification == LBN_SELCHANGE && control_id == 1) {
            int idx = (int)SendMessageW(a->hList_, LB_GETCURSEL, 0, 0);
            if (idx >= 0) a->show_panel(idx);
            return 0;
        }
        switch (control_id) {
        case 2001:
            if (a->save_config()) {
                DestroyWindow(hwnd);
            }
            return 0;
        case 2002:
            DestroyWindow(hwnd);
            return 0;
        case 2003:
            a->save_config();
            return 0;
        default:
            break;
        }
        if (a->handle_input_command(control_id, notification) ||
            a->handle_candidate_command(control_id, notification) ||
            a->handle_advanced_layout_command(control_id, notification) ||
            a->handle_shortcuts_command(control_id, notification) ||
            a->handle_dictionary_command(control_id, notification) ||
            a->handle_symbols_command(control_id, notification) ||
            a->handle_backup_command(control_id, notification) ||
            a->handle_diagnostics_command(control_id, notification)) {
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY:
        if (a->handle_dictionary_notify(lp) || a->handle_about_notify(lp)) {
            return 0;
        }
        break;
    case WM_MEASUREITEM: {
        if (wp == 1) {
            LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lp;
            mis->itemHeight = S(40);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        const HWND selected_lexicon_tab =
            a->lexiconResource_ == LexiconResource::kCandidatePreference
                ? a->lexiconViewTabs_.preferences
                : a->lexiconResource_ == LexiconResource::kManualCandidateOrder
                      ? a->lexiconViewTabs_.candidate_order
                      : a->lexiconViewTabs_.entries;
        if (draw_lexicon_view_tab(*dis, a->lexiconViewTabs_, selected_lexicon_tab)) {
            return TRUE;
        }
        if (dis->CtlID != 1) break;
        int idx = (int)dis->itemID;
        if (idx < 0 || idx >= kPanelCount) break;

        HDC dc = dis->hDC;
        RECT r = dis->rcItem;

        // Highlight rect with 4px margins, 6px corner radius
        RECT hr = {r.left + 4, r.top + 3, r.right - 4, r.bottom - 3};
        if (dis->itemState & ODS_SELECTED) {
            HBRUSH hBr = CreateSolidBrush(RGB(0, 122, 215));
            SelectObject(dc, GetStockObject(NULL_PEN));
            SelectObject(dc, hBr);
            RoundRect(dc, hr.left, hr.top, hr.right, hr.bottom, 6, 6);
            DeleteObject(hBr);
            SetTextColor(dc, RGB(255, 255, 255));
        } else {
            SetTextColor(dc, RGB(60, 60, 60));
        }
        SetBkMode(dc, TRANSPARENT);

        RECT tr = {hr.left + 12, r.top, hr.right - 4, r.bottom};
        DrawTextW(dc, kPanelNames[idx], -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        return TRUE;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace settings
} // namespace cxxime
