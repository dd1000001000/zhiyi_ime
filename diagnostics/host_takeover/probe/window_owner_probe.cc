// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "window_owner_probe.h"

#include <cxxime/host_trace.h>

#include <dwmapi.h>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace cxxime_probe {
namespace {

constexpr wchar_t kWindowOwnerProbeClass[] = L"ZhiyiImeWindowOwnerProbeWindow";

WindowOwnerProbe* g_active_probe = nullptr;

std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text, -1, &result[0], required, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string window_class(HWND hwnd) {
    wchar_t value[256] = {};
    return GetClassNameW(hwnd, value, ARRAYSIZE(value)) ? wide_to_utf8(value) : "";
}

std::string process_basename(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return {};
    }

    wchar_t path[32768] = {};
    DWORD length = ARRAYSIZE(path);
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried || length == 0) {
        return {};
    }

    DWORD offset = 0;
    for (DWORD index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/') {
            offset = index + 1;
        }
    }
    return wide_to_utf8(path + offset);
}

const char* event_name(DWORD event) {
    switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
        return "foreground";
    case EVENT_OBJECT_CREATE:
        return "create";
    case EVENT_OBJECT_DESTROY:
        return "destroy";
    case EVENT_OBJECT_SHOW:
        return "show";
    case EVENT_OBJECT_HIDE:
        return "hide";
    case EVENT_OBJECT_LOCATIONCHANGE:
        return "location_change";
    case EVENT_OBJECT_IME_SHOW:
        return "ime_show";
    case EVENT_OBJECT_IME_HIDE:
        return "ime_hide";
    case EVENT_OBJECT_IME_CHANGE:
        return "ime_change";
    default:
        return "unknown";
    }
}

bool is_ime_visibility_event(DWORD event) {
    return event >= EVENT_OBJECT_IME_SHOW && event <= EVENT_OBJECT_IME_CHANGE;
}

} // namespace

const std::wstring& WindowOwnerProbe::initialization_error() const {
    return initialization_error_;
}

bool WindowOwnerProbe::fail_initialization(const char* step, DWORD error) {
    std::wostringstream message;
    message << L"Failed to initialize the window owner Probe.\n\nStep: " << step
            << L"\nWin32 error: 0x" << std::uppercase << std::hex << std::setw(8)
            << std::setfill(L'0') << error;
    initialization_error_ = message.str();
    cxxime::write_host_trace("probe", "probe.window_owner.initialization", {
        {"step", step ? step : ""},
        {"win32_error", error},
        {"result", "failed"},
    });
    return false;
}

bool WindowOwnerProbe::install_hook(DWORD event_min, DWORD event_max) {
    SetLastError(ERROR_SUCCESS);
    HWINEVENTHOOK hook = SetWinEventHook(
        event_min, event_max, nullptr, win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) {
        return false;
    }
    hooks_.push_back(hook);
    return true;
}

bool WindowOwnerProbe::initialize(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance_;
    window_class.lpfnWndProc = window_proc;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowOwnerProbeClass;
    if (!RegisterClassExW(&window_class)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return fail_initialization("RegisterClassExW", error);
        }
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowOwnerProbeClass, L"Zhiyi IME Window Owner Probe",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        560, 180, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return fail_initialization("CreateWindowExW", GetLastError());
    }

    g_active_probe = this;
    if (!install_hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND) ||
        !install_hook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE) ||
        !install_hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE) ||
        !install_hook(EVENT_OBJECT_IME_SHOW, EVENT_OBJECT_IME_CHANGE)) {
        return fail_initialization("SetWinEventHook", GetLastError());
    }

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    cxxime::write_host_trace("probe", "probe.window_owner.runtime", {
        {"hwnd", reinterpret_cast<uintptr_t>(hwnd_)},
        {"hook_count", hooks_.size()},
        {"result", "ready"},
    });
    trace_initial_windows();
    return true;
}

