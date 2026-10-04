// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "tsf_trace.h"

#include "tsf_context.h"
#include "tsf_host_callsite.h"
#include "tsf_sdl_runtime.h"

#include "candidate_ui_element.h"
#include "globals.h"
#include "text_service.h"

#include <cxxime/host_trace.h>
#include <cxxime/tsf_factory.h>

#include <cstring>
#include <utility>

namespace cxxime_tsf {

namespace {

void trace_ui(TextService* service,
              const char* event,
              const char* element_type,
              DWORD element_id,
              nlohmann::json fields) {
    fields["input_id"] = service ? service->trace_input_id() : 0;
    fields["composition_id"] = service ? service->trace_composition_id() : 0;
    fields["element_type"] = element_type ? element_type : "";
    fields["element_id"] = element_id;
    cxxime::write_host_trace("tsf", event, std::move(fields));
}

nlohmann::json text_digests(const std::vector<std::wstring>& values) {
    nlohmann::json digests = nlohmann::json::array();
    for (const auto& value : values) {
        digests.push_back(cxxime::host_trace_digest_utf16(value));
    }
    return digests;
}

} // namespace

void trace_runtime_activate(DWORD activate_flags, TfClientId client_id) {
    const bool without_com = (activate_flags & TF_TMAE_COMLESS) != 0;
    TF_INPUTPROCESSORPROFILE profile = {};
    HRESULT profile_hr = E_UNEXPECTED;
    ITfInputProcessorProfileMgr* profile_manager = nullptr;
    HRESULT profile_manager_hr = E_UNEXPECTED;
    if (without_com) {
        profile_manager_hr =
            cxxime::create_tsf_input_processor_profile_manager_without_com(
                &profile_manager);
    } else {
        profile_manager_hr = CoCreateInstance(
            CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr,
            reinterpret_cast<void**>(&profile_manager));
    }
    if (SUCCEEDED(profile_manager_hr) && profile_manager) {
        profile_hr = profile_manager->GetProfile(
            TF_PROFILETYPE_INPUTPROCESSOR, TEXTSERVICE_LANGID_HANS,
            c_clsidTextService, c_guidProfile, nullptr, &profile);
        profile_manager->Release();
    }
    const HKL keyboard_layout = GetKeyboardLayout(0);

    cxxime::write_host_trace("tsf", "runtime.component_status", {
        {"name", "zhiyi-tsf"},
        {"result", "loaded"},
    });
    cxxime::write_host_trace("tsf", "runtime.activate", {
        {"activate_flags", activate_flags},
        {"manager_creation", without_com ? "without_com" : "com"},
        {"client_id", client_id},
        {"hkl", reinterpret_cast<uintptr_t>(keyboard_layout)},
        {"hkl_is_ime", ImmIsIME(keyboard_layout) != FALSE},
        {"profile_query_hr", static_cast<int64_t>(profile_hr)},
        {"profile_manager_hr", static_cast<int64_t>(profile_manager_hr)},
        {"profile_type", profile.dwProfileType},
        {"profile_hkl", reinterpret_cast<uintptr_t>(profile.hkl)},
        {"profile_hkl_substitute", reinterpret_cast<uintptr_t>(profile.hklSubstitute)},
        {"profile_caps", profile.dwCaps},
        {"profile_flags", profile.dwFlags},
        {"ui_element_only", (activate_flags & TF_TMF_UIELEMENTENABLEDONLY) != 0},
        {"result", "success"},
    });
    if ((activate_flags & TF_TMF_UIELEMENTENABLEDONLY) != 0) {
        trace_sdl_runtime();
    }
}

void trace_key_route(uint64_t input_id,
                     uint64_t composition_id,
                     uint32_t virtual_key,
                     uint32_t modifiers,
                     uint32_t engine_calls,
                     const char* result,
                     const char* reason) {
    nlohmann::json fields = {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"owner", "tsf"},
        {"vk", virtual_key},
        {"modifiers", modifiers},
        {"engine_calls", engine_calls},
        {"result", result ? result : ""},
    };
    if (reason) {
        fields["reason"] = reason;
    }
    cxxime::write_host_trace("tsf", "key.route", std::move(fields));
}

