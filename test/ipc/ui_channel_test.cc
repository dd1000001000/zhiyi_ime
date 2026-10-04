// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <cxxime/ui_channel.h>
#include <cxxime/ui_protocol.h>

#include "support/testutil.h"

namespace {

bool wait_for(const std::function<bool()>& condition, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::wstring test_pipe_name() {
    static std::atomic<unsigned long> sequence{0};
    return L"\\\\.\\pipe\\ZhiyiIME-UI-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(sequence.fetch_add(1));
}

HANDLE connect_test_pipe(const std::wstring& pipe_name, int timeout_ms = 3000) {
    const std::wstring scoped_name = cxxime::make_user_pipe_name(pipe_name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (WaitNamedPipeW(scoped_name.c_str(), 50)) {
            HANDLE pipe = CreateFileW(scoped_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_MESSAGE;
                if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                    return pipe;
                }
                CloseHandle(pipe);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return INVALID_HANDLE_VALUE;
}

cxxime::UiPresentationSnapshot make_snapshot(std::uint64_t generation) {
    cxxime::UiPresentationSnapshot snapshot;
    snapshot.session_id = 17;
    snapshot.session_generation = 3;
    snapshot.target_generation = generation;
    snapshot.composition_generation = generation + 10;
    snapshot.presentation_generation = generation + 20;
    snapshot.ownership = cxxime::UiOwnership::kExternal;
    snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kComposing) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasPreedit) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kImmersiveMode) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate);
    snapshot.caret = {100, 120, 102, 144};
    std::memcpy(snapshot.preedit, "ni", 2);
    snapshot.preedit_length = 2;
    snapshot.preedit_cursor = 2;
    snapshot.converted_prefix_bytes = 1;
    snapshot.focused_preedit_start_bytes = 1;
    snapshot.focused_preedit_end_bytes = 2;
    snapshot.preedit_presentation_flags = cxxime::preedit_presentation_flag(
        cxxime::PreeditPresentationFlag::SyllableBoundaries);
    snapshot.candidate_revision = generation + 30;
    snapshot.local_candidate_window = 0x12345678;
    snapshot.candidate_page.count = 1;
    snapshot.candidate_page.total = 1;
    snapshot.candidate_known_count = 1;
    snapshot.candidate_extent_state = cxxime::CandidateExtentState::kExhausted;
    snapshot.candidate_extent_complete = 1;
    snapshot.candidate_page.candidates[0].text_length = 9;
    std::memcpy(snapshot.candidate_page.candidates[0].text, "candidate", 9);
    return snapshot;
}

} // namespace

TEST(UiChannel, protocol_round_trip) {
    const cxxime::UiPresentationSnapshot expected = make_snapshot(41);
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(expected, 9, &packet));
    ASSERT_EQ(packet.size(), sizeof(cxxime::UiPacketHeader) + sizeof(expected));

    cxxime::UiPresentationSnapshot actual;
    ASSERT_TRUE(cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &actual));
    ASSERT_EQ(actual.session_id, expected.session_id);
    ASSERT_EQ(actual.target_generation, expected.target_generation);
    ASSERT_EQ(actual.flags, expected.flags);
    ASSERT_EQ(actual.preedit_length, static_cast<std::uint32_t>(2));
    ASSERT_EQ(actual.converted_prefix_bytes, static_cast<std::uint32_t>(1));
    ASSERT_EQ(actual.focused_preedit_start_bytes, static_cast<std::uint32_t>(1));
    ASSERT_EQ(actual.focused_preedit_end_bytes, static_cast<std::uint32_t>(2));
    ASSERT_EQ(actual.preedit_presentation_flags,
              cxxime::preedit_presentation_flag(
                  cxxime::PreeditPresentationFlag::SyllableBoundaries));
    ASSERT_EQ(actual.candidate_revision, static_cast<std::uint64_t>(71));
    ASSERT_EQ(actual.local_candidate_window, expected.local_candidate_window);
    ASSERT_EQ(actual.candidate_page.count, static_cast<std::uint32_t>(1));
    ASSERT_EQ(actual.candidate_known_count, static_cast<std::uint32_t>(1));
    ASSERT_EQ(actual.candidate_extent_state, cxxime::CandidateExtentState::kExhausted);
    ASSERT_EQ(actual.candidate_extent_complete, static_cast<std::uint32_t>(1));

    cxxime::UiCommand expected_command;
    expected_command.session_id = expected.session_id;
    expected_command.session_generation = expected.session_generation;
    expected_command.target_generation = expected.target_generation;
    expected_command.composition_generation = expected.composition_generation;
    expected_command.presentation_generation = expected.presentation_generation;
    expected_command.type = cxxime::UiCommandType::kSelectCandidate;
    expected_command.candidate_index = 0;
    expected_command.candidate_revision = expected.candidate_revision;
    ASSERT_TRUE(cxxime::build_ui_command_packet(expected_command, 10, &packet));

    cxxime::UiCommand actual_command;
    ASSERT_TRUE(cxxime::parse_ui_command_packet(packet.data(), packet.size(), &actual_command));
    ASSERT_EQ(actual_command.type, cxxime::UiCommandType::kSelectCandidate);
    ASSERT_EQ(actual_command.composition_generation, expected.composition_generation);
    ASSERT_EQ(actual_command.presentation_generation, expected.presentation_generation);
    ASSERT_EQ(actual_command.candidate_revision, expected.candidate_revision);
}

