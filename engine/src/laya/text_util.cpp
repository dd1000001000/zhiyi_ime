// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "text_util.h"

#include <windows.h>

namespace laya {

static size_t utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 0;  // 续字节或非法首字节
}

std::vector<std::string> utf8_chars(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t n = utf8_len(static_cast<unsigned char>(s[i]));
    bool ok = n > 0 && i + n <= s.size();
    for (size_t k = 1; ok && k < n; ++k)
      ok = (static_cast<unsigned char>(s[i + k]) >> 6) == 0x2;
    if (!ok) n = 1;
    out.emplace_back(s.substr(i, n));
    i += n;
  }
  return out;
}

size_t utf8_length(const std::string& s) { return utf8_chars(s).size(); }

std::string utf8_tail(const std::string& s, size_t n) {
  auto chars = utf8_chars(s);
  if (chars.size() <= n) return s;
  std::string out;
  for (size_t i = chars.size() - n; i < chars.size(); ++i) out += chars[i];
  return out;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

}  // namespace laya
