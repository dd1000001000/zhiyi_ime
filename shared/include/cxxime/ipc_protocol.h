// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_IPC_PROTOCOL_H_
#define CXXIME_IPC_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <cxxime/candidate.h>
#include <cxxime/input_limits.h>

namespace cxxime {

constexpr uint32_t IPC_WIRE_MAGIC = 0x43584950;  // "CXIP"
constexpr uint16_t IPC_WIRE_MIN_COMPATIBLE_VERSION = 1;
constexpr uint16_t IPC_WIRE_VERSION = 1;
constexpr uint32_t IPC_REQUEST_BASELINE_SIZE = 576;
constexpr uint32_t IPC_RESPONSE_BASELINE_SIZE = 3176;

// The framed wire protocol starts at 0.4.0; the 0.3.0 raw-struct format is intentionally
// unsupported. Payloads are append-only from this baseline. Existing fields and offsets must
// never be removed or reordered. Readers accept larger payloads and ignore unknown tails.

struct IPCWireHeader {
    uint32_t magic = IPC_WIRE_MAGIC;
    uint16_t version = IPC_WIRE_VERSION;
    uint16_t header_size = 12;
    uint32_t payload_size = 0;
};

static_assert(sizeof(IPCWireHeader) == 12, "IPCWireHeader layout changed");

enum class InputMode : uint32_t {
    PINYIN = 0,
    WUBI = 1,
    MIXED = 2,
};

enum class ImeStatusFlag : uint32_t {
    CHINESE_MODE = 1u << 0,
    CAPS_LOCK = 1u << 1,
    FULL_SHAPE = 1u << 2,
    CHINESE_PUNCT = 1u << 3,
    // English mode style: word completion (set) or plain letter-by-letter input (clear).
    ENGLISH_WORDS = 1u << 4,
    // Chinese pinyin style: initials only (set) or full pinyin (clear).
    PINYIN_INITIALS = 1u << 5,
};

constexpr uint32_t ime_status_flag(ImeStatusFlag flag) noexcept {
    return static_cast<uint32_t>(flag);
}

struct ImeStatus {
    uint32_t flags = ime_status_flag(ImeStatusFlag::CHINESE_MODE) |
                     ime_status_flag(ImeStatusFlag::CHINESE_PUNCT) |
                     ime_status_flag(ImeStatusFlag::ENGLISH_WORDS);
    InputMode input_mode = InputMode::PINYIN;
    uint64_t revision = 0;

    bool has_flag(ImeStatusFlag flag) const noexcept {
        return (flags & ime_status_flag(flag)) != 0;
    }

    void set_flag(ImeStatusFlag flag, bool enabled) noexcept {
        const uint32_t mask = ime_status_flag(flag);
        if (enabled) {
            flags |= mask;
        } else {
            flags &= ~mask;
        }
    }

    bool chinese_mode() const noexcept {
        return has_flag(ImeStatusFlag::CHINESE_MODE);
    }

    void set_chinese_mode(bool enabled) noexcept {
        set_flag(ImeStatusFlag::CHINESE_MODE, enabled);
    }

    bool caps_lock() const noexcept {
        return has_flag(ImeStatusFlag::CAPS_LOCK);
    }

    void set_caps_lock(bool enabled) noexcept {
        set_flag(ImeStatusFlag::CAPS_LOCK, enabled);
    }

    bool full_shape() const noexcept {
        return has_flag(ImeStatusFlag::FULL_SHAPE);
    }

    void set_full_shape(bool enabled) noexcept {
        set_flag(ImeStatusFlag::FULL_SHAPE, enabled);
    }

    bool chinese_punct() const noexcept {
        return has_flag(ImeStatusFlag::CHINESE_PUNCT);
    }

    void set_chinese_punct(bool enabled) noexcept {
        set_flag(ImeStatusFlag::CHINESE_PUNCT, enabled);
    }

    bool english_words() const noexcept {
        return has_flag(ImeStatusFlag::ENGLISH_WORDS);
    }

    void set_english_words(bool enabled) noexcept {
        set_flag(ImeStatusFlag::ENGLISH_WORDS, enabled);
    }

    bool pinyin_initials() const noexcept {
        return has_flag(ImeStatusFlag::PINYIN_INITIALS);
    }