int WindowOwnerProbe::run() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void WindowOwnerProbe::shutdown() {
    for (HWINEVENTHOOK hook : hooks_) {
        UnhookWinEvent(hook);
    }
    hooks_.clear();
    if (g_active_probe == this) {
        g_active_probe = nullptr;
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

void WindowOwnerProbe::trace_initial_windows() {
    EnumWindows(enum_window_proc, reinterpret_cast<LPARAM>(this));
}

BOOL CALLBACK WindowOwnerProbe::enum_window_proc(HWND hwnd, LPARAM context) {
    auto* probe = reinterpret_cast<WindowOwnerProbe*>(context);
    if (probe && IsWindowVisible(hwnd)) {
        probe->trace_window("snapshot", 0, hwnd, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }
    return TRUE;
}

void CALLBACK WindowOwnerProbe::win_event_proc(HWINEVENTHOOK,
                                               DWORD event,
                                               HWND hwnd,
                                               LONG object_id,
                                               LONG child_id,
                                               DWORD event_thread,
                                               DWORD event_time) {
    if (!g_active_probe || !hwnd) {
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND && !is_ime_visibility_event(event) &&
        (object_id != OBJID_WINDOW || child_id != CHILDID_SELF)) {
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE && !IsWindowVisible(hwnd)) {
        return;
    }
    g_active_probe->trace_window(
        event_name(event), event, hwnd, object_id, child_id, event_thread, event_time);
}

void WindowOwnerProbe::trace_window(const char* action,
                                    DWORD event,
                                    HWND hwnd,
                                    LONG object_id,
                                    LONG child_id,
                                    DWORD event_thread,
                                    DWORD event_time) {
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(hwnd, &process_id);
    RECT rect = {};
    const bool rect_valid = GetWindowRect(hwnd, &rect) != FALSE;
    DWORD cloaked = 0;
    const HRESULT cloaked_result = DwmGetWindowAttribute(
        hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    cxxime::write_host_trace("probe", "probe.window_owner.event", {
        {"action", action ? action : ""},
        {"win_event", event},
        {"event_thread", event_thread},
        {"event_time_ms", event_time},
        {"hwnd", reinterpret_cast<uintptr_t>(hwnd)},
        {"object_id", object_id},
        {"child_id", child_id},
        {"window_pid", process_id},
        {"window_tid", thread_id},
        {"window_process", process_basename(process_id)},
        {"window_class", window_class(hwnd)},
        {"is_window", IsWindow(hwnd) != FALSE},
        {"visible", IsWindowVisible(hwnd) != FALSE},
        {"foreground", GetForegroundWindow() == hwnd},
        {"top_level", GetAncestor(hwnd, GA_ROOT) == hwnd},
        {"parent", reinterpret_cast<uintptr_t>(GetParent(hwnd))},
        {"owner", reinterpret_cast<uintptr_t>(GetWindow(hwnd, GW_OWNER))},
        {"root", reinterpret_cast<uintptr_t>(GetAncestor(hwnd, GA_ROOT))},
        {"root_owner", reinterpret_cast<uintptr_t>(GetAncestor(hwnd, GA_ROOTOWNER))},
        {"style", static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE))},
        {"ex_style", static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))},
        {"rect_valid", rect_valid},
        {"left", rect.left},
        {"top", rect.top},
        {"right", rect.right},
        {"bottom", rect.bottom},
        {"cloaked_query_hr", static_cast<int64_t>(cloaked_result)},
        {"cloaked", SUCCEEDED(cloaked_result) && cloaked != 0},
        {"result", "observed"},
    });
}

void WindowOwnerProbe::paint(HDC dc) {
    RECT client = {};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 34, 40));
    const wchar_t* lines[] = {
        L"Window ownership monitoring is active.",
        L"Switch to DOTA2, show Microsoft Pinyin candidates, then close this window.",
        L"No window titles or input text are recorded.",
    };
    int y = 24;
    for (const wchar_t* line : lines) {
        TextOutW(dc, 20, y, line, static_cast<int>(wcslen(line)));
        y += 32;
    }
}

LRESULT CALLBACK WindowOwnerProbe::window_proc(HWND hwnd,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
    auto* probe = reinterpret_cast<WindowOwnerProbe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        probe = static_cast<WindowOwnerProbe*>(create->lpCreateParams);
        probe->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    if (!probe) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint_struct = {};
        HDC dc = BeginPaint(hwnd, &paint_struct);
        probe->paint(dc);
        EndPaint(hwnd, &paint_struct);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace cxxime_probe
