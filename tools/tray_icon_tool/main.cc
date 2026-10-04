// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
// tray_icon_tool — put ICO files into the real system tray for visual comparison

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cxxime/data_path.h>

// ── Constants ─────────────────────────────────────────────────
static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT_PTR ID_TRAYICON = 1;
static constexpr int NUM_GROUPS = 4;

// ── Types ─────────────────────────────────────────────────────
struct IconEntry {
    std::wstring name;       // "v5_v1"
    std::wstring path;       // full path
    HICON hicon;             // loaded at system tray size
    uint64_t file_size;
};

struct IconGroup {
    std::string name;        // "v4", "v5", "v6", "legacy"
    std::vector<IconEntry> icons;
};

// ── Globals ───────────────────────────────────────────────────
static std::vector<IconGroup> g_groups;
static int g_current_group = 1;     // 0=v4, 1=v5, 2=v6, 3=legacy
static int g_current_icon = 0;      // index within current group
static bool g_in_tray = false;      // icon is registered in tray
static HWND g_hwnd = nullptr;       // message-only window
static HWND g_preview = nullptr;    // preview window
static std::wstring g_resource_dir;
static int g_tray_icon_size = 16;   // system tray icon size

// ── Resource directory discovery ──────────────────────────────
static std::wstring get_exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t pos = dir.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? dir.substr(0, pos) : dir;
}

static bool dir_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring find_resource_dir() {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc - 1; i++) {
            if (wcscmp(argv[i], L"--resource-dir") == 0) {
                std::wstring dir(argv[i + 1]);
                LocalFree(argv);
                return dir;
            }
        }
        LocalFree(argv);
    }
    std::wstring dir = get_exe_dir();
    for (int i = 0; i < 6; i++) {
        std::wstring candidate = dir + L"\\resource";
        if (dir_exists(candidate))
            return candidate;
        size_t pos = dir.find_last_of(L"\\/");
        if (pos == std::wstring::npos) break;
        dir = dir.substr(0, pos);
    }
    return L"";
}

// ── Helpers ───────────────────────────────────────────────────
static uint64_t get_file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    ULARGE_INTEGER li;
    li.LowPart = fad.nFileSizeLow;
    li.HighPart = fad.nFileSizeHigh;
    return li.QuadPart;
}

static void format_size(uint64_t bytes, char* buf, size_t buflen) {
    if (bytes < 1024)
        snprintf(buf, buflen, "%llu B", bytes);
    else
        snprintf(buf, buflen, "%.1f KB", bytes / 1024.0);
}

// ── Icon scanning ─────────────────────────────────────────────
static void scan_icons() {
    g_groups.clear();
    g_groups.resize(NUM_GROUPS);
    g_groups[0].name = "v4";
    g_groups[1].name = "v5";
    g_groups[2].name = "v6";
    g_groups[3].name = "legacy";

    g_tray_icon_size = GetSystemMetrics(SM_CXSMICON);
    if (g_tray_icon_size < 16) g_tray_icon_size = 16;

    const wchar_t* versions[] = {L"v4", L"v5", L"v6"};
    for (int v = 0; v < 3; v++) {
        std::wstring pattern = g_resource_dir + L"\\" + versions[v] + L"\\*.ico";
        WIN32_FIND_DATAW fdata;
        HANDLE hfind = FindFirstFileW(pattern.c_str(), &fdata);
        if (hfind == INVALID_HANDLE_VALUE) continue;
        do {
            IconEntry entry;
            entry.name = fdata.cFileName;
            if (entry.name.size() > 4)
                entry.name = entry.name.substr(0, entry.name.size() - 4);
            entry.path = g_resource_dir + L"\\" + versions[v] + L"\\" + fdata.cFileName;
            entry.file_size = get_file_size(entry.path);
            entry.hicon = (HICON)LoadImageW(nullptr, entry.path.c_str(),
                IMAGE_ICON, g_tray_icon_size, g_tray_icon_size,
                LR_LOADFROMFILE | LR_DEFAULTCOLOR);
            g_groups[v].icons.push_back(std::move(entry));
        } while (FindNextFileW(hfind, &fdata));
        FindClose(hfind);
    }

    std::wstring legacy_pattern = g_resource_dir + L"\\*.ico";
    WIN32_FIND_DATAW ldata;
    HANDLE lfind = FindFirstFileW(legacy_pattern.c_str(), &ldata);
    if (lfind != INVALID_HANDLE_VALUE) {
        do {
            IconEntry entry;
            entry.name = ldata.cFileName;
            if (entry.name.size() > 4)
                entry.name = entry.name.substr(0, entry.name.size() - 4);
            entry.path = g_resource_dir + L"\\" + ldata.cFileName;
            entry.file_size = get_file_size(entry.path);
            entry.hicon = (HICON)LoadImageW(nullptr, entry.path.c_str(),
                IMAGE_ICON, g_tray_icon_size, g_tray_icon_size,
                LR_LOADFROMFILE | LR_DEFAULTCOLOR);
            g_groups[3].icons.push_back(std::move(entry));
        } while (FindNextFileW(lfind, &ldata));
        FindClose(lfind);
    }
}