    void set_pinyin_initials(bool enabled) noexcept {
        set_flag(ImeStatusFlag::PINYIN_INITIALS, enabled);
    }
};

static_assert(std::is_standard_layout<ImeStatus>::value, "ImeStatus must use standard layout");
static_assert(std::is_trivially_copyable<ImeStatus>::value,
              "ImeStatus must remain trivially copyable");
static_assert(alignof(ImeStatus) == 8, "ImeStatus alignment changed");
static_assert(sizeof(ImeStatus) == 16, "ImeStatus size changed");
static_assert(offsetof(ImeStatus, flags) == 0, "ImeStatus::flags offset changed");
static_assert(offsetof(ImeStatus, input_mode) == 4, "ImeStatus::input_mode offset changed");
static_assert(offsetof(ImeStatus, revision) == 8, "ImeStatus::revision offset changed");

enum class IPCCommand : uint32_t {
    START_SESSION = 1,
    END_SESSION = 2,
    PROCESS_KEY = 3,
    SELECT_CANDIDATE = 4,
    COMMIT_COMPOSITION = 5,
    CLEAR_COMPOSITION = 6,
    FOCUS_IN = 7,
    FOCUS_OUT = 8,
    TOGGLE_CHINESE = 9,
    TOGGLE_SHAPE = 10,
    TOGGLE_PUNCT = 11,
    SWITCH_INPUT_MODE = 12,
    GET_STATUS = 13,
    SYNC_CAPS_LOCK = 16,
    PING = 22,
    SET_CHINESE_MODE = 24,
    SEARCH_CANDIDATES = 25,
    SEARCH_CANDIDATE_RESULT = 26,
    OPEN_SETTINGS = 27,
    // The program and window title of the focused input (search_query: program file name,
    // search_result: window title, UTF-8). Sent only while input collection is allowed.
    SET_INPUT_TARGET = 28,
};

enum class IPCStatus : uint32_t {
    OK = 0,
    ERR_UNKNOWN_COMMAND = 1,
    ERR_INVALID_SESSION = 2,
    ERR_ENGINE_NOT_INITIALIZED = 100,
    ERR_ENGINE_PROCESS_FAILED = 101,
    ERR_STALE_CANDIDATE = 102,
};

constexpr uint64_t kClientCapabilitySegmentedSelection = 1ULL << 0;
constexpr uint64_t kClientCapabilitySegmentedPreeditPresentation = 1ULL << 1;

enum class PreeditPresentationFlag : uint32_t {
    SyllableBoundaries = 1u << 0,
};

constexpr uint32_t preedit_presentation_flag(PreeditPresentationFlag flag) noexcept {
    return static_cast<uint32_t>(flag);
}

struct CandidateUiContext {
    enum class Presenter : uint32_t {
        NONE = 0,
        SERVER = 1,
        LOCAL = 2,
        HOST = 3,
    };