void trace_context(uint64_t input_id,
                   uint64_t composition_id,
                   ITfContext* input_context,
                   ITfThreadMgr* thread_mgr,
                   const char* composition_transport) {
    ITfDocumentMgr* document_mgr = nullptr;
    ITfContext* top_context = nullptr;
    ITfInsertAtSelection* insert_at_selection = nullptr;
    ITfContextComposition* context_composition = nullptr;
    const HRESULT focus_hr = thread_mgr ? thread_mgr->GetFocus(&document_mgr) : E_POINTER;
    const HRESULT top_hr = document_mgr ? document_mgr->GetTop(&top_context) : E_POINTER;
    const HRESULT insert_hr = input_context
        ? input_context->QueryInterface(
            IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert_at_selection))
        : E_POINTER;
    const HRESULT composition_hr = input_context
        ? input_context->QueryInterface(
            IID_ITfContextComposition, reinterpret_cast<void**>(&context_composition))
        : E_POINTER;
    cxxime::write_host_trace("tsf", "tsf.context", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"context_present", input_context != nullptr},
        {"document_mgr_present", document_mgr != nullptr},
        {"top_context_present", top_context != nullptr},
        {"input_is_top_context", input_context && input_context == top_context},
        {"focus_hr", static_cast<int64_t>(focus_hr)},
        {"top_hr", static_cast<int64_t>(top_hr)},
        {"insert_at_selection_hr", static_cast<int64_t>(insert_hr)},
        {"context_composition_hr", static_cast<int64_t>(composition_hr)},
        {"composition_transport", composition_transport ? composition_transport : ""},
    });
    if (top_context) {
        top_context->Release();
    }
    if (document_mgr) {
        document_mgr->Release();
    }
    if (insert_at_selection) {
        insert_at_selection->Release();
    }
    if (context_composition) {
        context_composition->Release();
    }
    trace_context_state(input_id, composition_id, input_context);
}

void trace_key_result(uint64_t input_id,
                      uint64_t composition_id,
                      uint32_t virtual_key,
                      bool eaten,
                      size_t preedit_length,
                      size_t preedit_cursor,
                      uint32_t candidate_count,
                      size_t commit_length,
                      const char* result) {
    cxxime::write_host_trace("tsf", "key.result", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"vk", virtual_key},
        {"eaten", eaten},
        {"preedit_len", preedit_length},
        {"preedit_cursor", preedit_cursor},
        {"candidate_count", candidate_count},
        {"commit_len", commit_length},
        {"result", result ? result : ""},
    });
}

void trace_composition_end(uint64_t input_id,
                           uint64_t composition_id,
                           const char* reason) {
    if (composition_id == 0) {
        return;
    }
    cxxime::write_host_trace("tsf", "composition.lifecycle", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"action", "end"},
        {"reason", reason ? reason : ""},
        {"result", "closed"},
    });
}

void trace_ui_query(TextService* service,
                    const char* element_type,
                    REFIID iid,
                    HRESULT result) {
    trace_ui(service, "ui_element.query_interface", element_type, TF_INVALID_UIELEMENTID, {
        {"iid", cxxime::host_trace_guid(iid)},
        {"hr", static_cast<int64_t>(result)},
        {"result", SUCCEEDED(result) ? "supported" : "unsupported"},
    });
}

void trace_ui_show(TextService* service,
                   const char* element_type,
                   DWORD element_id,
                   bool requested_show,
                   bool actual_show,
                   HRESULT result) {
    trace_ui(service, "ui_element.show", element_type, element_id, {
        {"requested_show", requested_show},
        {"actual_show", actual_show},
        {"hr", static_cast<int64_t>(result)},
        {"result", SUCCEEDED(result) ? "success" : "failed"},
    });
}

