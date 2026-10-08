// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Win32 native controls settings editor.
//
// Modified by Zhiyi IME Contributors: four pages, translated strings, language switching.

#include "editor_app.h"

#include <algorithm>

#include <cxxime/control_client.h>
#include <cxxime/server_launcher.h>
#include <cxxime/data_path.h>

#include "cxxime_resource_ids.h"
#include "editor_app_internal.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

namespace cxxime {
namespace settings {
namespace {

EditorApp* g_app = nullptr;

constexpr int kListId = 1;
constexpr int kOkId = 2001;
constexpr int kCancelId = 2002;
constexpr int kApplyId = 2003;

const char* const kPanelKeys[] = {"nav.general",  "nav.fuzzy",   "nav.keys",
                                  "nav.dictionary", "nav.learning", "nav.privacy",
                                  "nav.update",   "nav.about"};
// Segoe Fluent Icons / MDL2 Assets glyphs for the pages above.
const wchar_t kPanelIcons[] = {0xE713, 0xE8D2, 0xE765, 0xE82D, 0xE7BE, 0xE72E, 0xE895, 0xE946};

UINT settings_navigate_message() {
    static const UINT message = RegisterWindowMessageW(cxxime::kSettingsNavigateMessage);
    return message;
}

int settings_panel_index(cxxime::SettingsPanel panel) {
    switch (panel) {
    case cxxime::SettingsPanel::kShortcuts:
        return 2;
    case cxxime::SettingsPanel::kDictionary:
        return 3;
    case cxxime::SettingsPanel::kDiagnostics:
        return 5;
    case cxxime::SettingsPanel::kUpdate:
        return kUpdatePanel;
    case cxxime::SettingsPanel::kAbout:
        return 7;
    default:
        return 0;
    }
}

HFONT make_ui_font(int point, int weight) {
    return CreateFontW(-S(point), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0,
                       L"Microsoft YaHei UI");
}

} // namespace

int EditorApp::run(HINSTANCE hInst, float dpiScale, cxxime::SettingsPanel initialPanel) {
    g_dpi = dpiScale;
    EditorApp app;
    g_app = &app;
    app.initial_panel_ = initialPanel;

    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_PROGRESS_CLASS};
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
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                    WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(800), S(650), nullptr, nullptr,
                                hInst, &app);
    if (!app.hwnd_) return 1;
    ShowWindow(app.hwnd_, SW_SHOW);
    UpdateWindow(app.hwnd_);
    // Started by the server for a menu click: the IME passed it the right to come forward
    // (text_service_activation.cpp); without it Windows keeps the window behind the app.
    SetForegroundWindow(app.hwnd_);

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
    SetWindowTextW(window, tr("window.title"));
    RECT client_rect;
    GetClientRect(window, &client_rect);

    const int right_margin = S(16);
    const int bottom_margin = S(12);
    const int button_width = S(88);
    const int button_height = S(30);
    const int button_gap = S(10);
    const int footer_height = S(kFontPt + 16);
    const int footer_y = client_rect.bottom - bottom_margin - footer_height;
    const int button_y = client_rect.bottom - bottom_margin - button_height;
    const int panel_y = kPadY;
    const int panel_height = button_y - S(8) - panel_y;
    const int panel_x = kPadX;
    const int panel_width = client_rect.right - kPadX - right_margin;
    const int apply_x = client_rect.right - right_margin - button_width;
    const int cancel_x = apply_x - button_gap - button_width;
    const int ok_x = cancel_x - button_gap - button_width;

    hFooterFont_ = make_ui_font(kFontPt + 4, FW_BOLD);
    hFooter_ = CreateWindowExW(0, L"STATIC", tr("app.name"),
                               WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, footer_y,
                               kListW, footer_height, window, nullptr, GetModuleHandle(nullptr),
                               nullptr);
    SendMessageW(hFooter_, WM_SETFONT, reinterpret_cast<WPARAM>(hFooterFont_), TRUE);