// ── Tray icon management ──────────────────────────────────────
static void tray_add() {
    if (g_in_tray) return;
    const auto& group = g_groups[g_current_group];
    if (g_current_icon >= (int)group.icons.size()) return;
    const auto& icon = group.icons[g_current_icon];

    NOTIFYICONDATAW nid = {};
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon.hicon;
    swprintf(nid.szTip, 128, L"%s (%hs)", icon.name.c_str(), group.name.c_str());
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        printf("Error: NIM_ADD failed (error %lu)\n", GetLastError());
        return;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    g_in_tray = true;
}

static void tray_update() {
    if (!g_in_tray) { tray_add(); return; }
    const auto& group = g_groups[g_current_group];
    if (g_current_icon >= (int)group.icons.size()) return;
    const auto& icon = group.icons[g_current_icon];

    NOTIFYICONDATAW nid = {};
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = icon.hicon;
    swprintf(nid.szTip, 128, L"%s (%hs)", icon.name.c_str(), group.name.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_remove() {
    if (!g_in_tray) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAYICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_in_tray = false;
}

// ── Preview window (shows all icons for comparison) ───────────
static HFONT g_font = nullptr;

static void update_preview() {
    if (g_preview) InvalidateRect(g_preview, nullptr, FALSE);
}

static void update_title() {
    if (!g_preview) return;
    const auto& group = g_groups[g_current_group];
    wchar_t title[256];
    swprintf(title, 256, L"Tray Icon Tool — %hs [%d/%d]  (click tray icon to cycle)",
             group.name.c_str(),
             g_current_icon + 1, (int)group.icons.size());
    SetWindowTextW(g_preview, title);
}

static void print_status() {
    const auto& group = g_groups[g_current_group];
    if (g_current_icon < (int)group.icons.size()) {
        const auto& icon = group.icons[g_current_icon];
        char size_buf[32];
        format_size(icon.file_size, size_buf, sizeof(size_buf));
        printf("Tray: %ls (%hs) [%d/%d]  size=%s  icon_size=%dpx\n",
               icon.name.c_str(), group.name.c_str(),
               g_current_icon + 1, (int)group.icons.size(),
               size_buf, g_tray_icon_size);
    }
}

static void paint_preview(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);
    int cw = client.right, ch = client.bottom;

    HDC mem_dc = CreateCompatibleDC(hdc);
    HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, cw, ch);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);
    HFONT old_font = g_font ? (HFONT)SelectObject(mem_dc, g_font) : nullptr;
    SetBkMode(mem_dc, TRANSPARENT);

    // Background
    HBRUSH bg_br = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(mem_dc, &client, bg_br);
    DeleteObject(bg_br);

    const auto& group = g_groups[g_current_group];
    int num = (int)group.icons.size();
    int cell_w = 100, cell_h = 96;
    int grid_x = 16, grid_y = 16;
    int cols = (cw - 32) / cell_w;
    if (cols < 1) cols = 1;

    // Title
    wchar_t hdr[128];
    swprintf(hdr, 128, L"%hs  —  system tray size: %dpx", group.name.c_str(), g_tray_icon_size);
    SetTextColor(mem_dc, GetSysColor(COLOR_WINDOWTEXT));
    TextOutW(mem_dc, grid_x, grid_y, hdr, (int)wcslen(hdr));
    grid_y += 28;

    // Grid
    for (int i = 0; i < num; i++) {
        int col = i % cols, row = i / cols;
        int cx = grid_x + col * cell_w;
        int cy = grid_y + row * cell_h;

        bool selected = (i == g_current_icon);

        // Cell background
        RECT cell = {cx, cy, cx + cell_w - 8, cy + cell_h - 8};
        HBRUSH cell_br = CreateSolidBrush(selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW));
        FillRect(mem_dc, &cell, cell_br);
        DeleteObject(cell_br);

        // Border
        HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1,
            selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_BTNSHADOW));
        HPEN old_pen = (HPEN)SelectObject(mem_dc, pen);
        HBRUSH old_br = (HBRUSH)SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
        Rectangle(mem_dc, cell.left, cell.top, cell.right, cell.bottom);
        SelectObject(mem_dc, old_pen);
        SelectObject(mem_dc, old_br);
        DeleteObject(pen);

        // Icon centered
        const auto& icon = group.icons[i];
        if (icon.hicon) {
            int ix = cx + (cell_w - 8 - g_tray_icon_size) / 2;
            int iy = cy + 8;
            DrawIconEx(mem_dc, ix, iy, icon.hicon,
                       g_tray_icon_size, g_tray_icon_size, 0, nullptr, DI_NORMAL);
        }

        // Label
        COLORREF text_c = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
        SetTextColor(mem_dc, text_c);
        RECT label = {cx + 4, cy + g_tray_icon_size + 12, cx + cell_w - 12, cy + cell_h - 12};
        DrawTextW(mem_dc, icon.name.c_str(), (int)icon.name.size(), &label,
                  DT_CENTER | DT_SINGLELINE | DT_TOP);
    }

    // Hints at bottom
    int hint_y = grid_y + ((num + cols - 1) / cols) * cell_h + 16;
    SetTextColor(mem_dc, GetSysColor(COLOR_GRAYTEXT));
    const wchar_t* hints = L"[1] v4  [2] v5  [3] v6  [4] legacy    [Tab] next variant    [Enter] refresh    [Esc] exit";
    TextOutW(mem_dc, grid_x, hint_y, hints, (int)wcslen(hints));

    BitBlt(hdc, 0, 0, cw, ch, mem_dc, 0, 0, SRCCOPY);
    if (old_font) SelectObject(mem_dc, old_font);
    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint_preview(hwnd);
        return 0;
    case WM_KEYDOWN:
        switch ((int)wp) {
        case '1': g_current_group = 0; g_current_icon = 0; tray_update(); update_title(); print_status(); update_preview(); break;
        case '2': g_current_group = 1; g_current_icon = 0; tray_update(); update_title(); print_status(); update_preview(); break;
        case '3': g_current_group = 2; g_current_icon = 0; tray_update(); update_title(); print_status(); update_preview(); break;
        case '4': g_current_group = 3; g_current_icon = 0; tray_update(); update_title(); print_status(); update_preview(); break;
        case VK_TAB: {
            const auto& group = g_groups[g_current_group];
            if (!group.icons.empty()) {
                g_current_icon = (g_current_icon + 1) % (int)group.icons.size();
                tray_update(); update_title(); print_status(); update_preview();
            }
            break;
        }
        case VK_RETURN:
            // Reload icons from disk (useful after regenerating)
            for (auto& g : g_groups)
                for (auto& icon : g.icons)
                    if (icon.hicon) { DestroyIcon(icon.hicon); icon.hicon = nullptr; }
            scan_icons();
            tray_update(); update_title(); print_status(); update_preview();
            printf("Reloaded icons from disk\n");
            break;
        case VK_ESCAPE:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Message-only window (receives tray callbacks) ─────────────
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAYICON) {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            // Left-click: cycle to next variant
            {
                const auto& group = g_groups[g_current_group];
                if (!group.icons.empty()) {
                    g_current_icon = (g_current_icon + 1) % (int)group.icons.size();
                    tray_update();
                    update_title();
                    print_status();
                    update_preview();
                }
            }
            break;
        case WM_RBUTTONUP:
            // Right-click: show context menu
            {
                HMENU menu = CreatePopupMenu();
                const auto& group = g_groups[g_current_group];

                // Add icon variants as menu items
                for (int i = 0; i < (int)group.icons.size(); i++) {
                    wchar_t item[128];
                    const auto& icon = group.icons[i];
                    char size_buf[32];
                    format_size(icon.file_size, size_buf, sizeof(size_buf));
                    swprintf(item, 128, L"%s  (%hs)", icon.name.c_str(), size_buf);
                    AppendMenuW(menu, (i == g_current_icon ? MF_CHECKED : MF_UNCHECKED) | MF_STRING,
                                100 + i, item);
                }

                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

                // Group selection
                for (int g = 0; g < NUM_GROUPS; g++) {
                    wchar_t item[64];
                    swprintf(item, 64, L"&%d  %hs", g + 1, g_groups[g].name.c_str());
                    AppendMenuW(menu, (g == g_current_group ? MF_CHECKED : MF_UNCHECKED) | MF_STRING,
                                200 + g, item);
                }

                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 999, L"E&xit");

                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                int sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);

                if (sel >= 100 && sel < 200) {
                    g_current_icon = sel - 100;
                    tray_update(); update_title(); print_status(); update_preview();
                } else if (sel >= 200 && sel < 300) {
                    g_current_group = sel - 200;
                    g_current_icon = 0;
                    tray_update(); update_title(); print_status(); update_preview();
                } else if (sel == 999) {
                    PostQuitMessage(0);
                }
            }
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Cleanup ───────────────────────────────────────────────────
static void cleanup() {
    tray_remove();
    for (auto& group : g_groups)
        for (auto& icon : group.icons)
            if (icon.hicon) { DestroyIcon(icon.hicon); icon.hicon = nullptr; }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
}