TEST(UiChannel, protocol_defaults_extension_fields_for_a_baseline_payload) {
    std::vector<std::uint8_t> packet;
    cxxime::UiPacketHeader header = {};
    struct Case {
        std::uint32_t total;
        cxxime::CandidateExtentState extent_state;
    };
    const Case cases[] = {
        {1, cxxime::CandidateExtentState::kExhausted},
        {2, cxxime::CandidateExtentState::kHasMore},
    };
    for (const Case& item : cases) {
        cxxime::UiPresentationSnapshot expected = make_snapshot(5);
        expected.candidate_page.total = item.total;
        ASSERT_TRUE(cxxime::build_ui_snapshot_packet(expected, 1, &packet));
        std::memcpy(&header, packet.data(), sizeof(header));
        header.payload_size = static_cast<std::uint32_t>(cxxime::UI_SNAPSHOT_BASELINE_SIZE);
        packet.resize(sizeof(header) + cxxime::UI_SNAPSHOT_BASELINE_SIZE);
        std::memcpy(packet.data(), &header, sizeof(header));

        cxxime::UiPresentationSnapshot actual;
        ASSERT_TRUE(cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &actual));
        ASSERT_EQ(actual.target_generation, expected.target_generation);
        ASSERT_EQ(actual.candidate_revision, static_cast<std::uint64_t>(0));
        ASSERT_EQ(actual.local_candidate_window, static_cast<std::uint64_t>(0));
        ASSERT_EQ(actual.converted_prefix_bytes, static_cast<std::uint32_t>(0));
        ASSERT_EQ(actual.focused_preedit_start_bytes, static_cast<std::uint32_t>(0));
        ASSERT_EQ(actual.focused_preedit_end_bytes, static_cast<std::uint32_t>(0));
        ASSERT_EQ(actual.preedit_presentation_flags, static_cast<std::uint32_t>(0));
        ASSERT_EQ(actual.candidate_known_count, item.total);
        ASSERT_EQ(actual.candidate_extent_state, item.extent_state);
        ASSERT_EQ(actual.candidate_extent_complete, static_cast<std::uint32_t>(1));
    }

    cxxime::UiCommand expected_command;
    expected_command.session_id = 17;
    expected_command.session_generation = 3;
    expected_command.presentation_generation = 1;
    expected_command.type = cxxime::UiCommandType::kSelectCandidate;
    expected_command.candidate_revision = 9;
    ASSERT_TRUE(cxxime::build_ui_command_packet(expected_command, 2, &packet));
    std::memcpy(&header, packet.data(), sizeof(header));
    header.payload_size = static_cast<std::uint32_t>(cxxime::UI_COMMAND_BASELINE_SIZE);
    packet.resize(sizeof(header) + cxxime::UI_COMMAND_BASELINE_SIZE);
    std::memcpy(packet.data(), &header, sizeof(header));

    cxxime::UiCommand actual_command;
    ASSERT_TRUE(cxxime::parse_ui_command_packet(packet.data(), packet.size(), &actual_command));
    ASSERT_EQ(actual_command.type, cxxime::UiCommandType::kSelectCandidate);
    ASSERT_EQ(actual_command.candidate_revision, static_cast<std::uint64_t>(0));
}

