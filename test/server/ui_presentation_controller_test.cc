// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.
//
// Modified by Zhiyi IME Contributors: the floating status window and its z-order tests were
// removed; the controller presents the candidate window only.

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>

#include <windows.h>

#include <cxxime/candidate_window.h>

#include "support/dpi_testutil.h"
#include "support/testutil.h"
#include "ui_presentation_controller.h"

namespace {

bool wait_for(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
    return true;
}

HWND find_window(DWORD process, const wchar_t* class_name) {
    struct Search {
        DWORD process;
        const wchar_t* class_name;
        HWND result = nullptr;
    } search{process, class_name};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(parameter);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            wchar_t name[64] = {};
            if (process == search.process && GetClassNameW(window, name, 64) &&
                lstrcmpW(name, search.class_name) == 0) {
                search.result = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.result;
}

}  // namespace

TEST(UiPresentationController, presents_the_candidate_window_only) {
    test::ScopedDpiAwarenessContext dpi;
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                       100, 100, 400, 200, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(owner != nullptr);
    auto config = std::make_shared<cxxime::Config>();
    config->render_backend = "gdi";
    UiPresentationController controller;
    ASSERT_TRUE(controller.start(config, [](cxxime::UiEndpointId, const cxxime::UiCommand&) {}));

    cxxime::UiPresentationSnapshot snapshot;
    snapshot.session_id = 1;
    snapshot.session_generation = 1;
    snapshot.target_generation = 1;
    snapshot.composition_generation = 1;
    snapshot.presentation_generation = 1;
    snapshot.ownership = cxxime::UiOwnership::kExternal;
    snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates);
    snapshot.candidate_page.count = 1;
    snapshot.candidate_page.total = 1;
    snapshot.candidate_known_count = 1;
    snapshot.candidate_page.candidates[0].text_length = 9;
    std::memcpy(snapshot.candidate_page.candidates[0].text, "candidate", 9);
    snapshot.target_window = reinterpret_cast<std::uint64_t>(owner);
    snapshot.caret = {150, 150, 151, 170};
    controller.present(1, &snapshot, 1, 1);

    HWND candidate = nullptr;
    ASSERT_TRUE(wait_for([&]() {
        candidate = find_window(GetCurrentProcessId(), L"ZhiyiIMECandidateWindow");
        return candidate && IsWindowVisible(candidate);
    }));
    // No floating status window any more (the taskbar indicator has the states).
    ASSERT_TRUE(find_window(GetCurrentProcessId(), L"ZhiyiIMEStatusWindow") == nullptr);

    // A snapshot from a module before 0.7.5 may still carry the status bit: ignored.
    snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kReservedStatusVisible) |
                     cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret);
    ++snapshot.presentation_generation;
    controller.present(1, &snapshot, 1, 2);
    ASSERT_TRUE(wait_for([&]() { return !IsWindowVisible(candidate); }));
    ASSERT_TRUE(find_window(GetCurrentProcessId(), L"ZhiyiIMEStatusWindow") == nullptr);

    controller.present(0, nullptr, 1, 3);
    controller.stop();
    DestroyWindow(owner);
}

RUN_ALL_TESTS()
