// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "legacy_trace.h"

#include <cxxime/host_trace.h>

#include <cstddef>
#include <cwchar>
#include <utility>

namespace cxxime_legacy {
namespace {

nlohmann::json callback_fields(HIMC himc, const char* callback) {
    return {
        {"callback", callback ? callback : ""},
        {"himc", reinterpret_cast<uintptr_t>(himc)},
        {"hkl", reinterpret_cast<uintptr_t>(GetKeyboardLayout(0))},
    };
}

void write_callback(nlohmann::json fields) {
    cxxime::write_host_trace("legacy", "legacy.callback", std::move(fields));
}

struct ImmTextReadback {
    LONG bytes = IMM_ERROR_GENERAL;
    std::wstring text;
};

ImmTextReadback read_imm_text(HIMC himc, DWORD index) {
    ImmTextReadback result;
    if (!himc) {
        return result;
    }
    result.bytes = ImmGetCompositionStringW(himc, index, nullptr, 0);
    if (result.bytes <= 0) {
        return result;
    }
    result.text.resize(static_cast<size_t>(result.bytes) / sizeof(wchar_t));
    const LONG copied = ImmGetCompositionStringW(
        himc, index, &result.text[0], static_cast<DWORD>(result.bytes));
    if (copied < 0) {
        result.text.clear();
    } else {
        result.text.resize(static_cast<size_t>(copied) / sizeof(wchar_t));
    }
    return result;
}

nlohmann::json text_digests(const std::vector<std::wstring>& values) {
    nlohmann::json digests = nlohmann::json::array();
    for (const auto& value : values) {
        digests.push_back(cxxime::host_trace_digest_utf16(value));
    }
    return digests;
}

struct CandidateReadback {
    DWORD bytes = 0;
    DWORD count = 0;
    DWORD selection = 0;
    DWORD page_start = 0;
    DWORD page_size = 0;
    bool valid = false;
    nlohmann::json digests = nlohmann::json::array();
};

CandidateReadback read_candidate_list(HIMC himc) {
    CandidateReadback result;
    result.bytes = ImmGetCandidateListW(himc, 0, nullptr, 0);
    if (result.bytes < offsetof(CANDIDATELIST, dwOffset)) {
        return result;
    }

    std::vector<BYTE> storage(result.bytes);
    const DWORD copied = ImmGetCandidateListW(
        himc, 0, reinterpret_cast<LPCANDIDATELIST>(storage.data()), result.bytes);
    if (copied < offsetof(CANDIDATELIST, dwOffset)) {
        return result;
    }

    const auto* list = reinterpret_cast<const CANDIDATELIST*>(storage.data());
    const size_t offset_bytes = offsetof(CANDIDATELIST, dwOffset) +
                                static_cast<size_t>(list->dwCount) * sizeof(DWORD);
    if (offset_bytes > copied) {
        return result;
    }

    result.count = list->dwCount;
    result.selection = list->dwSelection;
    result.page_start = list->dwPageStart;
    result.page_size = list->dwPageSize;
    for (DWORD index = 0; index < list->dwCount; ++index) {
        const DWORD offset = list->dwOffset[index];
        if (offset >= copied || (offset % sizeof(wchar_t)) != 0) {
            return result;
        }
        const wchar_t* text = reinterpret_cast<const wchar_t*>(storage.data() + offset);
        const size_t max_length = (copied - offset) / sizeof(wchar_t);
        const size_t length = wcsnlen_s(text, max_length);
        if (length == max_length) {
            return result;
        }
        result.digests.push_back(cxxime::host_trace_digest_utf16(text, length));
    }
    result.valid = true;
    return result;
}

} // namespace

void trace_legacy_inquire(DWORD system_info_flags, bool valid_arguments) {
    cxxime::write_host_trace("legacy", "runtime.component_status", {
        {"name", "zhiyi.ime"},
        {"result", "loaded_and_called"},
    });
    auto fields = callback_fields(nullptr, "ImeInquire");
    fields["system_info_flags"] = system_info_flags;
    fields["valid_arguments"] = valid_arguments;
    write_callback(std::move(fields));
}

void trace_legacy_select(HIMC himc, bool selected) {
    auto fields = callback_fields(himc, "ImeSelect");
    fields["select"] = selected;
    write_callback(std::move(fields));
}

void trace_legacy_active_context(HIMC himc, bool active) {
    auto fields = callback_fields(himc, "ImeSetActiveContext");
    fields["active"] = active;
    write_callback(std::move(fields));
}

void trace_legacy_process_key(HIMC himc,
                              uint64_t input_id,
                              UINT virtual_key,
                              LPARAM key_data,
                              uint32_t engine_calls,
                              bool eaten,
                              const char* result,
                              bool emit_route) {
    auto fields = callback_fields(himc, "ImeProcessKey");
    fields["input_id"] = input_id;
    fields["vk"] = virtual_key;
    fields["key_data"] = static_cast<int64_t>(key_data);
    fields["owner"] = "legacy";
    fields["engine_calls"] = engine_calls;
    fields["eaten"] = eaten;
    fields["result"] = result ? result : "";
    write_callback(fields);
    if (emit_route) {
        cxxime::write_host_trace("legacy", "key.route", std::move(fields));
    }
}

void trace_legacy_to_ascii(HIMC himc,
                           uint64_t input_id,
                           UINT virtual_key,
                           UINT scan_code,
                           UINT state) {
    auto fields = callback_fields(himc, "ImeToAsciiEx");
    fields["input_id"] = input_id;
    fields["vk"] = virtual_key;
    fields["scan_code"] = scan_code;
    fields["state"] = state;
    fields["message_count"] = 0;
    fields["result"] = "no_messages";
    write_callback(std::move(fields));
}

void trace_legacy_notify(HIMC himc, DWORD action, DWORD index, DWORD value) {
    auto fields = callback_fields(himc, "NotifyIME");
    fields["action"] = action;
    fields["index"] = index;
    fields["value"] = value;
    write_callback(std::move(fields));
}

void trace_legacy_destroy() {
    auto fields = callback_fields(nullptr, "ImeDestroy");
    fields["result"] = "clear_sessions";
    write_callback(std::move(fields));
}

void trace_legacy_response(uint64_t input_id,
                           uint64_t composition_id,
                           uint32_t session_id,
                           UINT virtual_key,
                           int status,
                           bool composing,
                           size_t preedit_length,
                           size_t commit_length,
                           uint32_t candidate_count,
                           uint32_t highlighted) {
    cxxime::write_host_trace("legacy", "legacy.response", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"session", session_id},
        {"vk", virtual_key},
        {"status", status},
        {"composing", composing},
        {"preedit_len", preedit_length},
        {"commit_len", commit_length},
        {"candidate_count", candidate_count},
        {"highlighted", highlighted},
        {"result", "received"},
    });
}