void trace_ui_get_number(TextService* service,
                         const char* element_type,
                         DWORD element_id,
                         const char* method,
                         const char* field,
                         uint64_t value,
                         HRESULT result) {
    if (method && std::strcmp(method, "GetUpdatedFlags") == 0) {
        trace_host_ui_callsite(
            "ITfCandidateListUIElement::GetUpdatedFlags",
            element_id != TF_INVALID_UIELEMENTID);
    }
    nlohmann::json fields = {
        {"method", method ? method : ""},
        {"hr", static_cast<int64_t>(result)},
    };
    fields[field ? field : "value"] = value;
    trace_ui(service, "ui_element.get", element_type, element_id, std::move(fields));
}

void trace_ui_get_bool(TextService* service,
                       const char* element_type,
                       DWORD element_id,
                       const char* method,
                       const char* field,
                       bool value,
                       HRESULT result) {
    nlohmann::json fields = {
        {"method", method ? method : ""},
        {"hr", static_cast<int64_t>(result)},
    };
    fields[field ? field : "value"] = value;
    trace_ui(service, "ui_element.get", element_type, element_id, std::move(fields));
}

void trace_ui_get_presence(TextService* service,
                           const char* element_type,
                           DWORD element_id,
                           const char* method,
                           const char* field,
                           bool present,
                           HRESULT result) {
    nlohmann::json fields = {
        {"method", method ? method : ""},
        {"hr", static_cast<int64_t>(result)},
    };
    fields[field ? field : "present"] = present;
    trace_ui(service, "ui_element.get", element_type, element_id, std::move(fields));
}

void trace_candidate_get_string(TextService* service,
                                DWORD element_id,
                                UINT index,
                                const std::wstring* text,
                                HRESULT result) {
    nlohmann::json fields = {
        {"method", "GetString"},
        {"index", index},
        {"text_len", text ? text->size() : 0},
        {"hr", static_cast<int64_t>(result)},
    };
    if (text) {
        fields["text_digest"] = cxxime::host_trace_digest_utf16(*text);
    }
    trace_ui(service, "ui_element.get", "candidate", element_id, std::move(fields));
}

void trace_candidate_get_page(TextService* service,
                              DWORD element_id,
                              UINT buffer_size,
                              UINT page_count,
                              bool query_only,
                              UINT first_page_index,
                              HRESULT result) {
    trace_ui(service, "ui_element.get", "candidate", element_id, {
        {"method", "GetPageIndex"},
        {"buffer_size", buffer_size},
        {"page_count", page_count},
        {"query_only", query_only},
        {"first_page_index", first_page_index},
        {"hr", static_cast<int64_t>(result)},
    });
}

void trace_candidate_page_set(TextService* service,
                              DWORD element_id,
                              UINT page_count,
                              UINT first_page_index,
                              HRESULT result) {
    trace_ui(service, "candidate.behavior", "candidate", element_id, {
        {"method", "SetPageIndex"},
        {"page_count", page_count},
        {"first_page_index", first_page_index},
        {"hr", static_cast<int64_t>(result)},
    });
}

void trace_candidate_behavior_number(TextService* service,
                                     DWORD element_id,
                                     const char* method,
                                     const char* field,
                                     uint64_t value,
                                     HRESULT result) {
    nlohmann::json fields = {
        {"method", method ? method : ""},
        {"hr", static_cast<int64_t>(result)},
    };
    fields[field ? field : "value"] = value;
    trace_ui(service, "candidate.behavior", "candidate", element_id, std::move(fields));
}

