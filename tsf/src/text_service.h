// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_TEXT_SERVICE_H_
#define CXXIME_TSF_TEXT_SERVICE_H_

class CandidateUIElement;
class ReadingUIElement;

#include "pch.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <cxxime/config.h>
#include <cxxime/control_protocol.h>
#include <cxxime/ime_menu.h>
#include <cxxime/ipc_client.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/host_trace.h>
#include <cxxime/ui_channel.h>

#include "candidate_presentation.h"
#include "edit_target.h"
#include "effective_edit_target.h"
#include "input_indicator_controller.h"

namespace cxxime {
class CandidateWindow;
}

namespace cxxime_tsf {

bool is_valid_caret_rect(const RECT& rect);
class UiPresentationBatch;

}  // namespace cxxime_tsf

class TextService : public ITfTextInputProcessorEx,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfThreadFocusSink,
                    public ITfThreadMgrEventSink,
                    public ITfCompartmentEventSink,
                    public ITfTextEditSink,
                    public ITfTextLayoutSink,
                    public ITfDisplayAttributeProvider,
                    public ITfFunctionProvider,
                    public ITfFnSearchCandidateProvider {
public:
    TextService();
    ~TextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus, ITfDocumentMgr* pDocMgrPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ITfCompartmentEventSink
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // ITfTextEditSink
    STDMETHODIMP OnEndEdit(ITfContext* pic,
                           TfEditCookie ecReadOnly,
                           ITfEditRecord* editRecord) override;

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(ITfContext* pic,
                                TfLayoutCode lcode,
                                ITfContextView* view) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID rguid, ITfDisplayAttributeInfo** ppInfo) override;

    // ITfFunctionProvider
    STDMETHODIMP GetType(GUID* guid) override;
    STDMETHODIMP GetDescription(BSTR* description) override;
    STDMETHODIMP GetFunction(REFGUID function_guid, REFIID riid, IUnknown** function) override;

    // ITfFnSearchCandidateProvider
    STDMETHODIMP GetDisplayName(BSTR* name) override;
    STDMETHODIMP GetSearchCandidates(BSTR query,
                                     BSTR application_id,
                                     ITfCandidateList** candidates) override;
    STDMETHODIMP SetResult(BSTR query, BSTR application_id, BSTR result) override;

    // Helper
    HRESULT insert_text(const std::wstring& text, bool sync = false,
                        uint64_t* request_generation = nullptr);
    bool select_candidate_from_ui(UINT index, uint64_t candidate_revision);
    bool navigate_candidate_page_from_ui(bool previous);
    void abort_candidate_ui_from_tsf();
    HRESULT finalize_exact_candidate_ui_from_tsf();
    bool set_candidate_ui_element_shown(bool show);
    bool is_candidate_ui_element_shown() const;
    void trace_ui_element_method(const char* element, const char* method, bool important = false);
    void trace_candidate_activation_state(ITfDocumentMgr* candidate_document_mgr) const;
    uint64_t trace_input_id() const { return _hostTraceSession.input_id(); }
    uint64_t trace_composition_id() const { return _hostTraceSession.composition_id(); }
    uint64_t ensure_trace_composition_id();
    void trace_caret_event(const char* action,
                           const char* source,
                           bool resolved,
                           const RECT* rect,
                           HRESULT hr = S_OK,
                           bool important = false);
    void trace_text_ext_rect(const cxxime_tsf::TextExtRectTrace& trace);
    HRESULT update_composition(ITfContext* pic,
                               const std::wstring& preedit,
                               size_t preedit_cursor,
                               bool ensure = false,
                               DWORD edit_session_mode = TF_ES_ASYNCDONTCARE,
                               size_t converted_prefix_utf16 = 0,
                               size_t focused_start_utf16 = 0,
                               size_t focused_end_utf16 = 0,
                               bool focused_converted = false,
                               const std::optional<std::wstring>& host_termination_text =
                                   std::nullopt,
                               bool* host_edit_started = nullptr,
                               uint64_t* request_generation = nullptr,
                               const std::wstring& commit_before_preedit = L"");
    bool apply_composition_display_attributes(ITfContext* pic,
                                              ITfRange* range,
                                              TfEditCookie ec,
                                              size_t converted_prefix_utf16,
                                              size_t focused_start_utf16,
                                              size_t focused_end_utf16,
                                              bool focused_converted);
    ITfComposition* get_composition() const { return _composition; }
    void set_composition(ITfComposition* comp) { _composition = comp; }
    ITfContext* get_composition_context() const { return _compositionContext; }
    bool is_composing() const { return _composing; }
    bool is_immersive_mode() const {
        return (_activateFlags & TF_TMF_IMMERSIVEMODE) != 0;
    }
    void set_composition_context(ITfContext* context);
    void set_composing(bool val) { _composing = val; }
    bool empty_composition_placeholder_active() const {
        return _emptyCompositionPlaceholderActive;
    }
    void set_empty_composition_placeholder_active(bool active) {
        _emptyCompositionPlaceholderActive = active;
    }
    void set_applied_inline_composition_text(
        const std::wstring& text,
        const std::optional<std::wstring>& host_termination_text = std::nullopt) {
        _lastInlineCompositionText = text;
        _hostTerminationCompositionText =
            host_termination_text && text != *host_termination_text
                ? host_termination_text
                : std::nullopt;
    }
    void clear_applied_inline_composition_text() {
        set_applied_inline_composition_text(L"");
    }
    bool composition_requires_placeholder(const std::wstring& next_text) const;
    void set_caret_rect(const RECT& rc) {
        _caretRect = rc;
        _caretRectTargetGeneration = _uiTargetGeneration;
        ++_caretSampleSerial;
    }
    void remember_viewport_caret(const RECT& view_rect, const RECT& caret_rect) {
        _caretViewportTracker.remember(_uiTargetGeneration, view_rect, caret_rect);
    }
    cxxime_tsf::CaretViewportFallback resolve_viewport_caret(const RECT* view_rect,
                                                             const RECT* logical_rect, bool clipped,
                                                             RECT* caret_rect) const {
        return _caretViewportTracker.resolve(_uiTargetGeneration, view_rect, logical_rect, clipped,
                                             caret_rect);
    }
    void update_candidate_position(const RECT& rc,
                                   ITfContext* context = nullptr,
                                   bool from_layout_change = false,
                                   uint64_t expected_generation = 0,
                                   bool viewport_fallback = false);
    bool composition_edit_request_is_current(
        uint64_t expected_generation, uintptr_t expected_context_identity) const;
    uint64_t begin_composition_edit_request();
    uint64_t invalidate_composition_edit_requests();
    void register_composition_edit() { ++_pendingCompositionEdits; }
    void release_composition_edit() { --_pendingCompositionEdits; }
    DWORD ordered_composition_edit_mode(DWORD requested) const {
        return _pendingCompositionEdits > 1 ? TF_ES_ASYNC : requested;
    }
    void handle_composition_restart_success(uint64_t expected_generation);
    bool handle_composition_edit_failure(uint64_t expected_generation,
                                         uintptr_t expected_context_identity);
    RECT _resolve_caret_rect(ITfContext* pic);

    // TSF layer trace (lightweight, no cross-module QueryTrace dependency)
    enum class TsfResult : uint8_t {
        IPC_FAILED = 0,
        COMMITTED,
        PREEDIT,
        CLEARED,
        HANDLED,
        REJECTED,
    };

    struct TsfTrace {
        uint32_t vk = 0;
        uint32_t modifiers = 0;
        TsfResult result = TsfResult::REJECTED;
        uint32_t candidate_count = 0;
        uint32_t preedit_len = 0;
        uint32_t preedit_cursor = 0;
        int64_t total_us = 0;
        int64_t ipc_us = 0;
        int64_t window_us = 0;
        bool slow = false;

        const char* result_string() const;
        int to_json(char* buf, int size) const;
        bool should_log() const;
    };

