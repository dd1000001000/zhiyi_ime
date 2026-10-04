// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/ui_protocol.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace cxxime {
namespace {

template <typename Payload>
bool build_packet(UiPacketType type, const Payload& payload, std::uint64_t sequence,
                  std::vector<std::uint8_t>* packet) {
    if (!packet || sizeof(UiPacketHeader) + sizeof(Payload) > kUiMaxPacketSize) {
        return false;
    }

    UiPacketHeader header;
    header.packet_type = type;
    header.payload_size = static_cast<std::uint32_t>(sizeof(Payload));
    header.sequence = sequence;
    packet->resize(sizeof(header) + sizeof(payload));
    std::memcpy(packet->data(), &header, sizeof(header));
    std::memcpy(packet->data() + sizeof(header), &payload, sizeof(payload));
    return true;
}

bool valid_ownership(UiOwnership ownership) {
    return ownership == UiOwnership::kNone || ownership == UiOwnership::kExternal ||
           ownership == UiOwnership::kHost;
}

bool valid_candidate_extent_state(CandidateExtentState state) {
    return state == CandidateExtentState::kExhausted ||
           state == CandidateExtentState::kHasMore ||
           state == CandidateExtentState::kIndeterminate;
}

constexpr std::uint32_t kKnownSnapshotFlags =
    ui_snapshot_flag(UiSnapshotFlag::kComposing) |
    ui_snapshot_flag(UiSnapshotFlag::kCandidateVisible) |
    ui_snapshot_flag(UiSnapshotFlag::kStatusVisible) |
    ui_snapshot_flag(UiSnapshotFlag::kHasCaret) |
    ui_snapshot_flag(UiSnapshotFlag::kHasPreedit) |
    ui_snapshot_flag(UiSnapshotFlag::kHasCandidates) |
    ui_snapshot_flag(UiSnapshotFlag::kSessionEnded) |
    ui_snapshot_flag(UiSnapshotFlag::kImmersiveMode) |
    ui_snapshot_flag(UiSnapshotFlag::kTsfLocalCandidate);

bool valid_command_type(UiCommandType type) {
    return type >= UiCommandType::kSelectCandidate &&
           type <= UiCommandType::kRefreshInputIndicator;
}

bool is_valid_utf8(const char* value, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const unsigned char lead = static_cast<unsigned char>(value[offset]);
        std::size_t sequence_length = 0;
        std::uint32_t code_point = 0;
        if (lead <= 0x7f) {
            sequence_length = 1;
            code_point = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            sequence_length = 2;
            code_point = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            sequence_length = 3;
            code_point = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            sequence_length = 4;
            code_point = lead & 0x07;
        } else {
            return false;
        }
        if (offset + sequence_length > length) {
            return false;
        }
        for (std::size_t index = 1; index < sequence_length; ++index) {
            const unsigned char continuation = static_cast<unsigned char>(value[offset + index]);
            if ((continuation & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((sequence_length == 3 && code_point < 0x800) ||
            (sequence_length == 4 && code_point < 0x10000) ||
            (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) {
            return false;
        }
        offset += sequence_length;
    }
    return true;
}

bool is_utf8_boundary(const char* value, std::size_t length, std::size_t offset) {
    return offset <= length && (offset == 0 || offset == length ||
                                (static_cast<unsigned char>(value[offset]) & 0xc0) != 0x80);
}

template <typename Payload>
UiPacketParseResult decode_packet(const void* data, std::size_t size, UiPacketType type,
                                  std::size_t baseline_size, Payload* payload) {
    if (!data || !payload || size < sizeof(UiPacketHeader)) {
        return UiPacketParseResult::kInvalid;
    }

    UiPacketHeader header = {};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != UI_PROTOCOL_MAGIC ||
        header.protocol_version < UI_PROTOCOL_MIN_COMPATIBLE_VERSION ||
        header.payload_size > size - sizeof(header) ||
        size != sizeof(header) + header.payload_size) {
        return UiPacketParseResult::kInvalid;
    }
    if (header.packet_type != type) {
        if (header.packet_type == UiPacketType::kSnapshot ||
            header.packet_type == UiPacketType::kCommand) {
            return UiPacketParseResult::kInvalid;
        }
        return UiPacketParseResult::kIgnored;
    }
    if (header.payload_size < baseline_size) {
        return UiPacketParseResult::kInvalid;
    }
    *payload = {};
    const std::size_t payload_size =
        (std::min)(static_cast<std::size_t>(header.payload_size), sizeof(*payload));
    std::memcpy(payload, static_cast<const std::uint8_t*>(data) + sizeof(header), payload_size);
    return UiPacketParseResult::kAccepted;
}

} // namespace

bool build_ui_snapshot_packet(const UiPresentationSnapshot& snapshot, std::uint64_t sequence,
                              std::vector<std::uint8_t>* packet) {
    return is_valid_ui_snapshot(snapshot) &&
           build_packet(UiPacketType::kSnapshot, snapshot, sequence, packet);
}

UiPacketParseResult decode_ui_snapshot_packet(const void* data, std::size_t size,
                                              UiPresentationSnapshot* snapshot) {
    if (!snapshot) {
        return UiPacketParseResult::kInvalid;
    }
    UiPresentationSnapshot parsed;
    const UiPacketParseResult result =
        decode_packet(data, size, UiPacketType::kSnapshot, UI_SNAPSHOT_BASELINE_SIZE, &parsed);
    if (result != UiPacketParseResult::kAccepted) {
        return result;
    }
    UiPacketHeader header = {};
    std::memcpy(&header, data, sizeof(header));
    constexpr std::size_t kCandidateExtentPayloadSize =
        offsetof(UiPresentationSnapshot, candidate_extent_complete) + sizeof(std::uint32_t);
    if (header.payload_size < kCandidateExtentPayloadSize) {
        const std::uint64_t returned_end =
            static_cast<std::uint64_t>(parsed.candidate_page.offset) +
            parsed.candidate_page.count;
        if (returned_end > (std::numeric_limits<std::uint32_t>::max)()) {
            return UiPacketParseResult::kInvalid;
        }
        parsed.candidate_known_count = (std::max)(
            parsed.candidate_page.total, static_cast<std::uint32_t>(returned_end));
        parsed.candidate_extent_state = parsed.candidate_page.total > returned_end
                                            ? CandidateExtentState::kHasMore
                                            : CandidateExtentState::kExhausted;
        parsed.candidate_extent_complete = 1;
    }
    constexpr std::size_t kLocalCandidateWindowPayloadSize =
        offsetof(UiPresentationSnapshot, local_candidate_window) + sizeof(std::uint64_t);
    if (header.payload_size < kLocalCandidateWindowPayloadSize) {
        parsed.local_candidate_window = 0;
    }
    constexpr std::size_t kRecommendedPayloadSize =
        offsetof(UiPresentationSnapshot, candidate_recommended_mask) + sizeof(std::uint32_t);
    if (header.payload_size < kRecommendedPayloadSize) {
        parsed.candidate_recommended_mask = 0;
    }
    constexpr std::size_t kCommentsPayloadSize =
        offsetof(UiPresentationSnapshot, candidate_comments) +
        sizeof(UiPresentationSnapshot::candidate_comments);
    if (header.payload_size < kCommentsPayloadSize) {
        for (auto& comment : parsed.candidate_comments) comment = {};
    }
    parsed.candidate_recommended_mask &=
        parsed.candidate_page.count >= 32 ? ~0u : ((1u << parsed.candidate_page.count) - 1u);
    if ((parsed.flags & ~kKnownSnapshotFlags) != 0 || !valid_ownership(parsed.ownership)) {
        return UiPacketParseResult::kIgnored;
    }
    if (!is_valid_ui_snapshot(parsed)) {
        return UiPacketParseResult::kInvalid;
    }
    *snapshot = parsed;
    return UiPacketParseResult::kAccepted;
}

bool parse_ui_snapshot_packet(const void* data, std::size_t size,
                              UiPresentationSnapshot* snapshot) {
    return decode_ui_snapshot_packet(data, size, snapshot) == UiPacketParseResult::kAccepted;
}

bool build_ui_command_packet(const UiCommand& command, std::uint64_t sequence,
                             std::vector<std::uint8_t>* packet) {
    if (!is_valid_ui_command(command)) {
        return false;
    }
    UiCommand normalized = {};
    normalized.session_id = command.session_id;
    normalized.session_generation = command.session_generation;
    normalized.target_generation = command.target_generation;
    normalized.composition_generation = command.composition_generation;
    normalized.presentation_generation = command.presentation_generation;
    normalized.type = command.type;
    normalized.candidate_index = command.candidate_index;
    normalized.value = command.value;
    normalized.candidate_revision = command.candidate_revision;
    return build_packet(UiPacketType::kCommand, normalized, sequence, packet);
}

UiPacketParseResult decode_ui_command_packet(const void* data, std::size_t size,
                                             UiCommand* command) {
    if (!command) {
        return UiPacketParseResult::kInvalid;
    }
    UiCommand parsed;
    const UiPacketParseResult result =
        decode_packet(data, size, UiPacketType::kCommand, UI_COMMAND_BASELINE_SIZE, &parsed);
    if (result != UiPacketParseResult::kAccepted) {
        return result;
    }
    if (parsed.type > UiCommandType::kRefreshInputIndicator) {
        return UiPacketParseResult::kIgnored;
    }
    if (!is_valid_ui_command(parsed)) {
        return UiPacketParseResult::kInvalid;
    }
    *command = parsed;
    return UiPacketParseResult::kAccepted;
}

bool parse_ui_command_packet(const void* data, std::size_t size, UiCommand* command) {
    return decode_ui_command_packet(data, size, command) == UiPacketParseResult::kAccepted;
}

bool is_valid_ui_snapshot(const UiPresentationSnapshot& snapshot) {
    constexpr std::uint32_t kKnownPreeditPresentationFlags =
        preedit_presentation_flag(PreeditPresentationFlag::SyllableBoundaries);
    if (snapshot.session_id == 0 || snapshot.session_generation == 0 ||
        snapshot.presentation_generation == 0 ||
        (snapshot.flags & ~kKnownSnapshotFlags) != 0 || !valid_ownership(snapshot.ownership) ||
        snapshot.preedit_length > static_cast<std::uint32_t>(kUiPreeditCapacity) ||
        snapshot.preedit_cursor > snapshot.preedit_length ||
        snapshot.converted_prefix_bytes > snapshot.preedit_cursor ||
        snapshot.converted_prefix_bytes > snapshot.preedit_length ||
        snapshot.focused_preedit_start_bytes > snapshot.focused_preedit_end_bytes ||
        snapshot.focused_preedit_start_bytes > snapshot.preedit_length ||
        snapshot.focused_preedit_end_bytes > snapshot.preedit_length ||
        ((snapshot.focused_preedit_start_bytes != 0 ||
          snapshot.focused_preedit_end_bytes != 0) &&
         snapshot.focused_preedit_start_bytes < snapshot.converted_prefix_bytes) ||
        (snapshot.preedit_presentation_flags & ~kKnownPreeditPresentationFlags) != 0 ||
        (snapshot.preedit_presentation_flags != 0 && snapshot.preedit_length == 0) ||
        !is_valid_utf8(snapshot.preedit, snapshot.preedit_length) ||
        !is_utf8_boundary(snapshot.preedit, snapshot.preedit_length,
                          snapshot.preedit_cursor) ||
        !is_utf8_boundary(snapshot.preedit, snapshot.preedit_length,
                          snapshot.converted_prefix_bytes) ||
        !is_utf8_boundary(snapshot.preedit, snapshot.preedit_length,
                          snapshot.focused_preedit_start_bytes) ||
        !is_utf8_boundary(snapshot.preedit, snapshot.preedit_length,
                          snapshot.focused_preedit_end_bytes) ||
        snapshot.candidate_page.count > static_cast<std::uint32_t>(kCandidateCapacity) ||
        !valid_candidate_extent_state(snapshot.candidate_extent_state) ||
        snapshot.candidate_extent_complete > 1 ||
        static_cast<std::uint64_t>(snapshot.candidate_known_count) <
            static_cast<std::uint64_t>(snapshot.candidate_page.offset) +
                snapshot.candidate_page.count) {
        return false;
    }
    if (snapshot.candidate_page.count != 0 &&
        snapshot.candidate_page.highlighted >= snapshot.candidate_page.count) {
        return false;
    }
    for (std::uint32_t index = 0; index < snapshot.candidate_page.count; ++index) {
        const UiCandidate& candidate = snapshot.candidate_page.candidates[index];
        if (candidate.text_length == 0 ||
            candidate.text_length > static_cast<std::uint32_t>(kCandidateTextCapacity) ||
            candidate.hint_length > static_cast<std::uint32_t>(kUiCandidateHintCapacity) ||
            !is_valid_utf8(candidate.text, candidate.text_length) ||
            !is_valid_utf8(candidate.hint, candidate.hint_length)) {
            return false;
        }
        const UiCandidateComment& comment = snapshot.candidate_comments[index];
        if (comment.length > static_cast<std::uint32_t>(kCandidateCommentCapacity) ||
            !is_valid_utf8(comment.text, comment.length)) {
            return false;
        }
    }
    return true;
}

bool is_valid_ui_command(const UiCommand& command) {
    if (command.session_id == 0 || command.session_generation == 0 ||
        !valid_command_type(command.type)) {
        return false;
    }
    if (command.type != UiCommandType::kRefreshInputIndicator &&
        command.presentation_generation == 0) {
        return false;
    }
    if (command.type == UiCommandType::kSelectCandidate &&
        command.candidate_index >= static_cast<std::uint32_t>(kCandidateCapacity)) {
        return false;
    }
    if (command.type == UiCommandType::kSwitchInputMode &&
        command.value > static_cast<std::uint32_t>(InputMode::MIXED)) {
        return false;
    }
    if (command.type == UiCommandType::kRefreshInputIndicator) {
        return command.target_generation == 0 && command.composition_generation == 0 &&
               command.presentation_generation == 0 && command.candidate_index == 0 &&
               command.value == 0;
    }
    return true;
}

} // namespace cxxime
