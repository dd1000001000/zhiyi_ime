// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// Win32 native controls settings editor.
//
// Modified by Zhiyi IME Contributors: four pages, translated strings, language switching.

#include "editor_app.h"

#include <algorithm>

#include <cxxime/control_client.h>
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

const char* const kPanelKeys[] = {"nav.general", "nav.fuzzy", "nav.keys", "nav.dictionary",
                                  "nav.about"};

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
    case cxxime::SettingsPanel::kAbout:
        return 4;
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
                                ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
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
                                CW_USEDEFAULT, CW_USEDEFAULT, S(660), S(420), nullptr, nullptr,
                                hInst, &app);
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
    SetWindowTextW(window, tr("window.title"));
    RECT client_rect;
    GetClientRect(window, &client_rect);

    const int right_margin = S(16);
    const int bottom_margin = S(12);
    const int button_width = S(80);
    const int button_height = S(26);
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
                             0, 0, kListW, footer_y, window,
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
    create_general_panel(hPanels_[0]);
    create_fuzzy_panel(hPanels_[1]);
    create_keys_panel(hPanels_[2]);
    create_dictionary_panel(hPanels_[3]);
    create_about_panel(hPanels_[4], panel_width);

    const struct {
        int id;
        const char* key;
        int x;
        DWORD style;
    } buttons[] = {
        {kOkId, "button.ok", ok_x, BS_DEFPUSHBUTTON},
        {kCancelId, "button.cancel", cancel_x, BS_PUSHBUTTON},
        {kApplyId, "button.apply", apply_x, BS_PUSHBUTTON},
    };
    for (const auto& button : buttons) {
        HWND control = CreateWindowExW(
            0, L"BUTTON", tr(button.key), WS_CHILD | WS_VISIBLE | WS_TABSTOP | button.style,
            button.x, button_y, button_width, button_height, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), nullptr, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    }
}

void EditorApp::destroy_controls() {
    for (HWND child = GetWindow(hwnd_, GW_CHILD); child;) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }
    hList_ = hFooter_ = hAboutTitle_ = nullptr;
    hints_.clear();
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
    for (HFONT* font : {&hFooterFont_, &hListFont_, &hAboutTitleFont_, &hHintFont_, &g_hFont}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

bool EditorApp::load_config() {
    config_ = {};
    // Defaults from the program directory, then %USERPROFILE%\zhiyi\default.json.
    const std::string default_path = cxxime::data_path("default.json");
    const std::string user_path = cxxime::user_data_path("default.json");
    const std::string themes_path = cxxime::data_path("themes.json");
    auto show_load_error = [this](const std::string& path) {
        std::wstring message = tr("error.load");
        message += L"\n\n";
        message += path_for_display(path);
        MessageBoxW(hwnd_, message.c_str(), tr("window.title"), MB_OK | MB_ICONERROR);
    };

    if (!config_.load(default_path)) {
        show_load_error(default_path);
        return false;
    }
    const std::wstring wide_user_path = path_for_display(user_path);
    const DWORD user_attributes = GetFileAttributesW(wide_user_path.c_str());
    if (user_attributes != INVALID_FILE_ATTRIBUTES) {
        if ((user_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !config_.load_user(user_path)) {
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
    if (!config_.load_themes(themes_path)) {
        show_load_error(themes_path);
        return false;
    }
    return true;
}

bool EditorApp::save_config() {
    if (!read_controls()) {
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
    return true;
}

LRESULT CALLBACK EditorApp::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditorApp* a = g_app;
    if (!a) return DefWindowProcW(hwnd, msg, wp, lp);

    const UINT navigate_message = settings_navigate_message();
    if (navigate_message != 0 && msg == navigate_message) {
        a->show_panel(settings_panel_index(static_cast<cxxime::SettingsPanel>(wp)));
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        a->hwnd_ = hwnd;
        // Strings are needed before any control exists; load_config() reports errors with them.
        load_ui_strings(resolve_ui_language(kAutoUiLanguage));
        if (!a->load_config()) {
            return -1;
        }
        a->ui_language_ = resolve_ui_language(a->config_.ui_language);
        load_ui_strings(a->ui_language_);
        a->create_controls(hwnd);
        a->populate_controls();
        a->show_panel(settings_panel_index(a->initial_panel_));
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // White pages (panels, labels, radio and check boxes); hints in gray.
        HDC dc = reinterpret_cast<HDC>(wp);
        const HWND control = reinterpret_cast<HWND>(lp);
        if (control == a->hFooter_) {
            // Filled with the window color, so the old text goes when the language changes.
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (std::find(a->hints_.begin(), a->hints_.end(), control) != a->hints_.end()) {
            SetTextColor(dc, RGB(110, 110, 110));
        }
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
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
    case WM_DESTROY:
        a->release_fonts();
        PostQuitMessage(0);
        return 0;
    case WM_COMMAND: {
        const int control_id = LOWORD(wp);
        const int notification = HIWORD(wp);
        if (control_id == kListId && notification == LBN_SELCHANGE) {
            const int idx = static_cast<int>(SendMessageW(a->hList_, LB_GETCURSEL, 0, 0));
            if (idx >= 0) a->show_panel(idx);
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
    case WM_NOTIFY:
        if (a->handle_about_notify(lp)) {
            return 0;
        }
        break;
    case WM_MEASUREITEM:
        if (wp == kListId) {
            reinterpret_cast<LPMEASUREITEMSTRUCT>(lp)->itemHeight = S(40);
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (dis->CtlID != kListId) break;
        const int idx = static_cast<int>(dis->itemID);
        if (idx < 0 || idx >= kPanelCount) break;

        HDC dc = dis->hDC;
        const RECT r = dis->rcItem;
        FillRect(dc, &r, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        const RECT hr = {r.left + 4, r.top + 3, r.right - 4, r.bottom - 3};
        if (dis->itemState & ODS_SELECTED) {
            HBRUSH brush = CreateSolidBrush(RGB(0, 122, 215));
            HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            HGDIOBJ old_brush = SelectObject(dc, brush);
            RoundRect(dc, hr.left, hr.top, hr.right, hr.bottom, 6, 6);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(brush);
            SetTextColor(dc, RGB(255, 255, 255));
        } else {
            SetTextColor(dc, RGB(60, 60, 60));
        }
        SetBkMode(dc, TRANSPARENT);
        RECT tr_rect = {hr.left + 12, r.top, hr.right - 4, r.bottom};
        DrawTextW(dc, tr(kPanelKeys[idx]), -1, &tr_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        return TRUE;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace settings
} // namespace cxxime