void trace_candidate_behavior_bool(TextService* service,
                                   DWORD element_id,
                                   const char* method,
                                   const char* field,
                                   bool value,
                                   HRESULT result) {
    nlohmann::json fields = {
        {"method", method ? method : ""},
        {"hr", static_cast<int64_t>(result)},
    };
    fields[field ? field : "value"] = value;
    trace_ui(service, "candidate.behavior", "candidate", element_id, std::move(fields));
}

void trace_candidate_integration_style(TextService* service,
                                       DWORD element_id,
                                       REFGUID integration_style,
                                       HRESULT result) {
    trace_ui(service, "candidate.behavior", "candidate", element_id, {
        {"method", "SetIntegrationStyle"},
        {"integration_style", cxxime::host_trace_guid(integration_style)},
        {"hr", static_cast<int64_t>(result)},
    });
}

void trace_candidate_key(TextService* service,
                         DWORD element_id,
                         WPARAM virtual_key,
                         LPARAM key_data,
                         bool eaten,
                         HRESULT result) {
    trace_ui(service, "candidate.behavior", "candidate", element_id, {
        {"method", "OnKeyDown"},
        {"vk", static_cast<uint64_t>(virtual_key)},
        {"key_data", static_cast<int64_t>(key_data)},
        {"eaten", eaten},
        {"hr", static_cast<int64_t>(result)},
    });
}

void trace_candidate_snapshot(TextService* service,
                              const std::vector<std::wstring>& candidates,
                              UINT selection,
                              int page_current,
                              int page_total) {
    nlohmann::json lengths = nlohmann::json::array();
    for (const auto& candidate : candidates) {
        lengths.push_back(candidate.size());
    }
    trace_ui(service, "candidate.snapshot", "candidate", TF_INVALID_UIELEMENTID, {
        {"count", candidates.size()},
        {"selection", selection},
        {"page_starts", {0}},
        {"current_page", 0},
        {"engine_page_current", page_current},
        {"engine_page_total", page_total},
        {"updated_flags", CandidateUIElement::kPublishedUpdatedFlags},
        {"text_lengths", std::move(lengths)},
        {"text_digests", text_digests(candidates)},
        {"result", "updated"},
    });
}

void trace_candidate_lifecycle(TextService* service,
                               const char* action,
                               DWORD element_id,
                               HRESULT result,
                               const char* result_name,
                               const bool* show_external) {
    nlohmann::json fields = {
        {"hr", static_cast<int64_t>(result)},
        {"result", result_name ? result_name : ""},
    };
    if (show_external) {
        fields["show_external"] = *show_external;
    }
    std::string event = "ui_element.";
    event += action ? action : "";
    trace_ui(service, event.c_str(), "candidate", element_id, std::move(fields));
}

void trace_external_ui_decision(TextService* service,
                                DWORD element_id,
                                bool show_external) {
    trace_ui(service, "external_ui.decision", "candidate", element_id, {
        {"show_external", show_external},
        {"result", show_external ? "allowed" : "suppressed"},
        {"reason", show_external ? "host_requested_tip_ui" : "host_takeover"},
    });
}

void trace_reading_snapshot(TextService* service,
                            const std::wstring& text,
                            UINT max_length,
                            bool context_present) {
    trace_ui(service, "reading.snapshot", "reading", TF_INVALID_UIELEMENTID, {
        {"text_len", text.size()},
        {"text_digest", cxxime::host_trace_digest_utf16(text)},
        {"max_length", max_length},
        {"context_present", context_present},
        {"result", "updated"},
    });
}

void trace_reading_lifecycle(TextService* service,
                             const char* action,
                             DWORD element_id,
                             HRESULT result,
                             const char* result_name,
                             const bool* show_external) {
    nlohmann::json fields = {
        {"action", action ? action : ""},
        {"hr", static_cast<int64_t>(result)},
        {"result", result_name ? result_name : ""},
    };
    if (show_external) {
        fields["show_external"] = *show_external;
    }
    trace_ui(service, "reading.lifecycle", "reading", element_id, std::move(fields));
}

} // namespace cxxime_tsf
