// UTF-8 / UTF-16 小工具 (Windows API 用 UTF-16, 其余一律 UTF-8)。
#pragma once

#include <string>
#include <vector>

namespace laya {

// 按 UTF-8 字符切分 (非法字节单独成一个元素, 交给 byte fallback 处理)。
std::vector<std::string> utf8_chars(const std::string& s);

// 返回 s 中最后 n 个 UTF-8 字符。
std::string utf8_tail(const std::string& s, size_t n);

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& w);

// 把全部 from 替换成 to。
std::string replace_all(std::string s, const std::string& from, const std::string& to);

}  // namespace laya