    hList_ = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                 LBS_NOINTEGRALHEIGHT,
                             S(8), kPadY, kListW - S(8), footer_y - kPadY, window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                             GetModuleHandle(nullptr), nullptr);
    hListFont_ = make_ui_font(kNavFontPt, FW_NORMAL);
    SendMessageW(hList_, WM_SETFONT, reinterpret_cast<WPARAM>(hListFont_), TRUE);
    for (const char* key : kPanelKeys) {
        SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(key)));
    }

    for (int i = 0; i < kPanelCount; ++i) {
        hPanels_[i] = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", nullptr,
                                      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, panel_x,
                                      panel_y, panel_width, panel_height, window, nullptr,
                                      GetModuleHandle(nullptr), nullptr);
        // Panels are STATIC windows: forward their children's notifications to the main window.
        SetWindowSubclass(hPanels_[i], PanelForwardProc, 3000 + i,
                          reinterpret_cast<DWORD_PTR>(window));
    }
    make_panel_scrollable(hPanels_[0]);
    create_general_panel(hPanels_[0]);
    update_panel_scroll(hPanels_[0]);
    create_fuzzy_panel(hPanels_[1]);
    create_keys_panel(hPanels_[2]);
    create_dictionary_panel(hPanels_[3]);
    make_panel_scrollable(hPanels_[kLearningPanel]);
    create_learning_panel(hPanels_[kLearningPanel]);
    update_panel_scroll(hPanels_[kLearningPanel]);
    create_privacy_panel(hPanels_[5]);
    create_update_panel(hPanels_[kUpdatePanel]);
    create_about_panel(hPanels_[7], panel_width);

    const struct {
        int id;
        const char* key;
        int x;
    } buttons[] = {
        {kOkId, "button.ok", ok_x},
        {kCancelId, "button.cancel", cancel_x},
        {kApplyId, "button.apply", apply_x},
    };
    for (const auto& button : buttons) {
        HWND control = make_page_button(button.id, tr(button.key), button.x, button_y,
                                        button_width, button_height, window);
        if (button.id == kOkId) set_button_primary(control, true);
    }
}

void EditorApp::destroy_controls() {
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child;) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }
    hList_ = hFooter_ = hAboutTitle_ = nullptr;
    hUpdateStatus_ = nullptr;  // show_update_state() waits for the new page
    hChineseTarget_ = hEnglishTarget_ = hGlossPreview_ = hPackList_ = hCheckAllPacks_ = nullptr;
    hPackTabs_[0] = hPackTabs_[1] = nullptr;
    hPackActions_.clear();
    hPackRemoves_.clear();
    hints_.clear();
    clear_cards();
    for (HWND& panel : hPanels_) {
        panel = nullptr;
    }
    release_fonts();
}

