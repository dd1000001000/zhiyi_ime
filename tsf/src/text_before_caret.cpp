// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "text_before_caret.h"

#include <InputScope.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <new>

namespace {

// GUID_PROP_INPUTSCOPE (InputScope.h declares it; no import library defines it).
constexpr GUID kInputScopeProperty = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool is_private_scope(InputScope scope) {
    switch (scope) {
    case IS_PASSWORD:
    case IS_PRIVATE:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
        return true;
    default:
        return false;
    }
}

// The input scopes the app attached to the caret position.
bool range_is_private(ITfContext* context, TfEditCookie ec, ITfRange* range) {
    com_ptr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(kInputScopeProperty, &property)) || !property) return false;
    VARIANT value;
    VariantInit(&value);
    bool hidden = false;
    if (SUCCEEDED(property->GetValue(ec, range, &value)) && value.vt == VT_UNKNOWN && value.punkVal) {
        com_ptr<ITfInputScope> input_scope;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&input_scope)))) {
            InputScope* scopes = nullptr;
            UINT count = 0;
            if (SUCCEEDED(input_scope->GetInputScopes(&scopes, &count)) && scopes) {
                hidden = std::any_of(scopes, scopes + count, is_private_scope);
                CoTaskMemFree(scopes);
            }
        }
    }
    VariantClear(&value);
    return hidden;
}

// Chromium (Chrome, Edge, Electron apps) marks its documents transitory, yet they hold the input
// box's text: an empty read there is an empty box, not an unreadable one.
bool is_chromium_document(ITfContext* context) {
    HWND window = nullptr;
    com_ptr<ITfContextView> view;
    if (SUCCEEDED(context->GetActiveView(&view)) && view) view->GetWnd(&window);
    if (!window) window = GetFocus();
    wchar_t name[64] = {};
    return window && GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 &&
           wcsncmp(name, L"Chrome_", 7) == 0;  // Chrome_RenderWidgetHostHWND, Chrome_WidgetWin_1
}

class ReadSession : public ITfEditSession {
public:
    ReadSession(ITfContext* context, size_t max_chars) : _context(context), _max(max_chars) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG ref = InterlockedDecrement(&_ref);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        if (FAILED(_context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
            fetched == 0 || !selection.range) {
            return E_FAIL;
        }
        com_ptr<ITfRange> caret;
        caret.Attach(selection.range);
        if (range_is_private(_context, ec, caret.Get())) {
            result = TextBeforeCaret::kPrivate;
            return S_OK;
        }
        com_ptr<ITfRange> range;
        if (FAILED(caret->Clone(&range))) return E_FAIL;
        range->Collapse(ec, TF_ANCHOR_START);  // typing replaces a selection: read before it
        LONG shifted = 0;
        if (FAILED(range->ShiftStart(ec, -static_cast<LONG>(_max), &shifted, nullptr))) return E_FAIL;
        std::wstring buffer(_max, L'\0');
        ULONG length = 0;
        if (FAILED(range->GetText(ec, 0, buffer.data(), static_cast<ULONG>(buffer.size()), &length))) {
            return E_FAIL;
        }
        buffer.resize((std::min)(static_cast<size_t>(length), buffer.size()));
        // Embedded objects (TS_CHAR_EMBEDDED) and region markers carry no text.
        buffer.erase(std::remove_if(buffer.begin(), buffer.end(),
                                    [](wchar_t c) { return c == TS_CHAR_EMBEDDED || c == TS_CHAR_REGION; }),
                     buffer.end());
        text = std::move(buffer);
        result = TextBeforeCaret::kRead;
        return S_OK;
    }

    TextBeforeCaret result = TextBeforeCaret::kUnavailable;
    std::wstring text;

private:
    LONG _ref = 1;
    ITfContext* _context;
    size_t _max;
};

}  // namespace

TextBeforeCaret read_text_before_caret(ITfContext* context, TfClientId client_id, size_t max_chars,
                                       std::wstring* text) {
    if (!context || client_id == TF_CLIENTID_NULL || max_chars == 0) return TextBeforeCaret::kUnavailable;
    // Classic Win32 edit boxes reach TSF through the IMM compatibility layer, whose transitory
    // documents hold only the composition: a read "succeeds" with no text.
    TF_STATUS status = {};
    if (SUCCEEDED(context->GetStatus(&status)) && (status.dwStaticFlags & TF_SS_TRANSITORY) != 0 &&
        !is_chromium_document(context)) {
        return TextBeforeCaret::kUnavailable;
    }
    ReadSession* session = new (std::nothrow) ReadSession(context, max_chars);
    if (!session) return TextBeforeCaret::kUnavailable;
    HRESULT session_hr = E_FAIL;
    const HRESULT hr = context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READ, &session_hr);
    TextBeforeCaret result = TextBeforeCaret::kUnavailable;
    if (SUCCEEDED(hr) && SUCCEEDED(session_hr)) {
        result = session->result;
        if (text) *text = std::move(session->text);
    }
    session->Release();
    return result;
}
