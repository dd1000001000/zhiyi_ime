// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

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

bool is_above(HWND upper, HWND lower) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == upper) {
            return true;
        }
        if (window == lower) {
            return false;
        }
    }
    return false;
}

void raise_window(HWND window) {
    ASSERT_TRUE(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE);
}

void send_message(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    DWORD_PTR result = 0;
    ASSERT_TRUE(SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG, 3000, &result) != 0);
}

// The real candidate lives in a second process, like a TSF-local presenter. Its
// message loop is independent of the server controller's UI thread and the test.
int run_candidate_host() {
    test::ScopedDpiAwarenessContext dpi;
    const HWND owner =
        CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Candidate test host", WS_OVERLAPPEDWINDOW,
                        50, 50, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) {
        return 2;
    }
    cxxime::Config config;
    config.render_backend = "gdi";
    cxxime::CandidateWindow candidate;
    if (!candidate.create(owner, config)) {
        DestroyWindow(owner);
        return 3;
    }
    cxxime::CandidatePage page;
    cxxime::Candidate item;
    item.text = "candidate";
    page.candidates.push_back(item);
    candidate.update(page);
    candidate.move_to_screen_position(200, 200);
    candidate.show();
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    candidate.destroy();
    DestroyWindow(owner);
    return 0;
}

class CandidateHost {
public:
    CandidateHost() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        ASSERT_TRUE(job_ != nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ASSERT_TRUE(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                            sizeof(limits)) != FALSE);
        wchar_t executable[MAX_PATH] = {};
        ASSERT_TRUE(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --candidate-host";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        ASSERT_TRUE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED, nullptr, nullptr, &startup,
                                   &process_) != FALSE);
        ASSERT_TRUE(AssignProcessToJobObject(job_, process_.hProcess) != FALSE);
        ASSERT_TRUE(ResumeThread(process_.hThread) != static_cast<DWORD>(-1));
        ASSERT_TRUE(wait_for([&]() {
            candidate = find_window(process_.dwProcessId, L"CxxIMECandidateWindow");
            owner = GetWindow(candidate, GW_OWNER);
            return owner && IsWindowVisible(candidate);
        }));
    }
    ~CandidateHost() {
        PostThreadMessageW(process_.dwThreadId, WM_QUIT, 0, 0);
        ASSERT_EQ(WaitForSingleObject(process_.hProcess, 3000), WAIT_OBJECT_0);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
        CloseHandle(job_);
    }
    HWND owner = nullptr;
    HWND candidate = nullptr;

private:
    PROCESS_INFORMATION process_ = {};
    HANDLE job_ = nullptr;
};