TEST(UiChannel, local_candidate_window_requires_a_complete_extension) {
    auto expected = make_snapshot(7);
    expected.local_candidate_window = 0x123456789abcdef0ull;
    // 3096 is the frozen sizeof payload used by 0.6.1, including tail padding.
    for (std::size_t payload_size = 3096; payload_size <= 3104; ++payload_size) {
        std::vector<std::uint8_t> packet;
        ASSERT_TRUE(cxxime::build_ui_snapshot_packet(expected, 1, &packet));
        cxxime::UiPacketHeader header = {};
        std::memcpy(&header, packet.data(), sizeof(header));
        header.payload_size = static_cast<std::uint32_t>(payload_size);
        packet.resize(sizeof(header) + payload_size);
        std::memcpy(packet.data(), &header, sizeof(header));

        cxxime::UiPresentationSnapshot actual;
        ASSERT_TRUE(cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &actual));
        ASSERT_EQ(actual.candidate_revision, expected.candidate_revision);
        ASSERT_EQ(actual.candidate_extent_complete, expected.candidate_extent_complete);
        ASSERT_EQ(actual.local_candidate_window,
                  payload_size == 3104 ? expected.local_candidate_window : 0ull);
    }
}

TEST(UiChannel, protocol_ignores_a_future_tail_after_current_fields) {
    const cxxime::UiPresentationSnapshot expected = make_snapshot(6);
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(expected, 1, &packet));

    cxxime::UiPacketHeader header = {};
    std::memcpy(&header, packet.data(), sizeof(header));
    constexpr std::size_t kFutureTailSize = 16;
    header.payload_size += static_cast<std::uint32_t>(kFutureTailSize);
    packet.resize(packet.size() + kFutureTailSize, 0x5a);
    std::memcpy(packet.data(), &header, sizeof(header));

    cxxime::UiPresentationSnapshot actual;
    ASSERT_TRUE(cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &actual));
    ASSERT_EQ(actual.candidate_revision, expected.candidate_revision);
    ASSERT_EQ(actual.converted_prefix_bytes, expected.converted_prefix_bytes);
    ASSERT_EQ(actual.local_candidate_window, expected.local_candidate_window);

    cxxime::UiCommand expected_command;
    expected_command.session_id = 17;
    expected_command.session_generation = 3;
    expected_command.presentation_generation = 1;
    expected_command.type = cxxime::UiCommandType::kSelectCandidate;
    expected_command.candidate_revision = 77;
    ASSERT_TRUE(cxxime::build_ui_command_packet(expected_command, 2, &packet));
    std::memcpy(&header, packet.data(), sizeof(header));
    header.payload_size += static_cast<std::uint32_t>(kFutureTailSize);
    packet.resize(packet.size() + kFutureTailSize, 0x5a);
    std::memcpy(packet.data(), &header, sizeof(header));

    cxxime::UiCommand actual_command;
    ASSERT_TRUE(cxxime::parse_ui_command_packet(packet.data(), packet.size(), &actual_command));
    ASSERT_EQ(actual_command.candidate_revision, expected_command.candidate_revision);
}

