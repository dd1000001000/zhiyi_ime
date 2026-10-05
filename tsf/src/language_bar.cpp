// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "language_bar.h"

#include <cwchar>
#include <string>

#include <cxxime/logging.h>

#include "globals.h"
#include "resource_loader.h"

#ifndef CONNECT_E_CANNOTCONNECT
#define CONNECT_E_CANNOTCONNECT 0x80040200
#endif
#ifndef CONNECT_E_ADVISELIMIT
#define CONNECT_E_ADVISELIMIT 0x80040201
#endif
#ifndef CONNECT_E_NOCONNECTION
#define CONNECT_E_NOCONNECTION 0x80040202
#endif

namespace {

UINT mode_icon_id(bool chinese_mode, bool caps_lock) {
    return caps_lock ? IDI_ICON_C : (chinese_mode ? IDI_ICON_ZH : IDI_ICON_EN);
}

}  // namespace

CLangBarItemButton::CLangBarItemButton(TfClientId tid, REFGUID guid)
    : _clientId(tid), _guid(guid), _chinese_mode(true), _pSink(nullptr) {
    DllAddRef();
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    _hIconZh = cxxime_tsf::load_resource_icon(IDI_ICON_ZH, cx, cy);
    _hIconEn = cxxime_tsf::load_resource_icon(IDI_ICON_EN, cx, cy);
    _hIconCaps = cxxime_tsf::load_resource_icon(IDI_ICON_C, cx, cy);
    if (!_hIconZh || !_hIconEn || !_hIconCaps) {
        CXXIME_LOG(L"ModeButton icons load failed: zh=%d, en=%d, caps=%d",
                   _hIconZh ? 1 : 0, _hIconEn ? 1 : 0, _hIconCaps ? 1 : 0);
    }
}

CLangBarItemButton::~CLangBarItemButton() {
    if (_pSink) {
        _pSink->Release();
        _pSink = nullptr;
    }
    if (_hIconZh) {
        DestroyIcon(_hIconZh);
        _hIconZh = nullptr;
    }
    if (_hIconEn) {
        DestroyIcon(_hIconEn);
        _hIconEn = nullptr;
    }
    if (_hIconCaps) {
        DestroyIcon(_hIconCaps);
        _hIconCaps = nullptr;
    }
    DllRelease();
}

// IUnknown
STDMETHODIMP CLangBarItemButton::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
        IsEqualIID(riid, IID_ITfLangBarItemButton))
        *ppvObj = static_cast<ITfLangBarItemButton*>(this);
    else if (IsEqualIID(riid, IID_ITfSource))
        *ppvObj = static_cast<ITfSource*>(this);

    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CLangBarItemButton::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) CLangBarItemButton::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