// ── Entry point ───────────────────────────────────────────────
int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("=== Zhiyi IME Tray Icon Preview Tool ===\n\n");

    g_resource_dir = find_resource_dir();
    if (g_resource_dir.empty()) {
        printf("Error: cannot find resource directory. Use --resource-dir <path>\n");
        return 1;
    }

    int u8len = WideCharToMultiByte(CP_UTF8, 0, g_resource_dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string rdir_utf8(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, g_resource_dir.c_str(), -1, &rdir_utf8[0], u8len, nullptr, nullptr);
    printf("Resource dir: %s\n", rdir_utf8.c_str());

    scan_icons();

    int total = 0;
    for (const auto& g : g_groups) total += (int)g.icons.size();
    printf("Loaded %d icons | system tray icon size: %dpx\n\n", total, g_tray_icon_size);
    if (total == 0) {
        printf("Error: no .ico files found\n");
        return 1;
    }

    printf("The icon is now in your system tray (bottom-right).\n");
    printf("NOTE: On Windows 10/11 the icon may be hidden in the overflow area (^).\n");
    printf("      Drag it out from the overflow menu to keep it visible.\n\n");
    printf("Keys:\n");
    printf("  1/2/3/4  = Switch v4/v5/v6/legacy group\n");
    printf("  Tab      = Next variant in current group\n");
    printf("  Enter    = Reload icons from disk\n");
    printf("  Esc      = Exit\n");
    printf("  Left-click tray icon  = cycle variant\n");
    printf("  Right-click tray icon = context menu\n\n");

    // Register window classes
    HINSTANCE hinst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc_msg = {};
    wc_msg.cbSize = sizeof(wc_msg);
    wc_msg.lpfnWndProc = MsgWndProc;
    wc_msg.hInstance = hinst;
    wc_msg.lpszClassName = L"ZhiyiIMETrayIconMsg";
    RegisterClassExW(&wc_msg);

    WNDCLASSEXW wc_prev = {};
    wc_prev.cbSize = sizeof(wc_prev);
    wc_prev.style = CS_HREDRAW | CS_VREDRAW;
    wc_prev.lpfnWndProc = PreviewWndProc;
    wc_prev.hInstance = hinst;
    wc_prev.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc_prev.lpszClassName = L"ZhiyiIMETrayIconPreview";
    RegisterClassExW(&wc_prev);

    // Create message-only window (receives tray callbacks)
    g_hwnd = CreateWindowExW(0, L"ZhiyiIMETrayIconMsg", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, nullptr);
    if (!g_hwnd) {
        printf("Error: failed to create message window\n");
        return 1;
    }

    // Create preview window
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_preview = CreateWindowExW(0, L"ZhiyiIMETrayIconPreview",
        L"Tray Icon Tool",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 360,
        nullptr, nullptr, hinst, nullptr);

    // Center preview window
    if (g_preview) {
        RECT rc;
        GetWindowRect(g_preview, &rc);
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        SetWindowPos(g_preview, nullptr, (sw - w) / 2, (sh - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(g_preview, SW_SHOW);
    }

    // Add icon to system tray
    tray_add();
    update_title();
    print_status();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    cleanup();
    return 0;
}