class ControllerFixture {
public:
    ControllerFixture() {
        auto config = std::make_shared<cxxime::Config>();
        config->render_backend = "gdi";
        config->status_window.enable = true;
        config->status_window.x = 200;
        config->status_window.y = 200;
        ASSERT_TRUE(
            controller.start(config,
                             [this](cxxime::UiEndpointId, const cxxime::UiCommand& command) {
                                 observed_generation.store(command.presentation_generation);
                             },
                             {}));
        status = find_window(GetCurrentProcessId(), L"CxxIMEStatusWindow");
        ASSERT_TRUE(status != nullptr);
        snapshot.session_id = 1;
        snapshot.session_generation = 1;
        snapshot.target_generation = 1;
        snapshot.composition_generation = 1;
        snapshot.ownership = cxxime::UiOwnership::kExternal;
        snapshot.flags = cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kStatusVisible) |
                         cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible) |
                         cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCaret) |
                         cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kHasCandidates) |
                         cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate);
        snapshot.candidate_page.count = 1;
        snapshot.candidate_page.total = 1;
        snapshot.candidate_known_count = 1;
        snapshot.candidate_page.candidates[0].text_length = 9;
        std::memcpy(snapshot.candidate_page.candidates[0].text, "candidate", 9);
        snapshot.target_window = reinterpret_cast<std::uint64_t>(host.owner);
        snapshot.local_candidate_window = reinterpret_cast<std::uint64_t>(host.candidate);
        overlap_candidate();
    }
    ~ControllerFixture() { controller.stop(); }

    void overlap_candidate() {
        RECT rect = {};
        ASSERT_TRUE(GetWindowRect(status, &rect) != FALSE);
        ASSERT_TRUE(SetWindowPos(host.candidate, HWND_TOPMOST, rect.left, rect.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE);
        snapshot.caret = {rect.left, rect.top - 24, rect.left + 1, rect.top - 4};
    }
    void present() {
        ++snapshot.presentation_generation;
        controller.present(1, &snapshot, false, snapshot.target_generation,
                           snapshot.presentation_generation);
        // A real status click echoes the generation of the rendered snapshot.
        // This is a processing barrier, not a fixed sleep or an assertion that
        // can pass against the previous frame's z-order.
        ASSERT_TRUE(wait_for([&]() {
            if (IsWindowVisible(status)) {
                const UINT dpi = GetDpiForWindow(status);
                const LPARAM point = MAKELPARAM(MulDiv(52, dpi, 96), MulDiv(17, dpi, 96));
                send_message(status, WM_LBUTTONDOWN, 0, point);
                send_message(status, WM_LBUTTONUP, 0, point);
            }
            return observed_generation.load() == snapshot.presentation_generation;
        }));
    }

    CandidateHost host;
    UiPresentationController controller;
    cxxime::UiPresentationSnapshot snapshot;
    HWND status = nullptr;
    std::atomic<std::uint64_t> observed_generation{0};
};

TEST(UiPresentationController, local_candidate_stays_above_status_without_being_raised) {
    test::ScopedDpiAwarenessContext dpi;
    ControllerFixture fixture;
    // A third topmost window detects implementations that raise the foreign
    // candidate to fix status ordering instead of moving only their own window.
    const HWND marker =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"Marker", WS_POPUP, 0, 0, 10,
                        10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(marker != nullptr);
    raise_window(marker);
    for (int update = 0; update < 4; ++update) {
        fixture.present();
        ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));
        ASSERT_TRUE(is_above(marker, fixture.host.candidate));
        ASSERT_TRUE((GetWindowLongPtrW(fixture.status, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    }
    ShowWindow(fixture.status, SW_HIDE);
    fixture.present();
    ASSERT_TRUE(IsWindowVisible(fixture.status) != FALSE);
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));

    // DPI/geometry notifications must use the local candidate too, without a new snapshot.
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(fixture.status, &rect) != FALSE);
    raise_window(fixture.status);
    const UINT status_dpi = GetDpiForWindow(fixture.status);
    send_message(fixture.status, WM_DPICHANGED, MAKELONG(status_dpi, status_dpi),
                 reinterpret_cast<LPARAM>(&rect));
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));
    ASSERT_TRUE(is_above(marker, fixture.host.candidate));
    DestroyWindow(marker);
}

