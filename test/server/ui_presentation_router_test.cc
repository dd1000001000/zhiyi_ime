// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include <cxxime/ui_channel.h>

#include "ui_presentation_router.h"
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
    return L"\\\\.\\pipe\\ZhiyiIME-UI-Router-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(sequence.fetch_add(1));
}

cxxime::UiPresentationSnapshot make_snapshot(std::uint64_t target_generation, bool visible = true) {
    cxxime::UiPresentationSnapshot snapshot;
    snapshot.session_id = 12;
    snapshot.session_generation = 4;
    snapshot.target_generation = target_generation;
    snapshot.composition_generation = 9;
    snapshot.presentation_generation = target_generation + 20;
    snapshot.ownership = cxxime::UiOwnership::kExternal;
    snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kComposing) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret);
    if (visible) {
        snapshot.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible) |
                          cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates);
        snapshot.candidate_page.count = 1;
        snapshot.candidate_page.total = 1;
        snapshot.candidate_known_count = 1;
        snapshot.candidate_extent_state = cxxime::CandidateExtentState::kExhausted;
        snapshot.candidate_page.candidates[0].text_length = 1;
        snapshot.candidate_page.candidates[0].text[0] = 'a';
    }
    snapshot.caret = {20, 40, 21, 60};
    return snapshot;
}

cxxime::UiCommand make_command(const cxxime::UiPresentationSnapshot& snapshot,
                               cxxime::UiCommandType type, std::uint32_t candidate_index = 0) {
    cxxime::UiCommand command;
    command.session_id = snapshot.session_id;
    command.session_generation = snapshot.session_generation;
    command.target_generation = snapshot.target_generation;
    command.composition_generation = snapshot.composition_generation;
    command.presentation_generation = snapshot.presentation_generation;
    command.type = type;
    command.candidate_index = candidate_index;
    return command;
}

} // namespace

TEST(UiPresentationRouter, ignores_stale_snapshots_and_routes_bound_commands) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> presentation_count{0};
    std::atomic<int> clear_count{0};
    std::atomic<cxxime::UiEndpointId> presented_endpoint{0};
    std::atomic<std::uint64_t> presented_generation{0};
    UiPresentationRouter router;
    ASSERT_TRUE(router.start(
        [&](cxxime::UiEndpointId endpoint, const cxxime::UiPresentationSnapshot* snapshot,
            std::uint64_t candidate_placement_cycle,
            std::uint64_t router_revision) {
            UNREFERENCED_PARAMETER(candidate_placement_cycle);
            UNREFERENCED_PARAMETER(router_revision);
            if (snapshot) {
                presented_endpoint.store(endpoint);
                presented_generation.store(snapshot->target_generation);
                presentation_count.fetch_add(1);
            } else {
                clear_count.fetch_add(1);
            }
        },
        pipe_name));

    std::atomic<int> command_count{0};
    cxxime::UiCommand received;
    std::mutex received_mutex;
    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start(
        [&](const cxxime::UiCommand& command) {
            if (command.type == cxxime::UiCommandType::kRefreshInputIndicator) {
                return;
            }
            std::lock_guard<std::mutex> lock(received_mutex);
            received = command;
            command_count.fetch_add(1);
        },
        pipe_name));

    const cxxime::UiPresentationSnapshot first = make_snapshot(7);
    ASSERT_TRUE(client.publish_latest(first));
    ASSERT_TRUE(wait_for([&]() { return presented_generation.load() == 7; }));
    ASSERT_TRUE(
        router.send_command(presented_endpoint.load(),
                            make_command(first, cxxime::UiCommandType::kSelectCandidate, 2)));
    ASSERT_TRUE(wait_for([&]() { return command_count.load() == 1; }));
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        ASSERT_EQ(received.type, cxxime::UiCommandType::kSelectCandidate);
        ASSERT_EQ(received.candidate_index, static_cast<std::uint32_t>(2));
        ASSERT_EQ(received.target_generation, static_cast<std::uint64_t>(7));
    }

    ASSERT_TRUE(client.publish_latest(make_snapshot(6)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(presentation_count.load(), 1);
    ASSERT_EQ(presented_generation.load(), static_cast<std::uint64_t>(7));

    const cxxime::UiPresentationSnapshot replacement = make_snapshot(8);
    ASSERT_TRUE(client.publish_latest(replacement));
    ASSERT_TRUE(wait_for([&]() { return presented_generation.load() == 8; }));
    ASSERT_TRUE(!router.send_command(presented_endpoint.load(),
                                     make_command(first, cxxime::UiCommandType::kPageNext)));
    ASSERT_TRUE(router.send_command(presented_endpoint.load(),
                                    make_command(replacement, cxxime::UiCommandType::kPageNext)));
    ASSERT_TRUE(wait_for([&]() { return command_count.load() == 2; }));

    cxxime::UiPresentationSnapshot host = replacement;
    host.presentation_generation++;
    host.ownership = cxxime::UiOwnership::kHost;
    ASSERT_TRUE(client.publish_latest(host));
    ASSERT_TRUE(wait_for([&]() { return presentation_count.load() == 3; }));
    ASSERT_TRUE(!router.send_command(presented_endpoint.load(),
                                     make_command(host, cxxime::UiCommandType::kPageNext)));

    cxxime::UiPresentationSnapshot local = host;
    local.presentation_generation++;
    local.ownership = cxxime::UiOwnership::kExternal;
    local.flags |= cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate);
    ASSERT_TRUE(client.publish_latest(local));
    ASSERT_TRUE(wait_for([&]() { return presentation_count.load() >= 5; }));
    ASSERT_TRUE(!router.send_command(presented_endpoint.load(),
                                     make_command(local, cxxime::UiCommandType::kSelectCandidate)));

    const cxxime::UiPresentationSnapshot hidden = make_snapshot(9, false);
    ASSERT_TRUE(client.publish_latest(hidden));
    ASSERT_TRUE(wait_for([&]() { return clear_count.load() == 1; }));
    ASSERT_TRUE(!router.send_command(presented_endpoint.load(),
                                     make_command(first, cxxime::UiCommandType::kPageNext)));
    ASSERT_TRUE(!router.send_command(presented_endpoint.load(),
                                     make_command(hidden, cxxime::UiCommandType::kPageNext)));

    client.stop();
    router.stop();
}