void trace_legacy_candidate_signal(uint64_t input_id,
                                   uint64_t composition_id,
                                   size_t preedit_length,
                                   size_t candidate_count,
                                   uint32_t highlighted,
                                   bool candidate_was_open) {
    cxxime::write_host_trace("legacy", "candidate.signal", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"preedit_len", preedit_length},
        {"candidate_count", candidate_count},
        {"highlighted", highlighted},
        {"candidate_was_open", candidate_was_open},
        {"result", "before_messages"},
    });
}

void trace_legacy_imm_write(HIMC himc,
                            uint64_t input_id,
                            uint64_t composition_id,
                            const std::wstring& composition,
                            const std::wstring& result) {
    const ImmTextReadback composition_readback = read_imm_text(himc, GCS_COMPSTR);
    const ImmTextReadback result_readback = read_imm_text(himc, GCS_RESULTSTR);
    cxxime::write_host_trace("legacy", "imm.write", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"himc", reinterpret_cast<uintptr_t>(himc)},
        {"comp_len", composition.size()},
        {"comp_digest", cxxime::host_trace_digest_utf16(composition)},
        {"result_len", result.size()},
        {"result_digest", cxxime::host_trace_digest_utf16(result)},
        {"readback_comp_bytes", composition_readback.bytes},
        {"readback_comp_digest", cxxime::host_trace_digest_utf16(composition_readback.text)},
        {"readback_result_bytes", result_readback.bytes},
        {"readback_result_digest", cxxime::host_trace_digest_utf16(result_readback.text)},
        {"result", "written"},
    });
}

void trace_legacy_candidate_snapshot(HIMC himc,
                                     uint64_t input_id,
                                     uint64_t composition_id,
                                     const std::vector<std::wstring>& candidates,
                                     uint32_t selection,
                                     DWORD page_start,
                                     DWORD page_size) {
    const CandidateReadback readback = read_candidate_list(himc);
    nlohmann::json lengths = nlohmann::json::array();
    for (const auto& candidate : candidates) {
        lengths.push_back(candidate.size());
    }
    cxxime::write_host_trace("legacy", "candidate.snapshot", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"count", candidates.size()},
        {"selection", selection},
        {"page_start", page_start},
        {"page_size", page_size},
        {"text_lengths", std::move(lengths)},
        {"text_digests", text_digests(candidates)},
        {"readback_bytes", readback.bytes},
        {"readback_count", readback.count},
        {"readback_selection", readback.selection},
        {"readback_page_start", readback.page_start},
        {"readback_page_size", readback.page_size},
        {"readback_digests", readback.digests},
        {"readback_valid", readback.valid},
        {"result", "updated"},
    });
}

void trace_legacy_imm_message(HIMC himc,
                              uint64_t input_id,
                              uint64_t composition_id,
                              UINT message,
                              WPARAM wparam,
                              LPARAM flags,
                              bool generated) {
    cxxime::write_host_trace("legacy", "imm.message", {
        {"input_id", input_id},
        {"composition_id", composition_id},
        {"himc", reinterpret_cast<uintptr_t>(himc)},
        {"message", message},
        {"wparam", static_cast<uint64_t>(wparam)},
        {"flags", static_cast<uint64_t>(flags)},
        {"result", generated ? "generated" : "failed"},
    });
}

} // namespace cxxime_legacy