TEST(UiPresentationController, local_window_validation_and_legacy_fallback_preserve_order) {
    test::ScopedDpiAwarenessContext dpi;
    ControllerFixture fixture;
    fixture.present();
    // Old senders report visibility but no HWND. Do not re-raise status.
    fixture.snapshot.local_candidate_window = 0;
    fixture.present();
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));

    fixture.snapshot.local_candidate_window =
        reinterpret_cast<std::uint64_t>(fixture.host.candidate);
    raise_window(fixture.status);
    // A different target's candidate must not constrain this status window.
    fixture.snapshot.target_window = reinterpret_cast<std::uint64_t>(fixture.status);
    fixture.present();
    ASSERT_TRUE(is_above(fixture.status, fixture.host.candidate));
    // GetFocus ownership is valid when no view HWND was available to the sender.
    fixture.snapshot.target_window = 0;
    fixture.present();
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));

    // A hidden or non-topmost candidate is not a usable z-order anchor.
    ShowWindow(fixture.host.candidate, SW_HIDE);
    fixture.present();
    ASSERT_TRUE(IsWindowVisible(fixture.host.candidate) == FALSE);
    ASSERT_TRUE((GetWindowLongPtrW(fixture.status, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    ASSERT_TRUE(SetWindowPos(fixture.host.candidate, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE);
    fixture.present();
    ASSERT_TRUE((GetWindowLongPtrW(fixture.status, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    // Destroyed handles must not hide status or resurrect the candidate.
    send_message(fixture.host.candidate, WM_CLOSE, 0, 0);
    ASSERT_TRUE(!IsWindow(fixture.host.candidate));
    fixture.present();
    ASSERT_TRUE(IsWindowVisible(fixture.status) != FALSE);
}

TEST(UiPresentationController, source_switch_and_independent_layout_preserve_candidate_priority) {
    test::ScopedDpiAwarenessContext dpi;
    ControllerFixture fixture;
    fixture.present();
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));
    RECT rect = {};
    ASSERT_TRUE(GetWindowRect(fixture.status, &rect) != FALSE);
    ASSERT_TRUE(SetWindowPos(fixture.host.candidate, nullptr, rect.left, rect.top + 100, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    fixture.present();
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));
    // Local layout can return to overlap without a new presentation or a show().
    ASSERT_TRUE(SetWindowPos(fixture.host.candidate, nullptr, rect.left, rect.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));

    ShowWindow(fixture.host.candidate, SW_HIDE);
    fixture.snapshot.flags &= ~cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kTsfLocalCandidate);
    // Even with a stale extension, flags select the server presenter.
    ++fixture.snapshot.target_generation;
    fixture.present();
    const HWND server_candidate = find_window(GetCurrentProcessId(), L"CxxIMECandidateWindow");
    ASSERT_TRUE(IsWindowVisible(server_candidate) != FALSE);
    RECT candidate_rect = {};
    ASSERT_TRUE(GetWindowRect(server_candidate, &candidate_rect) != FALSE);
    RECT intersection = {};
    ASSERT_TRUE(IntersectRect(&intersection, &rect, &candidate_rect) != FALSE);
    ASSERT_TRUE(is_above(server_candidate, fixture.status));

    fixture.snapshot.flags &= ~cxxime::ui_snapshot_flag(cxxime::UiSnapshotFlag::kCandidateVisible);
    fixture.present();
    ASSERT_TRUE(IsWindowVisible(server_candidate) == FALSE);
    ASSERT_TRUE(IsWindowVisible(fixture.status) != FALSE);
    fixture.controller.present(1, nullptr, false, 0, ++fixture.snapshot.presentation_generation);
    ASSERT_TRUE(wait_for([&]() { return !IsWindowVisible(fixture.status); }));
}

TEST(UiPresentationController, embedded_target_can_have_an_owner_in_another_process) {
    test::ScopedDpiAwarenessContext dpi;
    ControllerFixture fixture;
    const HWND frame =
        CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Embedding frame", WS_OVERLAPPEDWINDOW, 50,
                        50, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(frame != nullptr);
    const LONG_PTR previous_style = GetWindowLongPtrW(fixture.host.owner, GWL_STYLE);
    SetWindowLongPtrW(fixture.host.owner, GWL_STYLE, previous_style | WS_CHILD);
    SetParent(fixture.host.owner, frame);
    ASSERT_EQ(GetAncestor(fixture.host.owner, GA_ROOT), frame);
    SetWindowLongPtrW(fixture.host.candidate, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(frame));
    ASSERT_EQ(GetWindow(fixture.host.candidate, GW_OWNER), frame);
    raise_window(fixture.status);
    fixture.present();
    ASSERT_TRUE(is_above(fixture.host.candidate, fixture.status));

    // Restore the child's hierarchy before either process destroys its windows.
    SetParent(fixture.host.owner, nullptr);
    SetWindowLongPtrW(fixture.host.owner, GWL_STYLE, previous_style);
    SetWindowLongPtrW(fixture.host.candidate, GWLP_HWNDPARENT,
                      reinterpret_cast<LONG_PTR>(fixture.host.owner));
    DestroyWindow(frame);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--candidate-host") == 0) {
        return run_candidate_host();
    }
    return test::RunAllTests();
}