TEST(UiPresentationRouter, disconnect_clears_only_the_active_endpoint) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> clear_count{0};
    std::atomic<cxxime::UiEndpointId> endpoint{0};
    UiPresentationRouter router;
    ASSERT_TRUE(router.start(
        [&](cxxime::UiEndpointId published_endpoint,
            const cxxime::UiPresentationSnapshot* published_snapshot,
            std::uint64_t candidate_placement_cycle,
            std::uint64_t router_revision) {
            UNREFERENCED_PARAMETER(candidate_placement_cycle);
            UNREFERENCED_PARAMETER(router_revision);
            if (published_snapshot) {
                endpoint.store(published_endpoint);
            } else {
                clear_count.fetch_add(1);
            }
        },
        pipe_name));

    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));
    ASSERT_TRUE(client.publish_latest(make_snapshot(3)));
    ASSERT_TRUE(wait_for([&]() { return endpoint.load() != 0; }));
    ASSERT_TRUE(router.send_command(
        endpoint.load(), make_command(make_snapshot(3), cxxime::UiCommandType::kPageNext)));
    client.stop();
    ASSERT_TRUE(wait_for([&]() { return clear_count.load() == 1; }));
    router.stop();
}

TEST(UiPresentationRouter, refreshes_only_connected_sessions_and_clears_resume_ui) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> presentation_count{0};
    std::atomic<int> clear_count{0};
    UiPresentationRouter router;
    ASSERT_TRUE(router.start(
        [&](cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot* snapshot,
            std::uint64_t, std::uint64_t) {
            if (snapshot) {
                presentation_count.fetch_add(1);
            } else {
                clear_count.fetch_add(1);
            }
        },
        pipe_name));

    std::atomic<int> first_refresh_count{0};
    cxxime::UiCommand first_command;
    std::mutex first_command_mutex;
    cxxime::UiChannelClient first_client;
    ASSERT_TRUE(first_client.start(
        [&](const cxxime::UiCommand& command) {
            if (command.type == cxxime::UiCommandType::kRefreshInputIndicator) {
                std::lock_guard<std::mutex> lock(first_command_mutex);
                first_command = command;
                first_refresh_count.fetch_add(1);
            }
        },
        pipe_name));

    const cxxime::UiPresentationSnapshot visible = make_snapshot(5);
    ASSERT_TRUE(first_client.publish_latest(visible));
    ASSERT_TRUE(wait_for([&]() { return presentation_count.load() == 1; }));
    router.reconcile_system_ui(true);
    ASSERT_TRUE(wait_for([&]() { return first_refresh_count.load() == 1; }));
    ASSERT_TRUE(wait_for([&]() { return clear_count.load() == 1; }));
    {
        std::lock_guard<std::mutex> lock(first_command_mutex);
        ASSERT_EQ(first_command.session_id, visible.session_id);
        ASSERT_EQ(first_command.session_generation, visible.session_generation);
        ASSERT_EQ(first_command.target_generation, static_cast<std::uint64_t>(0));
        ASSERT_EQ(first_command.composition_generation, static_cast<std::uint64_t>(0));
    }

    std::atomic<int> second_refresh_count{0};
    cxxime::UiChannelClient second_client;
    ASSERT_TRUE(second_client.start(
        [&](const cxxime::UiCommand& command) {
            if (command.type == cxxime::UiCommandType::kRefreshInputIndicator) {
                second_refresh_count.fetch_add(1);
            }
        },
        pipe_name));
    cxxime::UiPresentationSnapshot second = make_snapshot(20);
    second.session_id = 24;
    second.session_generation = 5;
    ASSERT_TRUE(second_client.publish_latest(second));
    ASSERT_TRUE(wait_for([&]() { return presentation_count.load() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(second_refresh_count.load(), 0);

    router.reconcile_system_ui(false);
    ASSERT_TRUE(wait_for([&]() { return second_refresh_count.load() == 1; }));
    ASSERT_EQ(clear_count.load(), 1);

    second_client.stop();
    first_client.stop();
    router.stop();
}

TEST(UiPresentationRouter, candidate_placement_cycle_tracks_content_lifetime) {
    const std::wstring pipe_name = test_pipe_name();
    std::atomic<int> callback_count{0};
    std::atomic<bool> has_snapshot{false};
    std::atomic<std::uint64_t> placement_cycle{0};
    UiPresentationRouter router;
    ASSERT_TRUE(router.start(
        [&](cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot* snapshot,
            std::uint64_t candidate_placement_cycle, std::uint64_t) {
            has_snapshot.store(snapshot != nullptr);
            placement_cycle.store(candidate_placement_cycle);
            callback_count.fetch_add(1);
        },
        pipe_name));

    cxxime::UiChannelClient client;
    ASSERT_TRUE(client.start({}, pipe_name));

    cxxime::UiPresentationSnapshot visible = make_snapshot(3);
    ASSERT_TRUE(client.publish_latest(visible));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 1; }));
    const std::uint64_t first_cycle = placement_cycle.load();
    ASSERT_NE(first_cycle, static_cast<std::uint64_t>(0));

    visible.composition_generation++;
    visible.presentation_generation++;
    ASSERT_TRUE(client.publish_latest(visible));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 2; }));
    ASSERT_EQ(placement_cycle.load(), first_cycle);

    cxxime::UiPresentationSnapshot waiting = visible;
    waiting.presentation_generation++;
    waiting.flags &= ~cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible);
    ASSERT_TRUE(client.publish_latest(waiting));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 3; }));
    ASSERT_TRUE(!has_snapshot.load());
    ASSERT_EQ(placement_cycle.load(), first_cycle);

    visible.presentation_generation = waiting.presentation_generation + 1;
    ASSERT_TRUE(client.publish_latest(visible));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 4; }));
    ASSERT_TRUE(has_snapshot.load());
    ASSERT_EQ(placement_cycle.load(), first_cycle);

    cxxime::UiPresentationSnapshot cleared = visible;
    cleared.presentation_generation++;
    cleared.ownership = cxxime::UiOwnership::kNone;
    cleared.flags &= ~cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible);
    cleared.flags &= ~cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates);
    ASSERT_TRUE(client.publish_latest(cleared));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 5; }));
    ASSERT_TRUE(!has_snapshot.load());
    ASSERT_EQ(placement_cycle.load(), static_cast<std::uint64_t>(0));

    visible.composition_generation = cleared.composition_generation + 1;
    visible.presentation_generation = cleared.presentation_generation + 1;
    ASSERT_TRUE(client.publish_latest(visible));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 6; }));
    const std::uint64_t second_cycle = placement_cycle.load();
    ASSERT_NE(second_cycle, first_cycle);
    ASSERT_NE(second_cycle, static_cast<std::uint64_t>(0));

    cxxime::UiPresentationSnapshot next_target = visible;
    next_target.target_generation++;
    next_target.composition_generation++;
    next_target.presentation_generation++;
    ASSERT_TRUE(client.publish_latest(next_target));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 7; }));
    const std::uint64_t third_cycle = placement_cycle.load();
    ASSERT_NE(third_cycle, second_cycle);
    ASSERT_NE(third_cycle, static_cast<std::uint64_t>(0));

    cxxime::UiPresentationSnapshot ended = next_target;
    ended.composition_generation++;
    ended.presentation_generation++;
    ended.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kSessionEnded);
    ended.ownership = cxxime::UiOwnership::kNone;
    ASSERT_TRUE(client.publish_latest(ended));
    ASSERT_TRUE(wait_for([&]() { return callback_count.load() == 8; }));
    ASSERT_TRUE(!has_snapshot.load());
    ASSERT_EQ(placement_cycle.load(), static_cast<std::uint64_t>(0));

    client.stop();
    router.stop();
}

RUN_ALL_TESTS()
