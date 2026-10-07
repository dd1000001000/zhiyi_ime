// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#ifndef CXXIME_SERVER_LAUNCHER_H_
#define CXXIME_SERVER_LAUNCHER_H_

namespace cxxime {

// Held by the running zhiyi-server for its sign-in session; a second server exits at once.
constexpr wchar_t kServerInstanceMutex[] = L"Local\\ZhiyiIME.Server";

// Starts the installed zhiyi-server.exe when the input method is switched to and the server is
// not running (startup.autostart off, or the server was exited from the taskbar menu). Skipped
// while the installer runs, and in sandboxed (AppContainer) or elevated programs: a server
// started there could not serve the other programs. Returns whether a server was started.
bool start_server_on_demand();

} // namespace cxxime

#endif // CXXIME_SERVER_LAUNCHER_H_
