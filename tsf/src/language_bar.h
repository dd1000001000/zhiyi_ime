// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_LANGUAGE_BAR_H_
#define CXXIME_TSF_LANGUAGE_BAR_H_

#include "pch.h"

#include <functional>

#include <cxxime/ime_menu.h>
#include <cxxime/ipc_protocol.h>

using ToggleChineseCallback = std::function<void()>;
using MenuCommandCallback = std::function<void(cxxime::ImeMenuCommand)>;

class CLangBarItemButton : public ITfLangBarItemButton,
                           public ITfSource {
public:
    CLangBarItemButton(TfClientId tid, REFGUID guid);
    ~CLangBarItemButton();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override;
    STDMETHODIMP GetStatus(DWORD* pdwStatus) override;
    STDMETHODIMP Show(BOOL fShow) override;
    STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override;

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) override;
    STDMETHODIMP InitMenu(ITfMenu* pMenu) override;
    STDMETHODIMP OnMenuSelect(UINT wID) override;
    STDMETHODIMP GetIcon(HICON* phIcon) override;
    STDMETHODIMP GetText(BSTR* pbstrText) override;

    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
    STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

    void update_from_status(const cxxime::ImeStatus& status);
    void set_toggle_chinese_callback(ToggleChineseCallback cb);
    void set_menu_command_callback(MenuCommandCallback cb);
    // Menu and tooltip language (the IME interface language).
    void set_menu_language(bool chinese);
    void notify_full_update();

private:
    static const DWORD LANGBARITEMSINK_COOKIE = 0x43585849; // "CXXI"

    LONG _cRef = 1;
    TfClientId _clientId;
    GUID _guid;
    bool _chinese_mode = true;
    bool _caps_lock = false;
    cxxime::ImeStatus _status;  // the menu's check marks and the tooltip
    HICON _hIconZh = nullptr;
    HICON _hIconEn = nullptr;
    HICON _hIconCaps = nullptr;
    ITfLangBarItemSink* _pSink = nullptr;
    bool _visible = true;
    ToggleChineseCallback _toggle_chinese_cb;
    MenuCommandCallback _menu_command_cb;
    bool _menu_chinese = true;
};

#endif // CXXIME_TSF_LANGUAGE_BAR_H_
