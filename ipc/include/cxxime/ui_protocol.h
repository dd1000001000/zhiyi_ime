// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_UI_PROTOCOL_H_
#define CXXIME_UI_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <windows.h>

#include <cxxime/input_limits.h>
#include <cxxime/ipc_protocol.h>

namespace cxxime {

constexpr std::uint32_t UI_PROTOCOL_MAGIC = 0x43584955; // CXIU
constexpr std::uint16_t UI_PROTOCOL_MIN_COMPATIBLE_VERSION = 1;
constexpr std::uint16_t UI_PROTOCOL_VERSION = 1;
constexpr std::size_t UI_SNAPSHOT_BASELINE_SIZE = 3056;
constexpr std::size_t UI_COMMAND_BASELINE_SIZE = 56;

// UI payload prefixes are frozen at the framed 0.4.0 baseline and are append-only.
constexpr std::size_t kUiPreeditCapacity = 256;
constexpr std::size_t kUiCandidateHintCapacity = 4;
constexpr std::size_t kUiMaxPacketSize = 64 * 1024;

enum class UiPacketType : std::uint16_t {
    kSnapshot = 1,
    kCommand = 2,
};

enum class UiPacketParseResult : std::uint8_t {
    kAccepted,
    kIgnored,
    kInvalid,
};

enum class UiOwnership : std::uint32_t {
    kNone = 0,
    kExternal = 1, // CxxIME presents UI (server or TSF local window).
    kHost = 2,     // The application consumes the TSF UI element.
};

enum class UiSnapshotFlag : std::uint32_t {
    kComposing = 1u << 0,
    kCandidateVisible = 1u << 1,
    kStatusVisible = 1u << 2,
    kHasCaret = 1u << 3,
    kHasPreedit = 1u << 4,
    kHasCandidates = 1u << 5,
    kSessionEnded = 1u << 6,
    // Immersive hosts can reorder owned popups after creating their edit UI.
    kImmersiveMode = 1u << 7,
    kTsfLocalCandidate = 1u << 8,
};

constexpr std::uint32_t ui_snapshot_flag(UiSnapshotFlag flag) noexcept {
    return static_cast<std::uint32_t>(flag);
}

struct UiCandidate {
    std::uint32_t text_length = 0;
    std::uint32_t hint_length = 0;
    char text[kCandidateTextCapacity] = {};
    char hint[kUiCandidateHintCapacity] = {};
};

struct UiCandidatePage {
    std::uint32_t count = 0;
    std::uint32_t offset = 0;
    std::uint32_t total = 0;
    std::uint32_t highlighted = 0; // Index within candidates, not the complete result set.
    std::uint32_t page_current = 1;
    std::uint32_t page_total = 1;
    UiCandidate candidates[kCandidateCapacity] = {};
};

struct UiPresentationSnapshot {
    std::uint64_t session_id = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t target_generation = 0;
    std::uint64_t composition_generation = 0;
    std::uint64_t presentation_generation = 0;
    // TSF view window used for coordinate conversion and candidate owner identity.
    std::uint64_t target_window = 0;
    std::uint32_t flags = 0;
    UiOwnership ownership = UiOwnership::kNone;
    ImeStatus ime_status;
    RECT caret = {};
    std::uint32_t preedit_cursor = 0;
    std::uint32_t preedit_length = 0;
    char preedit[kUiPreeditCapacity] = {};
    UiCandidatePage candidate_page;
    // Fields after candidate_page form the 0.5 append-only extension.
    std::uint64_t candidate_revision = 0;
    std::uint32_t converted_prefix_bytes = 0;
    std::uint32_t focused_preedit_start_bytes = 0;
    std::uint32_t focused_preedit_end_bytes = 0;
    std::uint32_t preedit_presentation_flags = 0;
    std::uint32_t candidate_known_count = 0;
    CandidateExtentState candidate_extent_state = CandidateExtentState::kExhausted;
    std::uint32_t candidate_extent_complete = 1;
    // Optional native window for z-order coordination when kTsfLocalCandidate is set.
    // Append after the complete 0.6.1 payload, including its tail padding.
    std::uint64_t local_candidate_window = 0;
    // Bit i: candidate_page.candidates[i] is the Laya recommendation (sparkle mark).
    std::uint32_t candidate_recommended_mask = 0;
};

enum class UiCommandType : std::uint32_t {
    kNone = 0,
    kSelectCandidate = 1,
    kPagePrevious = 2,
    kPageNext = 3,
    kToggleChinese = 4,
    kToggleShape = 5,
    kTogglePunct = 6,
    kOpenSettings = 7,
    kToggleStatusWindow = 8,
    kCommitComposition = 9,
    kCancelComposition = 10,
    kSwitchInputMode = 11,
    kOpenDictionary = 12,
    kOpenAbout = 13,
    kMenuCommand = 14,
    kRefreshInputIndicator = 15,
};

struct UiCommand {
    std::uint64_t session_id = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t target_generation = 0;
    std::uint64_t composition_generation = 0;
    std::uint64_t presentation_generation = 0;
    UiCommandType type = UiCommandType::kNone;
    std::uint32_t candidate_index = 0;
    std::uint32_t value = 0;
    // Fields after value form the 0.5 append-only extension.
    std::uint64_t candidate_revision = 0;
};

#pragma pack(push, 1)
struct UiPacketHeader {
    std::uint32_t magic = UI_PROTOCOL_MAGIC;
    std::uint16_t protocol_version = UI_PROTOCOL_VERSION;
    UiPacketType packet_type = UiPacketType::kSnapshot;
    std::uint32_t payload_size = 0;
    std::uint64_t sequence = 0;
};
#pragma pack(pop)

static_assert(std::is_standard_layout<UiPresentationSnapshot>::value,
              "UiPresentationSnapshot must use standard layout");
static_assert(std::is_trivially_copyable<UiPresentationSnapshot>::value,
              "UiPresentationSnapshot must remain trivially copyable");
static_assert(std::is_standard_layout<UiCommand>::value, "UiCommand must use standard layout");
static_assert(std::is_trivially_copyable<UiCommand>::value,
              "UiCommand must remain trivially copyable");
static_assert(sizeof(UiCandidate) == 268, "UiCandidate layout changed");
static_assert(sizeof(UiCandidatePage) == 2704, "UiCandidatePage layout changed");
static_assert(alignof(UiPresentationSnapshot) == 8, "UiPresentationSnapshot alignment changed");
static_assert(offsetof(UiPresentationSnapshot, target_window) == 40,
              "UiPresentationSnapshot::target_window offset changed");
static_assert(offsetof(UiPresentationSnapshot, ime_status) == 56,
              "UiPresentationSnapshot::ime_status offset changed");
static_assert(offsetof(UiPresentationSnapshot, preedit) == 96,
              "UiPresentationSnapshot::preedit offset changed");
static_assert(offsetof(UiPresentationSnapshot, candidate_page) == 352,
              "UiPresentationSnapshot::candidate_page offset changed");
static_assert(offsetof(UiPresentationSnapshot, candidate_revision) == UI_SNAPSHOT_BASELINE_SIZE,
              "UiPresentationSnapshot 0.5 extension moved into the 0.4 baseline");
static_assert(offsetof(UiPresentationSnapshot, focused_preedit_start_bytes) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 12,
              "UiPresentationSnapshot focused preedit extension moved");
static_assert(offsetof(UiPresentationSnapshot, focused_preedit_end_bytes) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 16,
              "UiPresentationSnapshot focused preedit end extension moved");
static_assert(offsetof(UiPresentationSnapshot, preedit_presentation_flags) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 20,
              "UiPresentationSnapshot preedit presentation flags extension moved");
static_assert(offsetof(UiPresentationSnapshot, candidate_known_count) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 24,
              "UiPresentationSnapshot candidate known count extension moved");
static_assert(offsetof(UiPresentationSnapshot, candidate_extent_state) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 28,
              "UiPresentationSnapshot candidate extent extension moved");
static_assert(offsetof(UiPresentationSnapshot, candidate_extent_complete) ==
                  UI_SNAPSHOT_BASELINE_SIZE + 32,
              "UiPresentationSnapshot candidate extent completeness extension moved");
static_assert(sizeof(UiPresentationSnapshot) >= UI_SNAPSHOT_BASELINE_SIZE,
              "UiPresentationSnapshot cannot shrink below the 0.4.0 baseline");
static_assert(offsetof(UiPresentationSnapshot, local_candidate_window) == 3096,
              "Local candidate window must follow the complete 0.6.0 payload");
static_assert(alignof(UiCommand) == 8, "UiCommand alignment changed");
static_assert(offsetof(UiCommand, type) == 40, "UiCommand::type offset changed");
static_assert(offsetof(UiCommand, value) == 48, "UiCommand::value offset changed");
static_assert(offsetof(UiCommand, candidate_revision) == UI_COMMAND_BASELINE_SIZE,
              "UiCommand 0.5 extension moved into the 0.4 baseline");
static_assert(sizeof(UiCommand) >= UI_COMMAND_BASELINE_SIZE,
              "UiCommand cannot shrink below the 0.4.0 baseline");
static_assert(offsetof(UiPacketHeader, sequence) == 12, "UiPacketHeader::sequence offset changed");
static_assert(sizeof(UiPacketHeader) == 20, "UiPacketHeader layout changed");

constexpr std::size_t kUiSnapshotPacketSize =
    sizeof(UiPacketHeader) + sizeof(UiPresentationSnapshot);
constexpr std::size_t kUiCommandPacketSize = sizeof(UiPacketHeader) + sizeof(UiCommand);
static_assert(kUiSnapshotPacketSize <= kUiMaxPacketSize, "UI snapshot packet exceeds pipe limit");
static_assert(kUiCommandPacketSize <= kUiMaxPacketSize, "UI command packet exceeds pipe limit");

bool build_ui_snapshot_packet(const UiPresentationSnapshot& snapshot, std::uint64_t sequence,
                              std::vector<std::uint8_t>* packet);
UiPacketParseResult decode_ui_snapshot_packet(const void* data, std::size_t size,
                                              UiPresentationSnapshot* snapshot);
bool parse_ui_snapshot_packet(const void* data, std::size_t size, UiPresentationSnapshot* snapshot);
bool build_ui_command_packet(const UiCommand& command, std::uint64_t sequence,
                             std::vector<std::uint8_t>* packet);
UiPacketParseResult decode_ui_command_packet(const void* data, std::size_t size,
                                             UiCommand* command);
bool parse_ui_command_packet(const void* data, std::size_t size, UiCommand* command);
bool is_valid_ui_snapshot(const UiPresentationSnapshot& snapshot);
bool is_valid_ui_command(const UiCommand& command);

} // namespace cxxime

#endif // CXXIME_UI_PROTOCOL_H_