TEST(UiChannel, protocol_rejects_invalid_payloads) {
    cxxime::UiPresentationSnapshot snapshot = make_snapshot(1);
    snapshot.candidate_page.count = static_cast<std::uint32_t>(cxxime::kCandidateCapacity + 1);
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    snapshot.preedit_cursor = snapshot.preedit_length + 1;
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    snapshot.preedit_cursor = 0;
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    snapshot.candidate_extent_state = static_cast<cxxime::CandidateExtentState>(99);
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    snapshot.candidate_extent_complete = 2;
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    const std::string multibyte_preedit = u8"你a";
    std::memcpy(snapshot.preedit, multibyte_preedit.data(), multibyte_preedit.size());
    snapshot.preedit_length = static_cast<std::uint32_t>(multibyte_preedit.size());
    snapshot.preedit_cursor = snapshot.preedit_length;
    snapshot.converted_prefix_bytes = 1;
    ASSERT_TRUE(!cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));

    snapshot = make_snapshot(1);
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(snapshot, 1, &packet));
    cxxime::UiPacketHeader header = {};
    std::memcpy(&header, packet.data(), sizeof(header));
    header.protocol_version++;
    header.payload_size += 4;
    packet.resize(packet.size() + 4, 0);
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &snapshot));
    header.protocol_version = 0;
    std::memcpy(packet.data(), &header, sizeof(header));
    ASSERT_TRUE(!cxxime::parse_ui_snapshot_packet(packet.data(), packet.size(), &snapshot));

    cxxime::UiCommand command;
    command.session_id = 17;
    command.session_generation = 3;
    command.presentation_generation = 1;
    command.type = cxxime::UiCommandType::kSelectCandidate;
    command.candidate_index = cxxime::kCandidateCapacity;
    ASSERT_TRUE(!cxxime::build_ui_command_packet(command, 1, &packet));

    command.type = cxxime::UiCommandType::kRefreshInputIndicator;
    command.candidate_index = 0;
    command.target_generation = 1;
    command.value = 0;
    ASSERT_TRUE(!cxxime::build_ui_command_packet(command, 1, &packet));
    command.target_generation = 0;
    command.presentation_generation = 0;
    ASSERT_TRUE(cxxime::build_ui_command_packet(command, 1, &packet));
    cxxime::UiCommand parsed;
    ASSERT_TRUE(cxxime::parse_ui_command_packet(packet.data(), packet.size(), &parsed));
    ASSERT_EQ(parsed.type, cxxime::UiCommandType::kRefreshInputIndicator);
    command.value = 1;
    ASSERT_TRUE(!cxxime::build_ui_command_packet(command, 1, &packet));
}

TEST(UiChannel, protocol_ignores_unknown_compatible_extensions) {
    cxxime::UiCommand command;
    command.session_id = 17;
    command.session_generation = 3;
    command.presentation_generation = 1;
    command.type = cxxime::UiCommandType::kPageNext;
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_ui_command_packet(command, 1, &packet));
    command.type = static_cast<cxxime::UiCommandType>(0xffff);
    std::memcpy(packet.data() + sizeof(cxxime::UiPacketHeader), &command, sizeof(command));
    ASSERT_EQ(cxxime::decode_ui_command_packet(packet.data(), packet.size(), &command),
              cxxime::UiPacketParseResult::kIgnored);

    cxxime::UiPresentationSnapshot snapshot = make_snapshot(1);
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(snapshot, 2, &packet));
    snapshot.flags |= 1u << 31;
    std::memcpy(packet.data() + sizeof(cxxime::UiPacketHeader), &snapshot, sizeof(snapshot));
    ASSERT_EQ(cxxime::decode_ui_snapshot_packet(packet.data(), packet.size(), &snapshot),
              cxxime::UiPacketParseResult::kIgnored);

    snapshot = make_snapshot(1);
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(snapshot, 3, &packet));
    snapshot.ownership = static_cast<cxxime::UiOwnership>(0xffff);
    std::memcpy(packet.data() + sizeof(cxxime::UiPacketHeader), &snapshot, sizeof(snapshot));
    ASSERT_EQ(cxxime::decode_ui_snapshot_packet(packet.data(), packet.size(), &snapshot),
              cxxime::UiPacketParseResult::kIgnored);
}