// ITfLangBarItem
STDMETHODIMP CLangBarItemButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
    if (!pInfo)
        return E_INVALIDARG;

    pInfo->clsidService = c_clsidTextService;
    pInfo->guidItem = _guid;
    pInfo->dwStyle = TF_LBI_STYLE_SHOWNINTRAY | TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU;
    pInfo->ulSort = 0;
    wcscpy_s(pInfo->szDescription, cxxime_tsf::localized_display_name());
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetStatus(DWORD* pdwStatus) {
    if (pdwStatus) {
        *pdwStatus = 0;
        if (!_visible) {
            *pdwStatus |= TF_LBI_STATUS_HIDDEN;
            return S_OK;
        }
        if (_chinese_mode)
            *pdwStatus |= TF_LBI_STATUS_BTN_TOGGLED;
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::Show(BOOL fShow) {
    bool visible = !!fShow;
    if (_visible == visible)
        return S_OK;

    _visible = visible;
    if (_pSink) {
        HRESULT hr = _pSink->OnUpdate(TF_LBI_STATUS);
        CXXIME_LOG(L"ModeButton Show: visible=%d, hr=0x%08x", _visible ? 1 : 0, hr);
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetTooltipString(BSTR* pbstrToolTip) {
    if (!pbstrToolTip)
        return E_INVALIDARG;
    // Every state at a glance, e.g. "知意 - 中文 · 全拼 · 半角 · 中文标点".
    const std::wstring tip = std::wstring(_menu_chinese ? L"知意 - " : L"Zhiyi - ") +
                             cxxime::ime_status_summary(_status, _menu_chinese);
    *pbstrToolTip = SysAllocString(tip.c_str());
    return S_OK;
}

// ITfLangBarItemButton
STDMETHODIMP CLangBarItemButton::OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) {
    CXXIME_LOG(L"OnClick: click=%d, has_callback=%d", static_cast<int>(click),
               _toggle_chinese_cb ? 1 : 0);
    if (click == TF_LBI_CLK_LEFT) {
        if (_toggle_chinese_cb) {
            _toggle_chinese_cb();
        }
    } else if (click == TF_LBI_CLK_RIGHT) {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return E_FAIL;

        for (const cxxime::ImeMenuItem& item : cxxime::kImeMenuItems) {
            if (item.starts_group) {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            }
            UINT flags = MF_STRING;
            if (cxxime::ime_menu_command_checked(item.command, _status)) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(hMenu, flags, static_cast<UINT>(item.command),
                        cxxime::ime_menu_item_label(item, _menu_chinese));
        }

        // Get foreground window for TrackPopupMenuEx
        HWND hwnd = GetForegroundWindow();
        UINT wID = TrackPopupMenuEx(hMenu,
                                    TPM_NONOTIFY | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, hwnd, nullptr);
        DestroyMenu(hMenu);

        if (wID > 0) {
            OnMenuSelect(wID);
        }
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::InitMenu(ITfMenu* pMenu) {
    if (!pMenu) return E_INVALIDARG;

    for (const cxxime::ImeMenuItem& item : cxxime::kImeMenuItems) {
        if (item.starts_group) {
            pMenu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr,
                               nullptr, 0, nullptr);
        }
        DWORD flags = cxxime::ime_menu_command_checked(item.command, _status)
                          ? TF_LBMENUF_CHECKED
                          : 0;
        const wchar_t* label = cxxime::ime_menu_item_label(item, _menu_chinese);
        pMenu->AddMenuItem(static_cast<UINT>(item.command), flags, nullptr, nullptr,
                           label, static_cast<ULONG>(std::wcslen(label)), nullptr);
    }

    return S_OK;
}

STDMETHODIMP CLangBarItemButton::OnMenuSelect(UINT wID) {
    const cxxime::ImeMenuItem* selected = cxxime::find_ime_menu_item(wID);
    if (selected && _menu_command_cb) {
        _menu_command_cb(selected->command);
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetIcon(HICON* phIcon) {
    if (!phIcon) return E_INVALIDARG;
    UINT iconId = mode_icon_id(_chinese_mode, _caps_lock);
    HICON source = _caps_lock ? _hIconCaps : (_chinese_mode ? _hIconZh : _hIconEn);
    *phIcon = source ? CopyIcon(source) : nullptr;
    if (!*phIcon) {
        CXXIME_LOG(L"ModeButton GetIcon failed: chinese=%d, caps=%d, iconId=%u",
                   _chinese_mode ? 1 : 0, _caps_lock ? 1 : 0, iconId);
    }
    return (*phIcon == nullptr) ? E_FAIL : S_OK;
}

STDMETHODIMP CLangBarItemButton::GetText(BSTR* pbstrText) {
    if (!pbstrText)
        return E_INVALIDARG;
    *pbstrText = SysAllocString(cxxime_tsf::localized_display_name());
    return S_OK;
}

// ITfSource
STDMETHODIMP CLangBarItemButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) {
    CXXIME_LOG(L"AdviseSink called, riid=?");
    if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
        return CONNECT_E_CANNOTCONNECT;

    // If already have a sink, release it first (handle re-advise)
    if (_pSink != nullptr) {
        CXXIME_LOG(L"AdviseSink: releasing existing sink=0x%p", _pSink);
        _pSink->Release();
        _pSink = nullptr;
    }

    if (punk->QueryInterface(IID_ITfLangBarItemSink, (LPVOID*)&_pSink) != S_OK) {
        _pSink = nullptr;
        CXXIME_LOG(L"AdviseSink: QI for ITfLangBarItemSink FAILED");
        return E_NOINTERFACE;
    }
    *pdwCookie = LANGBARITEMSINK_COOKIE;
    CXXIME_LOG(L"AdviseSink: OK, sink=0x%p", _pSink);
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::UnadviseSink(DWORD dwCookie) {
    if (dwCookie != LANGBARITEMSINK_COOKIE || _pSink == nullptr)
        return CONNECT_E_NOCONNECTION;
    _pSink->Release();
    _pSink = nullptr;
    return S_OK;
}

void CLangBarItemButton::update_from_status(const cxxime::ImeStatus& status) {
    const bool tooltip_changed =
        status.input_mode != _status.input_mode || status.flags != _status.flags;
    _status = status;
    if (_chinese_mode == status.chinese_mode() && _caps_lock == status.caps_lock()) {
        if (tooltip_changed && _pSink) {
            _pSink->OnUpdate(TF_LBI_TOOLTIP);  // e.g. punctuation or full width
        }
        return;
    }
    {
        UINT old_icon = mode_icon_id(_chinese_mode, _caps_lock);
        UINT new_icon = mode_icon_id(status.chinese_mode(), status.caps_lock());
        _chinese_mode = status.chinese_mode();
        _caps_lock = status.caps_lock();
        if (_pSink) {
            DWORD flags = TF_LBI_STATUS | TF_LBI_TOOLTIP;
            if (old_icon != new_icon) flags |= TF_LBI_ICON;
            HRESULT hr = _pSink->OnUpdate(flags);
            CXXIME_LOG(L"ModeButton OnUpdate: chinese=%d, caps=%d, flags=0x%08x, hr=0x%08x",
                       _chinese_mode ? 1 : 0, _caps_lock ? 1 : 0, flags, hr);
        }
    }
}

void CLangBarItemButton::set_toggle_chinese_callback(ToggleChineseCallback cb) {
    _toggle_chinese_cb = std::move(cb);
}

void CLangBarItemButton::set_menu_command_callback(MenuCommandCallback cb) {
    _menu_command_cb = std::move(cb);
}

void CLangBarItemButton::set_menu_language(bool chinese) {
    if (_menu_chinese == chinese) return;
    _menu_chinese = chinese;
    if (_pSink) _pSink->OnUpdate(TF_LBI_TOOLTIP);
}

void CLangBarItemButton::notify_full_update() {
    if (_pSink) {
        const DWORD flags = TF_LBI_STATUS | TF_LBI_ICON | TF_LBI_TOOLTIP | TF_LBI_TEXT;
        const HRESULT hr = _pSink->OnUpdate(flags);
        CXXIME_LOG(L"ModeButton full update: flags=0x%08x, hr=0x%08x", flags, hr);
    }
}
