// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TEST_TSF_TEXT_SERVICE_TEST_SUPPORT_H_
#define CXXIME_TEST_TSF_TEXT_SERVICE_TEST_SUPPORT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <windows.h>
#include <msctf.h>

#include <cxxime/engine.h>
#include <cxxime/ipc_server.h>
#include <cxxime/pipe_names.h>
#include <cxxime/processor.h>

#include "support/testutil.h"
#include "text_service.h"

// Only this peer accesses service internals. These helpers arrange lifecycle boundaries;
// behavior is checked through host writes, engine state and published UI snapshots.
struct TextServiceTestPeer {
    static void bind(TextService& service, ITfContext* context) {
        service._effectiveEditTarget = {1, reinterpret_cast<uintptr_t>(context), 0, true};
        service._config.inline_preedit = true;
        service._config.preedit_type = "composition";
    }
    static bool connect(TextService& service, const std::wstring& pipe) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        const auto pipe_name = cxxime::make_user_pipe_name(pipe);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                return false;
            }
            if (WaitNamedPipeW(pipe_name.c_str(), static_cast<DWORD>(remaining))) {
                // Keep the request timeout independent of the startup wait budget.
                return service._client.connect(pipe, 2000) &&
                       service._client.start_session(service._sessionId);
            }
            if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                return false;
            }
            // No pipe exists until the accept thread starts; WaitNamedPipe returns immediately.
            Sleep(1);
        }
    }
    static cxxime::IPCResponse key(TextService& service, uint32_t key) {
        cxxime::IPCResponse response = {};
        ASSERT_TRUE(service._client.process_key(service._sessionId, key, 0, response));
        return response;
    }
    static bool apply(TextService& service, ITfContext* context,
                      const cxxime::IPCResponse& response, BOOL* eaten) {
        return service._apply_engine_response(context, response, eaten);
    }
    static bool process_key(TextService& service, ITfContext* context, uint32_t key, BOOL* eaten) {
        return service._ProcessKeyEvent(context, key, 0, eaten);
    }
    static void clear_focus(TextService& service) {
        service._clear_effective_edit_target("test_focus_lost");
    }
    static bool empty(const TextService& service) {
        return !service._composing && !service._composition &&
               service._candidatePresentation.content_state() ==
                   cxxime_tsf::CandidateContentState::kEmpty &&
               service._lastInlineCompositionText.empty();
    }
    static uint64_t generation(const TextService& service) {
        return service._candidatePresentation.generation();
    }
    static void invalidate_composition_edits(TextService& service) {
        service.invalidate_composition_edit_requests();
    }
    static void replace_presentation(TextService& service) {
        service._candidatePresentation.finish();
        service._candidatePresentation.update_content(cxxime::CandidatePage{}, "new", 3, 0, 0);
    }
    static std::string preedit(const TextService& service) {
        return service._candidatePresentation.popup_preedit();
    }
    static bool disconnected(const TextService& service) {
        return service._sessionId == 0 && !service._client.is_connected() && !service._ipcHealthy;
    }
    static void set_thread_manager(TextService& service, ITfThreadMgr* manager) {
        service._threadMgr = manager;
        manager->AddRef();
    }
    static bool target_matches(const TextService& service, ITfContext* context) {
        return service._context_matches_effective_edit_target(context);
    }
    static unsigned int pending_writes(const TextService& service) {
        return service._pendingCompositionEdits;
    }
    static void popup_only(TextService& service) { service._config.inline_preedit = false; }
    static void local_candidate_host(TextService& service, HWND view) {
        service._activateFlags |= TF_TMF_IMMERSIVEMODE;
        service._effectiveEditTarget.view_window = reinterpret_cast<uintptr_t>(view);
        service._config.render_backend = "gdi";
    }
    static void start_ui(TextService& service, const std::wstring& pipe) {
        service._uiSessionGeneration = 1;
        service._uiTargetGeneration = 1;
        ASSERT_TRUE(service._uiChannel.start({}, pipe));
    }
    static void stop_ui(TextService& service) { service._uiChannel.stop(); }
    static uint64_t caret_sample(const TextService& service) { return service._caretSampleSerial; }
    static void change_target_generation(TextService& service) { ++service._uiTargetGeneration; }
    static UINT poll_without_status(TextService& service) {
        service._activated = true;
        service._inputFocused = true;
        service._config.status_window.enable = false;
        service._lastIpcHeartbeat = std::chrono::steady_clock::now();
        service._poll_runtime_state();
        const UINT interval = service._statePollIntervalMs;
        service._stop_state_poll_timer();
        service._activated = false;
        service._inputFocused = false;
        return interval;
    }
    static void age_caret_wait(TextService& service) {
        service._candidatePresentation.begin_waiting_for_caret(
            false, nullptr,
            cxxime_tsf::CandidatePresentation::Clock::now() - std::chrono::seconds(1));
    }
    static void expire_caret_wait(TextService& service) {
        ASSERT_TRUE(service._candidatePresentation.expire_caret_wait(
            cxxime_tsf::CandidatePresentation::Clock::now() + std::chrono::seconds(1)));
    }
    static bool caret_poll_pending(const TextService& service) {
        return service._candidatePresentation.caret_poll_pending();
    }
    static void age_initial_layout(TextService& service, const RECT& provisional) {
        service._candidatePresentation.begin_waiting_for_initial_layout(
            provisional, cxxime_tsf::CandidatePresentation::Clock::now() - std::chrono::seconds(1));
    }
    static void age_completed_restart(TextService& service) {
        const auto started =
            cxxime_tsf::CandidatePresentation::Clock::now() - std::chrono::seconds(1);
        service._candidatePresentation.begin_composition_restart(started);
        ASSERT_TRUE(service._candidatePresentation.complete_composition_restart(
            service._candidatePresentation.generation(), started));
    }
};

