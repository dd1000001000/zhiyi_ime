// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_PIPE_NAMES_H_
#define CXXIME_PIPE_NAMES_H_

#include <string>

namespace cxxime {

constexpr wchar_t IPC_PIPE_BASE_NAME[] = L"\\\\.\\pipe\\ZhiyiIME";
constexpr wchar_t CONTROL_PIPE_BASE_NAME[] = L"\\\\.\\pipe\\ZhiyiIME-Control";
constexpr wchar_t UI_PIPE_BASE_NAME[] = L"\\\\.\\pipe\\ZhiyiIME-UI";

std::wstring make_user_pipe_name(const std::wstring& base_name);
std::wstring make_user_pipe_name(const std::wstring& base_name, const std::wstring& username);

} // namespace cxxime

#endif // CXXIME_PIPE_NAMES_H_
