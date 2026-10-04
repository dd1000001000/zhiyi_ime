// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "display_attribute.h"
#include "globals.h"
#include "resource_loader.h"

// DisplayAttributeInfo
DisplayAttributeInfo::DisplayAttributeInfo(REFGUID guid, TF_DA_ATTR_INFO attribute)
    : _guid(guid), _attribute(attribute) {
    DllAddRef();
}

DisplayAttributeInfo::~DisplayAttributeInfo() {
    DllRelease();
}

STDMETHODIMP DisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
        *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) DisplayAttributeInfo::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

STDMETHODIMP DisplayAttributeInfo::GetGUID(GUID* pguid) {
    if (pguid)
        *pguid = _guid;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetDescription(BSTR* pbstrDesc) {
    if (pbstrDesc)
        *pbstrDesc = SysAllocString(cxxime_tsf::localized_display_name());
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) {
    if (!pda)
        return E_INVALIDARG;

    *pda = {};
    pda->crText.type = TF_CT_NONE;
    pda->crBk.type = TF_CT_NONE;
    pda->lsStyle = TF_LS_DOT;
    pda->fBoldLine = FALSE;
    pda->crLine.type = TF_CT_NONE;
    pda->bAttr = _attribute;
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) {
    return S_OK;
}

STDMETHODIMP DisplayAttributeInfo::Reset() {
    return S_OK;
}

// EnumDisplayAttributeInfo
EnumDisplayAttributeInfo::EnumDisplayAttributeInfo() {
    DllAddRef();
}

EnumDisplayAttributeInfo::~EnumDisplayAttributeInfo() {
    DllRelease();
}

STDMETHODIMP EnumDisplayAttributeInfo::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
        *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::AddRef() {
    return InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) EnumDisplayAttributeInfo::Release() {
    LONG cr = InterlockedDecrement(&_cRef);
    if (cr == 0)
        delete this;
    return cr;
}

STDMETHODIMP EnumDisplayAttributeInfo::Clone(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum)
        return E_INVALIDARG;
    auto* pEnum = new (std::nothrow) EnumDisplayAttributeInfo();
    if (!pEnum)
        return E_OUTOFMEMORY;
    pEnum->_index = _index;
    *ppEnum = pEnum;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo,
                                            ULONG* pcFetched) {
    if (pcFetched)
        *pcFetched = 0;
    if (!rgInfo || ulCount == 0)
        return E_INVALIDARG;

    ULONG fetched = 0;
    while (_index < 4 && fetched < ulCount) {
        const GUID* guid = &c_guidDisplayAttribute;
        TF_DA_ATTR_INFO attribute = TF_ATTR_INPUT;
        if (_index == 1) {
            guid = &c_guidConvertedDisplayAttribute;
            attribute = TF_ATTR_CONVERTED;
        } else if (_index == 2) {
            guid = &c_guidFocusedDisplayAttribute;
            attribute = TF_ATTR_TARGET_NOTCONVERTED;
        } else if (_index == 3) {
            guid = &c_guidFocusedConvertedDisplayAttribute;
            attribute = TF_ATTR_TARGET_CONVERTED;
        }
        auto* info = new (std::nothrow) DisplayAttributeInfo(*guid, attribute);
        if (!info) {
            break;
        }
        rgInfo[fetched++] = info;
        ++_index;
    }

    if (pcFetched)
        *pcFetched = fetched;
    return fetched == ulCount ? S_OK : S_FALSE;
}

STDMETHODIMP EnumDisplayAttributeInfo::Reset() {
    _index = 0;
    return S_OK;
}

STDMETHODIMP EnumDisplayAttributeInfo::Skip(ULONG ulCount) {
    _index += ulCount;
    return S_OK;
}