namespace tsf_test {

// A controlled host for service integration tests, not a complete TSF text store.
// Ranges share one text buffer (Clone has no independent offsets), and tests explicitly
// complete queued writes. Native edit locks, message-loop reentrancy and window placement
// still require dedicated tests or real-host validation.
// The host owns these stack-lifetime COM objects. Reference counters still verify
// that production code releases every interface and deferred callback correctly.
template <class Interface>
class HostObject : public Interface {
public:
    ULONG references = 1;
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(Interface)) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override { return --references; }
};

class HostRange : public HostObject<ITfRange> {
public:
    bool fail_write = false;
    bool fail_read = false;
    int writes = 0;
    std::wstring text;
    std::vector<std::wstring> written_texts;
    STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* buffer, ULONG count,
                         ULONG* fetched) override {
        *fetched = 0;
        if (fail_read || flags != 0) {
            return E_FAIL;
        }
        *fetched = static_cast<ULONG>(text.copy(buffer, count));
        return S_OK;
    }
    STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR* value, LONG count) override {
        ++writes;
        if (fail_write && count > 0) {
            return E_FAIL;
        }
        text.assign(value ? value : L"", static_cast<size_t>(count));
        written_texts.push_back(text);
        return S_OK;
    }
    STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
    STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftStart(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
        *shifted = count;
        return S_OK;
    }
    STDMETHODIMP ShiftEnd(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
        *shifted = count;
        return S_OK;
    }
    STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
        *empty = text.empty();
        return S_OK;
    }
    STDMETHODIMP Collapse(TfEditCookie, TfAnchor) override { return S_OK; }
    STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
    STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
    STDMETHODIMP Clone(ITfRange** range) override {
        *range = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class HostComposition : public HostObject<ITfComposition> {
public:
    explicit HostComposition(HostRange& range)
        : range_(range) {}
    int ends = 0;
    STDMETHODIMP GetRange(ITfRange** range) override { return range_.Clone(range); }
    STDMETHODIMP ShiftStart(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEnd(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }
    STDMETHODIMP EndComposition(TfEditCookie) override {
        ++ends;
        return S_OK;
    }

private:
    HostRange& range_;
};

class HostCompartment : public HostObject<ITfCompartment> {
public:
    LONG value = 0;
    STDMETHODIMP SetValue(TfClientId, const VARIANT* data) override {
        value = data->lVal;
        return S_OK;
    }
    STDMETHODIMP GetValue(VARIANT* data) override {
        VariantInit(data);
        data->vt = VT_I4;
        data->lVal = value;
        return S_OK;
    }
};

class HostContext : public ITfContext,
                    public ITfInsertAtSelection,
                    public ITfContextComposition,
                    public ITfCompartmentMgr {
public:
    ULONG references = 1;
    HostRange range;
    HostComposition composition{range};
    HostCompartment disabled;
    HostCompartment empty;
    bool reject_composition = false;
    bool fail_selection = false;
    bool defer_write = false;
    bool honor_async = false;
    bool reject_write_request = false;
    bool fail_get_selection = false;
    HRESULT insertion_result = S_OK;
    HRESULT status_result = S_OK;
    DWORD dynamic_flags = 0;
    int read_requests = 0;
    int view_requests = 0;
    int write_requests = 0;
    int document_start_queries = 0;
    int starts = 0;
    int selections = 0;
    ITfEditSession* pending = nullptr;
    std::deque<ITfEditSession*> later_pending;
    ITfContextView* active_view = nullptr;
    ITfDocumentMgr* document = nullptr;

    ~HostContext() {
        ASSERT_TRUE(pending == nullptr);
        ASSERT_TRUE(later_pending.empty());
        ASSERT_EQ(references, 1u);
        ASSERT_EQ(range.references, 1u);
        ASSERT_EQ(composition.references, 1u);
    }
    HRESULT complete_pending() {
        ASSERT_TRUE(pending != nullptr);
        ITfEditSession* session = pending;
        pending = later_pending.empty() ? nullptr : later_pending.front();
        if (!later_pending.empty()) {
            later_pending.pop_front();
        }
        const HRESULT result = session->DoEditSession(1);
        session->Release();
        return result;
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfContext) {
            *object = static_cast<ITfContext*>(this);
        } else if (iid == IID_ITfInsertAtSelection) {
            *object = static_cast<ITfInsertAtSelection*>(this);
        } else if (iid == IID_ITfContextComposition) {
            *object = static_cast<ITfContextComposition*>(this);
        } else if (iid == IID_ITfCompartmentMgr) {
            *object = static_cast<ITfCompartmentMgr*>(this);
        }
        if (*object) {
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override { return --references; }
    STDMETHODIMP GetCompartment(REFGUID guid, ITfCompartment** out) override {
        *out = guid == GUID_COMPARTMENT_KEYBOARD_DISABLED ? &disabled
               : guid == GUID_COMPARTMENT_EMPTYCONTEXT    ? &empty
                                                          : nullptr;
        if (!*out) {
            return E_NOTIMPL;
        }
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP ClearCompartment(TfClientId, REFGUID) override { return E_NOTIMPL; }
    STDMETHODIMP EnumCompartments(IEnumGUID**) override { return E_NOTIMPL; }
    STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession* session, DWORD flags,
                                    HRESULT* result) override {
        if ((flags & TF_ES_READWRITE) == TF_ES_READWRITE) {
            ++write_requests;
        } else {
            ++read_requests;
        }
        if (reject_write_request && (flags & TF_ES_READWRITE) == TF_ES_READWRITE) {
            *result = E_FAIL;
            return E_FAIL;
        }
        if ((defer_write || (honor_async && (flags & TF_ES_ASYNC) != 0)) &&
            (flags & TF_ES_READWRITE) == TF_ES_READWRITE) {
            if (flags & TF_ES_SYNC) {
                *result = TF_E_SYNCHRONOUS;
                return S_OK;
            }
            if (pending) {
                later_pending.push_back(session);
            } else {
                pending = session;
            }
            session->AddRef();
            *result = TF_S_ASYNC;
            return S_OK;
        }
        *result = session->DoEditSession(1);
        return S_OK;
    }
    STDMETHODIMP InWriteSession(TfClientId, BOOL* writing) override {
        *writing = FALSE;
        return S_OK;
    }
    STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection,
                              ULONG* fetched) override {
        *selection = {};
        if (fail_get_selection) {
            *fetched = 0;
            return TF_E_NOSELECTION;
        }
        *fetched = 1;
        return range.Clone(&selection->range);
    }
    STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override {
        ++selections;
        return fail_selection ? E_FAIL : S_OK;
    }
    STDMETHODIMP GetStart(TfEditCookie, ITfRange** out) override {
        ++document_start_queries;
        return range.Clone(out);
    }
    STDMETHODIMP GetEnd(TfEditCookie, ITfRange** out) override { return range.Clone(out); }
    STDMETHODIMP GetActiveView(ITfContextView** view) override {
        ++view_requests;
        *view = active_view;
        if (*view) {
            (*view)->AddRef();
        }
        return *view ? S_OK : E_FAIL;
    }
    STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
    STDMETHODIMP GetStatus(TF_STATUS* status) override {
        *status = {};
        status->dwDynamicFlags = dynamic_flags;
        return status_result;
    }
    STDMETHODIMP GetProperty(REFGUID, ITfProperty** property) override {
        *property = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
    STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                                 ITfReadOnlyProperty**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** out) override {
        *out = document;
        if (*out) {
            (*out)->AddRef();
        }
        return *out ? S_OK : E_FAIL;
    }
    STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP InsertTextAtSelection(TfEditCookie, DWORD flags, const WCHAR*, LONG,
                                       ITfRange** out) override {
        ASSERT_EQ(flags, static_cast<DWORD>(TF_IAS_QUERYONLY));
        if (FAILED(insertion_result)) {
            *out = nullptr;
            return insertion_result;
        }
        return range.Clone(out);
    }
    STDMETHODIMP InsertEmbeddedAtSelection(TfEditCookie, DWORD, IDataObject*, ITfRange**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP StartComposition(TfEditCookie, ITfRange*, ITfCompositionSink*,
                                  ITfComposition** out) override {
        ++starts;
        *out = nullptr;
        if (!reject_composition) {
            *out = &composition;
            composition.AddRef();
        }
        return S_OK;
    }
    STDMETHODIMP EnumCompositions(IEnumITfCompositionView**) override { return E_NOTIMPL; }
    STDMETHODIMP FindComposition(TfEditCookie, ITfRange*, IEnumITfCompositionView**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP TakeOwnership(TfEditCookie, ITfCompositionView*, ITfCompositionSink*,
                               ITfComposition**) override {
        return E_NOTIMPL;
    }
};

class HostView : public HostObject<ITfContextView> {
public:
    HRESULT text_result = TF_E_NOLAYOUT;
    RECT text_rect = {};
    bool foreground_window = false;
    STDMETHODIMP GetRangeFromPoint(TfEditCookie, const POINT*, DWORD, ITfRange**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetTextExt(TfEditCookie, ITfRange*, RECT* rect, BOOL* clipped) override {
        *rect = text_rect;
        *clipped = FALSE;
        return text_result;
    }
    STDMETHODIMP GetScreenExt(RECT* rect) override {
        *rect = {};
        return E_FAIL;
    }
    STDMETHODIMP GetWnd(HWND* window) override {
        *window = foreground_window ? GetForegroundWindow() : nullptr;
        return S_OK;
    }
};

class HostDocument : public HostObject<ITfDocumentMgr> {
public:
    ITfContext* top = nullptr;
    STDMETHODIMP CreateContext(TfClientId, DWORD, IUnknown*, ITfContext**, TfEditCookie*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Push(ITfContext*) override { return E_NOTIMPL; }
    STDMETHODIMP Pop(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetTop(ITfContext** context) override {
        *context = top;
        if (*context) {
            (*context)->AddRef();
        }
        return S_OK;
    }
    STDMETHODIMP GetBase(ITfContext**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumContexts(IEnumTfContexts**) override { return E_NOTIMPL; }
};

class HostThreadManager : public HostObject<ITfThreadMgr> {
public:
    HostDocument document;
    STDMETHODIMP Activate(TfClientId*) override { return E_NOTIMPL; }
    STDMETHODIMP Deactivate() override { return E_NOTIMPL; }
    STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs**) override { return E_NOTIMPL; }
    STDMETHODIMP GetFocus(ITfDocumentMgr** out) override {
        *out = &document;
        document.AddRef();
        return S_OK;
    }
    STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
    STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
    STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
};

// Keep real transport and engine input state, but synthesize responses here instead
// of running SessionManager or dictionary lookup. Fixtures run sequentially in this target.
class InputServer {
public:
    cxxime::IpcServer server;
    cxxime::Engine engine;
    cxxime::PinyinProcessor processor;
    std::atomic<int> clears{0};
    bool reject_clear = false;
    std::wstring pipe =
        L"\\\\.\\pipe\\ZhiyiIME-CompositionFailure-" + std::to_wstring(GetCurrentProcessId());

    InputServer() {
        server.set_handler([this](const cxxime::IPCRequest& request) {
            cxxime::IPCResponse response = {};
            response.status = cxxime::IPCStatus::OK;
            response.ime_status.set_chinese_mode(true);
            if (request.command == cxxime::IPCCommand::START_SESSION) {
                response.highlighted = 1;
            } else if (request.command == cxxime::IPCCommand::PROCESS_KEY) {
                if (request.key_code == VK_ESCAPE) {
                    engine.clear_composition();
                    response.key_handled = true;
                    return response;
                }
                cxxime::KeyEvent key;
                key.keycode = request.key_code;
                ASSERT_EQ(processor.process_key(key, engine.context()),
                          cxxime::ProcessResult::ACCEPTED);
                const std::string& input = engine.context().active_input();
                strcpy_s(response.preedit, input.c_str());
                response.preedit_cursor = static_cast<uint32_t>(input.size());
                response.composing = true;
                response.key_handled = true;
            } else if (request.command == cxxime::IPCCommand::CLEAR_COMPOSITION) {
                ++clears;
                if (reject_clear) {
                    response.status = cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED;
                } else {
                    engine.clear_composition();
                }
            }
            return response;
        });
        ASSERT_TRUE(server.start(pipe));
    }
    ~InputServer() { server.stop(); }
};

struct Fixture {
    InputServer server;
    HostView view;
    HostThreadManager manager;
    HostContext host;
    TextService service;

    Fixture() {
        manager.document.top = &host;
        host.document = &manager.document;
        TextServiceTestPeer::bind(service, &host);
        TextServiceTestPeer::set_thread_manager(service, &manager);
        ASSERT_TRUE(TextServiceTestPeer::connect(service, server.pipe));
    }
    ~Fixture() { ASSERT_EQ(TextServiceTestPeer::pending_writes(service), 0u); }
    cxxime::IPCResponse key(uint32_t key) { return TextServiceTestPeer::key(service, key); }
    bool apply(const cxxime::IPCResponse& response, BOOL* eaten) {
        return TextServiceTestPeer::apply(service, &host, response, eaten);
    }
    void assert_cleared() {
        ASSERT_GE(server.clears.load(), 1);
        ASSERT_TRUE(server.engine.context().active_input().empty());
        ASSERT_TRUE(TextServiceTestPeer::empty(service));
    }
};

} // namespace tsf_test

#endif // CXXIME_TEST_TSF_TEXT_SERVICE_TEST_SUPPORT_H_