private:
    friend class cxxime_tsf::UiPresentationBatch;
    friend struct TextServiceTestPeer;

    HRESULT _initialize_required_activation_sinks();
    void _initialize_optional_activation_services();
    void _synchronize_activation_focus();
    HRESULT _register_key_event_sink();
    HRESULT _unregister_key_event_sink();
    HRESULT _register_thread_mgr_event_sink();
    HRESULT _register_thread_focus_sink();
    void _unregister_thread_sinks();
    void _register_conversion_compartment_sink();
    void _unregister_conversion_compartment_sink();
    bool _register_display_attribute_atom();
    HRESULT _end_composition(ITfContext* pic, bool sync = false);
    HRESULT _commit_text(ITfContext* pic, const std::wstring& text, bool sync = false,
                         uint64_t* request_generation = nullptr);
    HRESULT _commit_then_restart_composition(ITfContext* pic,
                                             const std::wstring& commit_text,
                                             const std::wstring& preedit,
                                             size_t preedit_cursor,
                                             size_t converted_prefix_utf16,
                                             size_t focused_start_utf16,
                                             size_t focused_end_utf16,
                                             bool focused_converted,
                                             const std::optional<std::wstring>&
                                                 host_termination_text,
                                             uint64_t* request_generation = nullptr);
    bool _apply_engine_response(ITfContext* context,
                                const cxxime::IPCResponse& response,
                                BOOL* eaten,
                                TsfTrace* trace = nullptr);
    bool _ProcessKeyEvent(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);
    bool _ProcessKeyUp(WPARAM wParam, LPARAM lParam);
    void _AbortComposition();
    void _reset_trace_composition(const char* reason);
    ITfContext* _current_edit_context_for_composition() const;
    uint32_t _get_modifiers() const;
    bool _is_caps_lock_on() const;
    void _sync_ime_status(const cxxime::ImeStatus& status);
    void _handle_ime_menu_command(cxxime::ImeMenuCommand command);
    void _sync_conversion_mode_compartment(const cxxime::ImeStatus& status);
    HRESULT _read_conversion_mode_compartment(DWORD* conversion_mode,
                                              VARTYPE* value_type) const;
    bool _context_belongs_to_foreground(ITfContext* context) const;
    bool _context_matches_effective_edit_target(ITfContext* context) const;
    bool _ensure_text_edit_sink(ITfContext* context);
    bool _bind_text_edit_sink(ITfContext* context);
    void _unadvise_text_edit_sink();
    bool _bind_text_layout_sink(ITfContext* context);
    void _unadvise_text_layout_sink();
    void _request_candidate_position_update(ITfContext* pic,
                                            const char* reason,
                                            bool from_layout_change = false);
    void _follow_native_caret();
    bool _resolve_native_caret_rect(RECT* out) const;
    bool _resolve_context_native_caret_rect(ITfContext* context,
                                            RECT* out,
                                            HWND* context_window = nullptr) const;
    bool _read_context_compartment_bool(ITfContext* context, REFGUID guid, bool* value) const;
    bool _context_keyboard_disabled(ITfContext* context) const;
    const char* _input_context_block_reason(ITfContext* context) const;
    bool _context_allows_input(ITfContext* context) const;
    bool _document_allows_input(ITfDocumentMgr* doc_mgr) const;
    bool _query_input_focus_from_thread_mgr() const;
    bool _synchronize_effective_edit_target(ITfContext* event_context,
                                            ITfDocumentMgr* event_document_mgr,
                                            const char* source);
    bool _synchronize_effective_edit_target_from_thread_mgr(const char* source);
    void _clear_effective_edit_target(const char* source, bool target_unavailable = false);
    void _release_effective_edit_target();
    cxxime_tsf::EffectiveEditTargetBindings _effective_edit_target_bindings(
        const cxxime_tsf::EffectiveEditTargetSnapshot& target) const;
    void _trace_effective_edit_target_sync(
        const char* source,
        cxxime_tsf::EffectiveEditTargetAction action,
        const cxxime_tsf::EffectiveEditTargetSnapshot& previous,
        const cxxime_tsf::EffectiveEditTargetSnapshot& next,
        const cxxime_tsf::EffectiveEditTargetBindings& bindings,
        bool succeeded);
    bool _sync_caps_lock_state(bool caps_lock,
                               const char* source,
                               cxxime::ImeStatus* synced_status = nullptr);
    bool _refresh_caps_lock_on_focus(const char* source);
    void _schedule_caps_lock_refresh();
    bool _ensure_ipc_session();
    bool _recreate_ipc_session_preserving_status();
    bool _heartbeat_ipc();
    bool _refresh_input_indicator();
    void _schedule_input_indicator_refresh_retry();
    void _stop_input_indicator_refresh_retry();
    bool _has_synced_ime_status() const noexcept {
        return _hasLastImeStatus.load(std::memory_order_acquire);
    }
    void _show_status_window_if_allowed(const char* reason = "input_allowed");
    void _hide_status_window(const char* reason);
    void _show_candidate_window(const char* reason);
    void _hide_candidate_window(const char* reason);
    void _hide_candidate_projection(const char* reason);
    void _hide_external_candidate_window(const char* reason);
    bool _publish_candidate_ui_element();
    void _sync_candidate_ui_element_snapshot();
    cxxime::CandidateUiContext _candidate_ui_context() const;
    static std::wstring utf8_to_wstring(const char* text);
    void _start_host_compatibility_runtime();
    void _stop_host_compatibility_runtime();
    void _prepare_host_candidate_compatibility();
    void _update_reading_ui_element(ITfContext* context, const std::wstring& reading);
    void _end_reading_ui_element(const char* reason);
    void _trace_input_decision(const char* block_reason);
    void _update_state_poll_timer();
    void _stop_state_poll_timer();
    void _poll_runtime_state();
    static VOID CALLBACK _state_poll_timer_proc(HWND hwnd, UINT msg, UINT_PTR id_event, DWORD time);
    bool _start_config_updates();
    void _stop_config_updates();
    void _apply_config_snapshot();
    bool _start_ui_presentation_channel();
    void _stop_ui_presentation_channel();
    void _publish_ui_presentation();
    bool _present_local_candidate_window(const cxxime::CandidatePresentationPage& page,
                                         int page_current,
                                         int page_total,
                                         const std::string& preedit,
                                         std::size_t preedit_cursor, const RECT& caret);
    void _hide_local_candidate_window();
    void _publish_ui_session_ended();
    void _queue_ui_command(const cxxime::UiCommand& command);
    void _drain_ui_commands();
    bool _is_current_ui_command(const cxxime::UiCommand& command) const;
    void _handle_ui_command(const cxxime::UiCommand& command);
    static LRESULT CALLBACK _config_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    LONG _cRef = 1;
    ITfThreadMgr* _threadMgr = nullptr;
    TfClientId _clientId = TF_CLIENTID_NULL;
    ITfComposition* _composition = nullptr;
    ITfContext* _compositionContext = nullptr;
    DWORD _activateFlags = 0;
    DWORD _dwThreadFocusCookie = TF_INVALID_COOKIE;
    DWORD _dwThreadMgrEventCookie = TF_INVALID_COOKIE;
    DWORD _dwConversionCompartmentCookie = TF_INVALID_COOKIE;
    DWORD _dwTextEditSinkCookie = TF_INVALID_COOKIE;
    DWORD _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
    TfGuidAtom _displayAttributeAtom = 0;
    TfGuidAtom _convertedDisplayAttributeAtom = 0;
    TfGuidAtom _focusedDisplayAttributeAtom = 0;
    TfGuidAtom _focusedConvertedDisplayAttributeAtom = 0;
    ITfContext* _textEditSinkContext = nullptr;
    ITfContext* _textLayoutSinkContext = nullptr;
    ITfDocumentMgr* _effectiveDocumentMgr = nullptr;
    ITfContext* _effectiveContext = nullptr;
    cxxime_tsf::EffectiveEditTargetSnapshot _effectiveEditTarget;
    ITfCompartment* _conversionCompartment = nullptr;
    ITfSource* _conversionCompartmentSource = nullptr;

    cxxime::IpcClient _client;
    uint32_t _sessionId = 0;
    bool _composing = false;
    uint64_t _compositionEditGeneration = 0;
    unsigned int _pendingCompositionEdits = 0;
    bool _emptyCompositionPlaceholderActive = false;
    bool _chinese_mode = true;
    bool _caps_lock = false;
    std::mutex _lastImeStatusMutex;
    cxxime::ImeStatus _lastImeStatus;
    std::atomic<bool> _hasLastImeStatus{false};
    bool _activated = false;
    bool _inputFocused = false;
    bool _inputTargetUnavailable = false;
    unsigned int _runtimeTargetValidationFailures = 0;
    bool _fTestKeyDownPending = false;
    bool _fTestKeyUpPending = false;
    std::bitset<256> _passThroughKeyUps;
    bool _hostCompatibilityRuntimeActive = false;
    bool _writingConversionCompartment = false;
    bool _handlingConversionCompartmentChange = false;
    UINT_PTR _statePollTimer = 0;
    UINT _statePollIntervalMs = 0;
    std::chrono::steady_clock::time_point _lastIpcHeartbeat = {};
    bool _ipcHealthy = true;
    std::string _lastInputBlockReason;
    std::wstring _lastInlineCompositionText;
    std::optional<std::wstring> _hostTerminationCompositionText;
    cxxime_tsf::CandidatePresentation _candidatePresentation;
    CandidateUIElement* _candidateUiElement = nullptr;
    ReadingUIElement* _readingUiElement = nullptr;
    cxxime::Config _config;
    std::unique_ptr<cxxime::CandidateWindow> _localCandidateWindow;
    std::uint64_t _localCandidatePlacementTargetGeneration = 0;
    cxxime::ConfigGeneration _configGeneration;
    HWND _configWindow = nullptr;
    std::uint32_t _configSubscriptionId = 0;
    bool _capsLockRefreshPending = false;

    cxxime_tsf::InputIndicatorController _inputIndicator;

    RECT _caretRect = {};
    std::uint64_t _caretRectTargetGeneration = 0;
    std::uint64_t _caretSampleSerial = 0;
    cxxime_tsf::CaretViewportTracker _caretViewportTracker;

    cxxime::UiChannelClient _uiChannel;
    std::mutex _uiCommandMutex;
    std::deque<cxxime::UiCommand> _uiCommands;
    std::optional<cxxime::UiCommand> _pendingInputIndicatorRefreshCommand;
    std::uint64_t _uiSessionGeneration = 0;
    std::uint64_t _uiTargetGeneration = 0;
    unsigned int _inputIndicatorRefreshRetryCount = 0;
    bool _inputIndicatorRefreshRetryActive = false;
    unsigned int _uiPresentationBatchDepth = 0;
    bool _uiPresentationPublishPending = false;

    std::chrono::steady_clock::time_point _key_event_start;
    int64_t _last_ipc_us = 0;
    int64_t _last_window_update_us = 0;
    cxxime::HostTraceSession _hostTraceSession;

    // Async trace writer (bounded queue, writer thread, batch flush)
    void _enqueue_trace(const TsfTrace& trace);
    void _enqueue_event_trace(const char* event, const char* detail, bool important = false);
    void _enqueue_ui_presentation_trace(const cxxime::UiPresentationSnapshot& snapshot);
};

#endif // CXXIME_TSF_TEXT_SERVICE_H_