TEST(UiChannel, server_keeps_connection_after_unknown_packet) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> snapshot_count{0};
    cxxime::UiChannelServer server;
    ASSERT_TRUE(
        server.start([&](cxxime::UiEndpointId,
                         const cxxime::UiPresentationSnapshot&) { snapshot_count.fetch_add(1); },
        {}, pipe_name));

    HANDLE pipe = connect_test_pipe(pipe_name);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);
    ASSERT_TRUE(wait_for([&]() { return server.endpoint_count() == 1; }));

    cxxime::UiPacketHeader unknown_header;
    unknown_header.packet_type = static_cast<cxxime::UiPacketType>(0xffff);
    DWORD transferred = 0;
    ASSERT_TRUE(WriteFile(pipe, &unknown_header, sizeof(unknown_header), &transferred, nullptr));
    ASSERT_EQ(transferred, static_cast<DWORD>(sizeof(unknown_header)));

    const cxxime::UiPresentationSnapshot snapshot = make_snapshot(9);
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_ui_snapshot_packet(snapshot, 2, &packet));
    ASSERT_TRUE(
        WriteFile(pipe, packet.data(), static_cast<DWORD>(packet.size()), &transferred, nullptr));
    ASSERT_EQ(transferred, static_cast<DWORD>(packet.size()));
    ASSERT_TRUE(wait_for([&]() { return snapshot_count.load() == 1; }));
    ASSERT_EQ(server.endpoint_count(), static_cast<std::size_t>(1));

    CloseHandle(pipe);
    server.stop();
}

TEST(UiChannel, client_keeps_connection_after_unknown_command) {
    const std::wstring pipe_name = cxxime::make_user_pipe_name(test_pipe_name());
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                   static_cast<DWORD>(cxxime::kUiMaxPacketSize),
                                   static_cast<DWORD>(cxxime::kUiMaxPacketSize), 0, nullptr);
    ASSERT_TRUE(pipe != INVALID_HANDLE_VALUE);

    std::atomic<int> command_count{0};
    cxxime::UiCommand received;
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start(
        [&](const cxxime::UiCommand& command) {
            received = command;
            command_count.fetch_add(1);
        },
        pipe_name));
    ASSERT_TRUE(ConnectNamedPipe(pipe, nullptr) != FALSE ||
                GetLastError() == ERROR_PIPE_CONNECTED);
    ASSERT_TRUE(wait_for([&]() { return client.is_connected(); }));

    cxxime::UiCommand command;
    command.session_id = 17;
    command.session_generation = 3;
    command.presentation_generation = 1;
    command.type = cxxime::UiCommandType::kPageNext;
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(cxxime::build_ui_command_packet(command, 1, &packet));
    command.type = static_cast<cxxime::UiCommandType>(0xffff);
    std::memcpy(packet.data() + sizeof(cxxime::UiPacketHeader), &command, sizeof(command));
    DWORD transferred = 0;
    ASSERT_TRUE(
        WriteFile(pipe, packet.data(), static_cast<DWORD>(packet.size()), &transferred, nullptr));
    ASSERT_EQ(transferred, static_cast<DWORD>(packet.size()));

    command.type = cxxime::UiCommandType::kPageNext;
    ASSERT_TRUE(cxxime::build_ui_command_packet(command, 2, &packet));
    ASSERT_TRUE(
        WriteFile(pipe, packet.data(), static_cast<DWORD>(packet.size()), &transferred, nullptr));
    ASSERT_EQ(transferred, static_cast<DWORD>(packet.size()));
    ASSERT_TRUE(wait_for([&]() { return command_count.load() == 1; }));
    ASSERT_EQ(received.type, cxxime::UiCommandType::kPageNext);
    ASSERT_TRUE(client.is_connected());

    client.stop();
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

TEST(UiChannel, queued_snapshots_coalesce_before_connect) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    for (std::uint64_t generation = 1; generation <= 20; ++generation) {
        ASSERT_TRUE(client.publish_latest(make_snapshot(generation)));
    }

    std::atomic<int> snapshot_count{0};
    std::atomic<std::uint64_t> received_generation{0};
    cxxime::UiChannelServer server;
    ASSERT_TRUE(server.start(
        [&](cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot& snapshot) {
            received_generation.store(snapshot.target_generation);
            snapshot_count.fetch_add(1);
        },
        {}, pipe_name));

    ASSERT_TRUE(wait_for([&]() { return received_generation.load() == 20; }, 5000));
    ASSERT_EQ(snapshot_count.load(), 1);
    client.stop();
    server.stop();
}

