// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Modified by Zhiyi IME Contributors: translated strings, project links.

#include "editor_app.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cxxime/version.h>

#include "editor_app_internal.h"

namespace cxxime {
namespace settings {
namespace {

constexpr int kProjectLinkId = 5002;
constexpr int kUpstreamLinkId = 5003;
constexpr int kGlossaryLinkId = 5006;
constexpr UINT kCopyLinkCommand = 1;
constexpr wchar_t kProjectUrl[] = L"https://github.com/dd1000001000/zhiyi_ime";
constexpr wchar_t kUpstreamUrl[] = L"https://github.com/deanxyuan/cxx-ime";
// The learning-mode word lists, open to corrections.
constexpr wchar_t kGlossaryUrl[] = L"https://github.com/dd1000001000/zhiyi-glossary";

const wchar_t* about_link_url(UINT_PTR control_id) {
    switch (control_id) {
    case kProjectLinkId:
        return kProjectUrl;
    case kUpstreamLinkId:
        return kUpstreamUrl;
    case kGlossaryLinkId:
        return kGlossaryUrl;
    case kPrivacyDocLinkId:
        return tr("privacy.doc_url");  // the document in the UI language
    case kReleaseLinkId:
        return update::kReleasesPage;
    default:
        return nullptr;
    }
}

LRESULT CALLBACK AboutLinkProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                               UINT_PTR subclass_id, DWORD_PTR) {
    if (message == WM_CONTEXTMENU) {
        const wchar_t* url = about_link_url(subclass_id);
        if (!url) {
            return 0;
        }
        POINT position = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (position.x == -1 && position.y == -1) {
            RECT rect = {};
            GetWindowRect(window, &rect);
            position.x = rect.left;
            position.y = rect.bottom;
        }
        HWND owner = GetAncestor(window, GA_ROOT);
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return 0;
        }
        AppendMenuW(menu, MF_STRING, kCopyLinkCommand, tr("about.copy_link"));
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                            position.x, position.y, 0, owner, nullptr);
        DestroyMenu(menu);
        if (command == kCopyLinkCommand && !copy_text_to_clipboard(owner, url)) {
            MessageBoxW(owner, tr("about.copy_failed"), tr("app.name"), MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, AboutLinkProc, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

} // namespace

void EditorApp::create_about_panel(HWND panel, int panel_width) {
    const int width = panel_width - kPanelPadLeft * 2;
    hAboutTitleFont_ = CreateFontW(-S(kFontPt + 2), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    auto make_text = [&](const std::wstring& text, int y, int height, HFONT font) {
        HWND control = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       kPanelPadLeft, y, width, height, panel, nullptr,
                                       GetModuleHandle(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    auto make_link = [&](int id, const wchar_t* url, int y) {
        const std::wstring markup = std::wstring(L"<a href=\"") + url + L"\">" + url + L"</a>";
        HWND link = CreateWindowExW(0, WC_LINK, markup.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    kPanelPadLeft, y, width, kCtrlH, panel,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandle(nullptr), nullptr);
        SendMessageW(link, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
        SetWindowSubclass(link, AboutLinkProc, id, 0);
    };

    int top = card_begin(panel, kPanelPadTop, nullptr);
    hAboutTitle_ = make_text(tr("app.name"), top, S(30), hAboutTitleFont_);
    make_text(std::wstring(tr("about.version")) + L" " CXXIME_VERSION_WSTRING L" · GPL-3.0",
              top + kRowH, kCtrlH, get_font());
    make_text(tr("about.tagline"), top + kRowH * 2, kCtrlH, get_font());
    top = card_end(panel, top + kRowH * 3 - S(6));

    // Label and link on separate lines, so long labels in any language fit.
    top = card_begin(panel, top, tr("about.card_links"));
    make_text(tr("about.project"), top, kCtrlH, get_font());
    make_link(kProjectLinkId, kProjectUrl, top + kRowH - S(6));
    make_text(tr("about.based_on"), top + kRowH * 2, kCtrlH, get_font());
    make_link(kUpstreamLinkId, kUpstreamUrl, top + kRowH * 3 - S(6));
    make_text(tr("about.glossary"), top + kRowH * 4, kCtrlH, get_font());
    make_link(kGlossaryLinkId, kGlossaryUrl, top + kRowH * 5 - S(6));
    card_end(panel, top + kRowH * 6 - S(10));
}

HWND make_web_link(int id, const wchar_t* text, int x, int y, int width, HWND parent) {
    const std::wstring markup = std::wstring(L"<a>") + text + L"</a>";
    HWND link = CreateWindowExW(0, WC_LINK, markup.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, x,
                                y, width, kCtrlH, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandle(nullptr), nullptr);
    SendMessageW(link, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    SetWindowSubclass(link, AboutLinkProc, id, 0);
    return link;
}

bool EditorApp::handle_about_notify(LPARAM notification) {
    auto* header = reinterpret_cast<LPNMHDR>(notification);
    const wchar_t* url = header ? about_link_url(header->idFrom) : nullptr;
    if (!url) {
        return false;
    }
    if (header->code == NM_CLICK || header->code == NM_RETURN) {
        HINSTANCE result = ShellExecuteW(hwnd_, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            MessageBoxW(hwnd_, tr("about.open_failed"), tr("app.name"), MB_OK | MB_ICONERROR);
        }
        return true;
    }
    return false;
}

} // namespace settings
} // namespace cxxime