void EditorApp::rebuild_ui() {
    const int panel = panel_;
    SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    destroy_controls();
    create_controls(hwnd_);
    populate_controls();
    apply_ui_theme(ui_colors().dark, true);  // the new controls
    show_panel(panel);
    SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void EditorApp::show_panel(int idx) {
    if (idx < 0 || idx >= kPanelCount) {
        return;
    }
    if (hList_ && SendMessageW(hList_, LB_GETCURSEL, 0, 0) != idx) {
        SendMessageW(hList_, LB_SETCURSEL, idx, 0);
    }
    for (int i = 0; i < kPanelCount; ++i) {
        ShowWindow(hPanels_[i], i == idx ? SW_SHOW : SW_HIDE);
    }
    panel_ = idx;
    InvalidateRect(hList_, nullptr, TRUE);
}

void EditorApp::release_fonts() {
    for (HFONT* font : {&hFooterFont_, &hListFont_, &hAboutTitleFont_, &hHintFont_}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    release_shared_fonts();
}

bool EditorApp::load_config() {
    Config config;
    if (!read_config_files(config, true)) {
        return false;
    }
    config_ = config;
    loaded_config_ = std::move(config);
    return true;
}

void EditorApp::refresh_config() {
    Config disk;
    if (!read_config_files(disk, false)) {
        return;
    }
    read_controls(false);
    // Start from the files; a value changed on a page since the last load stays.
    Config merged = disk;
    const Config& ui = config_;
    const Config& before = loaded_config_;
#define KEEP_PAGE_EDIT(member) \
    if (!(ui.member == before.member)) merged.member = ui.member
    KEEP_PAGE_EDIT(input_mode);
    KEEP_PAGE_EDIT(pinyin_initials);
    KEEP_PAGE_EDIT(theme);
    KEEP_PAGE_EDIT(font_size);
    KEEP_PAGE_EDIT(layout);
    KEEP_PAGE_EDIT(chinese_gloss_target);
    KEEP_PAGE_EDIT(english_gloss_target);
    KEEP_PAGE_EDIT(mt_enable);
    KEEP_PAGE_EDIT(mt_device);
    KEEP_PAGE_EDIT(page_size);
    KEEP_PAGE_EDIT(ui_language);
    KEEP_PAGE_EDIT(english.correction);
    KEEP_PAGE_EDIT(candidate_learning);
    KEEP_PAGE_EDIT(experience_program);
    KEEP_PAGE_EDIT(collect_input);
    KEEP_PAGE_EDIT(update_notify);
    KEEP_PAGE_EDIT(autostart);
    KEEP_PAGE_EDIT(laya.enable);
    KEEP_PAGE_EDIT(laya.device);
    KEEP_PAGE_EDIT(fuzzy_pinyin);
    KEEP_PAGE_EDIT(fuzzy_groups);
#undef KEEP_PAGE_EDIT
    // The switch keys move together, so a merge never repeats a key.
    if (!(ui.ascii_switch_key == before.ascii_switch_key) ||
        ui.ascii_toggle_shortcut != before.ascii_toggle_shortcut ||
        ui.english_style_shortcut != before.english_style_shortcut ||
        ui.punct_toggle_shortcut != before.punct_toggle_shortcut ||
        ui.shape_toggle_shortcut != before.shape_toggle_shortcut) {
        merged.ascii_switch_key = ui.ascii_switch_key;
        merged.ascii_toggle_shortcut = ui.ascii_toggle_shortcut;
        merged.english_style_shortcut = ui.english_style_shortcut;
        merged.punct_toggle_shortcut = ui.punct_toggle_shortcut;
        merged.shape_toggle_shortcut = ui.shape_toggle_shortcut;
    }
    config_ = std::move(merged);
    loaded_config_ = std::move(disk);
    populate_controls();
}

bool EditorApp::read_config_files(Config& config, bool report_errors) {
    config = {};
    // Defaults from the program directory, then %USERPROFILE%\zhiyi\default.json.
    const std::string default_path = cxxime::data_path("default.json");
    const std::string user_path = cxxime::user_data_path("default.json");
    const std::string themes_path = cxxime::data_path("themes.json");
    auto show_load_error = [this, report_errors](const std::string& path) {
        if (!report_errors) return;
        std::wstring message = tr("error.load");
        message += L"\n\n";
        message += path_for_display(path);
        MessageBoxW(hwnd_, message.c_str(), tr("window.title"), MB_OK | MB_ICONERROR);
    };

    if (!config.load(default_path)) {
        show_load_error(default_path);
        return false;
    }
    const std::wstring wide_user_path = path_for_display(user_path);
    const DWORD user_attributes = GetFileAttributesW(wide_user_path.c_str());
    if (user_attributes != INVALID_FILE_ATTRIBUTES) {
        if ((user_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !config.load_user(user_path)) {
            show_load_error(user_path);
            return false;
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            show_load_error(user_path);
            return false;
        }
    }
    if (!config.load_themes(themes_path)) {
        show_load_error(themes_path);
        return false;
    }
    return true;
}

bool EditorApp::save_config() {
    if (!read_controls()) {
        return false;
    }
    refresh_config();  // keep what changed elsewhere (e.g. the taskbar menu) since the last load
    if (!ensure_server_running()) {  // it saves the settings
        return false;
    }
    unsigned long error_code = ERROR_SUCCESS;
    if (!replace_user_config(config_.to_user_json(), nullptr, &error_code)) {
        MessageBoxW(hwnd_,
                    error_code == ERROR_HOTKEY_ALREADY_REGISTERED ? tr("error.hotkey_taken")
                                                                  : tr("error.save"),
                    tr("window.title"), MB_OK | MB_ICONERROR);
        return false;
    }
    loaded_config_ = config_;
    return true;
}

LRESULT CALLBACK EditorApp::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditorApp* a = g_app;
    if (!a) return DefWindowProcW(hwnd, msg, wp, lp);

    const UINT navigate_message = settings_navigate_message();
    if (a->handle_update_message(msg, wp, lp) || a->handle_learning_message(msg, wp, lp) ||
        a->handle_gpu_message(msg, wp, lp) || a->handle_data_message(msg, wp, lp) ||
        a->handle_translator_message(msg, wp, lp)) {
        return 0;
    }
    if (navigate_message != 0 && msg == navigate_message) {  // opened again from the IME
        a->refresh_config();
        a->show_panel(settings_panel_index(static_cast<cxxime::SettingsPanel>(wp)));
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_MOUSEWHEEL:
        // With the keyboard focus here (or on the page list) the wheel comes to this window:
        // scroll the page that is shown. Only a scrolling page (WS_VSCROLL): any other passes
        // the wheel back up to this window.
        for (HWND panel : a->hPanels_) {
            if (panel && IsWindowVisible(panel) && (GetWindowLongPtrW(panel, GWL_STYLE) & WS_VSCROLL)) {
                return SendMessageW(panel, msg, wp, lp);
            }
        }
        return 0;
    case WM_CREATE:
        a->hwnd_ = hwnd;
        // Strings are needed before any control exists; load_config() reports errors with them.
        load_ui_strings(resolve_ui_language(kAutoUiLanguage));
        if (!a->load_config()) {
            return -1;
        }
        a->ui_language_ = resolve_ui_language(a->config_.ui_language);
        load_ui_strings(a->ui_language_);
        // Saving goes through zhiyi-server; it is not running when startup.autostart is off or
        // after Exit in the taskbar menu, so start it while the page is read.
        start_server_on_demand();
        a->init_update();  // may open the Updates page
        a->init_learning();
        a->create_controls(hwnd);
        a->populate_controls();
        a->apply_ui_theme(ui_colors().dark, true);  // populate_controls() chose it
        a->show_panel(settings_panel_index(a->initial_panel_));
        if (a->config_.update_notify) {
            a->start_update_check(true);
            a->start_pack_check(-1);  // installed language packs
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        // Pages in the window color (light or dark); hints in gray.
        return a->control_colors(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, window_brush());
        return 1;
    }
    case WM_DPICHANGED: {
        g_dpi = static_cast<float>(LOWORD(wp)) / 96.0f;
        const RECT* rect = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        a->read_controls(false);  // keep unsaved choices across the rebuild
        a->rebuild_ui();
        return 0;
    }
    case WM_ACTIVATE: {
        // Back from the taskbar menu or another program: show what changed there. Not when one of
        // this window's own message boxes closes (its caller is still updating the page).
        DWORD other_process = 0;
        if (lp) GetWindowThreadProcessId(reinterpret_cast<HWND>(lp), &other_process);
        if (LOWORD(wp) != WA_INACTIVE && a->hList_ && other_process != GetCurrentProcessId()) {
            a->refresh_config();
        }
        break;
    }
    case WM_DESTROY:
        a->release_fonts();
        PostQuitMessage(0);
        return 0;
    case WM_COMMAND: {
        const int control_id = LOWORD(wp);
        const int notification = HIWORD(wp);
        if (control_id == kListId && notification == LBN_SELCHANGE) {
            const int idx = static_cast<int>(SendMessageW(a->hList_, LB_GETCURSEL, 0, 0));
            if (idx >= 0) {
                a->refresh_config();
                a->show_panel(idx);
            }
            return 0;
        }
        switch (control_id) {
        case kOkId:
            if (a->save_config()) {
                DestroyWindow(hwnd);
            }
            return 0;
        case kCancelId:
            DestroyWindow(hwnd);
            return 0;
        case kApplyId:
            a->save_config();
            return 0;
        default:
            a->handle_command(control_id, notification);
            return 0;
        }
    }
    case WM_NOTIFY: {
        LRESULT result = 0;
        if (a->draw_choice_button(lp, &result)) {
            return result;
        }
        if (a->handle_about_notify(lp)) {
            return 0;
        }
        break;
    }
    case DM_GETDEFID:  // Enter in IsDialogMessage: OK (the drawn buttons cannot be default)
        return MAKELRESULT(kOkId, DC_HASDEFID);
    case WM_MEASUREITEM:
        if (wp == kListId) {
            reinterpret_cast<LPMEASUREITEMSTRUCT>(lp)->itemHeight = S(38);
            return TRUE;
        }
        if (a->handle_learning_item(msg, lp)) return TRUE;
        break;
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (a->handle_learning_item(msg, lp)) return TRUE;
        if (dis->CtlType == ODT_BUTTON) {
            draw_button(*dis);
            return TRUE;
        }
        if (dis->CtlID != kListId) break;
        const int idx = static_cast<int>(dis->itemID);
        if (idx < 0 || idx >= kPanelCount) break;

        // The selected page: a tinted pill with an accent bar, like the cards' rounded corners.
        HDC dc = dis->hDC;
        const UiColors& colors = ui_colors();
        const RECT r = dis->rcItem;
        FillRect(dc, &r, window_brush());
        const RECT hr = {r.left, r.top + S(2), r.right - S(4), r.bottom - S(2)};
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        if (selected) {
            fill_round_rect(dc, hr, S(6), colors.nav_selected, colors.nav_selected);
            const RECT bar = {hr.left, hr.top + S(9), hr.left + S(3), hr.bottom - S(9)};
            fill_round_rect(dc, bar, S(1), colors.accent, colors.accent);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, selected ? colors.nav_selected_text
                                  : (colors.dark ? colors.text : RGB(60, 60, 60)));
        RECT icon_rect = {hr.left + S(12), r.top, hr.left + S(36), r.bottom};
        HGDIOBJ old_font = SelectObject(dc, get_icon_font(kFontPt));
        DrawTextW(dc, &kPanelIcons[idx], 1, &icon_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(dc, old_font);
        RECT tr_rect = {hr.left + S(40), r.top, hr.right - S(4), r.bottom};
        DrawTextW(dc, tr(kPanelKeys[idx]), -1, &tr_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        return TRUE;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace settings
} // namespace cxxime