TEST(UiChannel, publish_latest_never_waits_for_a_pipe_connection) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t generation = 1; generation <= 1000; ++generation) {
        ASSERT_TRUE(client.publish_latest(make_snapshot(generation)));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_TRUE(elapsed.count() < 100);
    ASSERT_TRUE(!client.is_connected());
    client.stop();
}

TEST(UiChannel, server_routes_command_to_the_originating_endpoint) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<cxxime::UiEndpointId> endpoint{0};
    cxxime::UiChannelServer server;
    ASSERT_TRUE(server.start(
        [&](cxxime::UiEndpointId value, const cxxime::UiPresentationSnapshot&) {
            endpoint.store(value);
        },
        {}, pipe_name));

    std::atomic<int> command_count{0};
    cxxime::UiCommand received;
    std::mutex received_mutex;
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start(
        [&](const cxxime::UiCommand& command) {
            std::lock_guard<std::mutex> lock(received_mutex);
            received = command;
            command_count.fetch_add(1);
        },
        pipe_name));
    ASSERT_TRUE(client.publish_latest(make_snapshot(7)));
    ASSERT_TRUE(wait_for([&]() { return endpoint.load() != 0; }));

    cxxime::UiCommand command;
    command.session_id = 17;
    command.session_generation = 3;
    command.target_generation = 7;
    command.composition_generation = 17;
    command.presentation_generation = 18;
    command.type = cxxime::UiCommandType::kPageNext;
    ASSERT_TRUE(server.send_command(endpoint.load(), command));
    ASSERT_TRUE(wait_for([&]() { return command_count.load() == 1; }));
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        ASSERT_EQ(received.type, cxxime::UiCommandType::kPageNext);
        ASSERT_EQ(received.target_generation, static_cast<std::uint64_t>(7));
    }

    client.stop();
    server.stop();
}

TEST(UiChannel, client_republishes_latest_snapshot_after_server_restart) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(client.publish_latest(make_snapshot(55)));

    {
        std::atomic<std::uint64_t> received{0};
        cxxime::UiChannelServer server;
        ASSERT_TRUE(server.start(
            [&](cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot& snapshot) {
                received.store(snapshot.target_generation);
            },
            {}, pipe_name));
        ASSERT_TRUE(wait_for([&]() { return received.load() == 55; }, 5000));
        server.stop();
    }
    ASSERT_TRUE(wait_for([&]() { return !client.is_connected(); }));
    {
        std::atomic<std::uint64_t> received{0};
        cxxime::UiChannelServer server;
        ASSERT_TRUE(server.start(
            [&](cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot& snapshot) {
                received.store(snapshot.target_generation);
            },
            {}, pipe_name));
        ASSERT_TRUE(wait_for([&]() { return received.load() == 55; }, 5000));
        server.stop();
    }

    client.stop();
}

TEST(UiChannel, disconnect_callback_releases_the_endpoint) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> disconnect_count{0};
    cxxime::UiChannelServer server;
    ASSERT_TRUE(
        server.start({}, [&](cxxime::UiEndpointId) { disconnect_count.fetch_add(1); }, pipe_name));

    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.endpoint_count() == 1; }));

    client.stop();
    ASSERT_TRUE(wait_for([&]() { return disconnect_count.load() == 1; }));
    ASSERT_EQ(server.endpoint_count(), static_cast<std::size_t>(0));
    server.stop();
}

TEST(UiChannel, server_stop_cancels_active_endpoint_without_waiting_for_a_client) {
    const std::wstring pipe_name = test_pipe_name();
    cxxime::UiChannelServer server;
    ASSERT_TRUE(server.start({}, {}, pipe_name));

    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(wait_for([&]() { return server.endpoint_count() == 1; }));

    const auto start = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    ASSERT_TRUE(elapsed.count() < 1000);
    ASSERT_TRUE(!server.is_running());
    client.stop();
}

RUN_ALL_TESTS()