    uint64_t session_generation = 0;
    uint64_t target_generation = 0;
    uint64_t composition_generation = 0;
    uint64_t presentation_generation = 0;
    uint32_t local_visible_candidate_count = 0;
    Presenter presenter = Presenter::NONE;
};

static_assert(std::is_standard_layout<CandidateUiContext>::value,
              "CandidateUiContext must use standard layout");
static_assert(std::is_trivially_copyable<CandidateUiContext>::value,
              "CandidateUiContext must remain trivially copyable");
static_assert(alignof(CandidateUiContext) == 8, "CandidateUiContext alignment changed");
static_assert(sizeof(CandidateUiContext) == 40, "CandidateUiContext size changed");

constexpr uint32_t candidate_ui_visible_count(const CandidateUiContext& context,
                                              uint32_t server_visible_count) noexcept {
    const uint32_t capacity = static_cast<uint32_t>(kCandidateCapacity);
    if (context.presenter == CandidateUiContext::Presenter::LOCAL) {
        return context.local_visible_candidate_count == 0
                   ? 1
                   : (context.local_visible_candidate_count < capacity
                          ? context.local_visible_candidate_count
                          : capacity);
    }
    if (context.presenter == CandidateUiContext::Presenter::SERVER) {
        return server_visible_count == 0
                   ? 1
                   : (server_visible_count < capacity ? server_visible_count : capacity);
    }
    return 0;
}

struct IPCRequest {
    IPCCommand command;
    uint32_t session_id = 0;
    uint32_t key_code = 0;
    uint32_t modifiers = 0;
    uint32_t candidate_index = 0;  // Candidate, input mode, or settings panel by command.
    uint32_t is_key_up = 0;
    CandidateUiContext candidate_ui;
    // UTF-8 query for SEARCH_CANDIDATES; unused by input-session commands.
    char search_query[256] = {};
    char search_result[256] = {};
    // Fields after search_result form the 0.5 append-only extension.
    uint64_t client_capabilities = 0;
    uint64_t candidate_revision = 0;
};

static_assert(std::is_standard_layout<IPCRequest>::value,
              "IPCRequest must use standard layout");
static_assert(std::is_trivially_copyable<IPCRequest>::value,
              "IPCRequest must remain trivially copyable");
static_assert(alignof(IPCRequest) == 8, "IPCRequest alignment changed");
static_assert(sizeof(IPCRequest) >= IPC_REQUEST_BASELINE_SIZE,
              "IPCRequest dropped fields from the 0.4.0 baseline");
static_assert(offsetof(IPCRequest, is_key_up) == 20, "IPCRequest::is_key_up offset changed");
static_assert(offsetof(IPCRequest, candidate_ui) == 24,
              "IPCRequest::candidate_ui offset changed");
static_assert(offsetof(IPCRequest, search_query) == 64,
              "IPCRequest::search_query offset changed");
static_assert(offsetof(IPCRequest, search_result) == 320,
              "IPCRequest::search_result offset changed");
static_assert(offsetof(IPCRequest, client_capabilities) == IPC_REQUEST_BASELINE_SIZE,
              "IPCRequest 0.5 extension moved into the 0.4 baseline");

struct IPCResponse {
    IPCStatus status = IPCStatus::OK;
    char commit_text[256] = {};
    char preedit[256] = {};
    uint32_t preedit_cursor = 0;
    uint32_t candidate_count = 0;
    uint32_t candidate_offset = 0;
    uint32_t candidate_total = 0;
    char candidates[kCandidateCapacity][kCandidateTextCapacity] = {};
    char candidate_hints[kCandidateCapacity][4] = {};  // Remaining Wubi code, up to 3 ASCII letters.
    uint32_t highlighted = 0;
    uint32_t ascii_mode = 0;
    uint32_t composing = 0;
    ImeStatus ime_status;
    uint32_t page_current = 1;
    uint32_t page_total = 1;
    uint32_t key_handled = 0;
    uint32_t server_process_id = 0;
    // Fields after server_process_id form the 0.5 append-only extension.
    uint64_t candidate_revision = 0;
    uint32_t converted_prefix_bytes = 0;
    uint32_t focused_preedit_start_bytes = 0;
    uint32_t focused_preedit_end_bytes = 0;
    uint32_t preedit_presentation_flags = 0;
    uint32_t candidate_known_count = 0;
    CandidateExtentState candidate_extent_state = CandidateExtentState::kExhausted;
    uint32_t candidate_extent_complete = 1;
    // Bit i: candidates[i] is the Laya recommendation (sparkle mark).
    uint32_t candidate_recommended_mask = 0;
    // Text shown after candidates[i]: the Wubi remaining code or the correct pinyin of a fuzzy
    // match ("zhong guo"). candidate_hints only fits 3 letters; it keeps short hints for older
    // clients.
    char candidate_comments[kCandidateCapacity][kCandidateCommentCapacity] = {};
};

static_assert(std::is_standard_layout<IPCResponse>::value,
              "IPCResponse must use standard layout");
static_assert(std::is_trivially_copyable<IPCResponse>::value,
              "IPCResponse must remain trivially copyable");
static_assert(alignof(IPCResponse) == 8, "IPCResponse alignment changed");
static_assert(offsetof(IPCResponse, candidates) == 532,
              "IPCResponse::candidates offset changed");
static_assert(offsetof(IPCResponse, ascii_mode) == 3136,
              "IPCResponse::ascii_mode offset changed");
static_assert(offsetof(IPCResponse, composing) == 3140,
              "IPCResponse::composing offset changed");
static_assert(offsetof(IPCResponse, ime_status) == 3144,
              "IPCResponse::ime_status offset changed");
static_assert(offsetof(IPCResponse, page_current) == 3160,
              "IPCResponse::page_current offset changed");
static_assert(offsetof(IPCResponse, key_handled) == 3168,
              "IPCResponse::key_handled offset changed");
static_assert(sizeof(IPCResponse) >= IPC_RESPONSE_BASELINE_SIZE,
              "IPCResponse dropped fields from the 0.4.0 baseline");
static_assert(offsetof(IPCResponse, candidate_revision) == IPC_RESPONSE_BASELINE_SIZE,
              "IPCResponse 0.5 extension moved into the 0.4 baseline");
static_assert(offsetof(IPCResponse, focused_preedit_start_bytes) ==
                  IPC_RESPONSE_BASELINE_SIZE + 12,
              "IPCResponse focused preedit extension moved");
static_assert(offsetof(IPCResponse, focused_preedit_end_bytes) ==
                  IPC_RESPONSE_BASELINE_SIZE + 16,
              "IPCResponse focused preedit end extension moved");
static_assert(offsetof(IPCResponse, preedit_presentation_flags) ==
                  IPC_RESPONSE_BASELINE_SIZE + 20,
              "IPCResponse preedit presentation flags extension moved");
static_assert(offsetof(IPCResponse, candidate_known_count) ==
                IPC_RESPONSE_BASELINE_SIZE + 24,
              "IPCResponse candidate known count extension moved");
static_assert(offsetof(IPCResponse, candidate_extent_state) ==
                IPC_RESPONSE_BASELINE_SIZE + 28,
              "IPCResponse candidate extent extension moved");
static_assert(offsetof(IPCResponse, candidate_extent_complete) ==
                  IPC_RESPONSE_BASELINE_SIZE + 32,
              "IPCResponse candidate extent completeness extension moved");
static_assert(offsetof(IPCResponse, candidate_comments) == IPC_RESPONSE_BASELINE_SIZE + 40,
              "IPCResponse candidate comments extension moved");

} // namespace cxxime

#endif // CXXIME_IPC_PROTOCOL_H_
