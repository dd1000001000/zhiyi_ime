// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include <cxxime/voice_files.h>

#include <windows.h>

#include <filesystem>
#include <fstream>

#include <json.hpp>

#include <cxxime/data_path.h>

namespace cxxime {
namespace voice {

namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// A plain folder name (v<N>): no separators or "..".
bool safe_dir(const std::string& dir) {
    return !dir.empty() && dir.size() <= 32 && dir.find_first_of("\\/:") == std::string::npos &&
           dir.find("..") == std::string::npos;
}

}  // namespace

bool find_installed(InstalledModel* installed) {
    const std::wstring root = wide(local_data_dir()) + wide(kVoiceDir) + L"\\";
    std::ifstream file{std::filesystem::path(root + wide(kVoiceInstalled))};
    const nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (!state.is_object() || !state.contains("dir") || !state["dir"].is_string() ||
        !safe_dir(state["dir"].get<std::string>())) {
        return false;
    }
    InstalledModel found;
    found.dir = root + wide(state["dir"].get<std::string>()) + L"\\";
    found.model = found.dir + wide(kVoiceModelFile);
    found.tokens = found.dir + wide(kVoiceTokensFile);
    found.vad = found.dir + wide(kVoiceVadFile);
    found.version = state.value("version", 0u);
    if (!file_exists(found.model) || !file_exists(found.tokens) || !file_exists(found.vad)) return false;
    if (installed) *installed = std::move(found);
    return true;
}

}  // namespace voice
}  // namespace cxxime
