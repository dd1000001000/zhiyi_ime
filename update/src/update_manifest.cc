// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// latest.json, version order and the update state file.

#include <cxxime/update.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <cxxime/data_path.h>
#include <cxxime/installer_version.h>
#include <json.hpp>

namespace cxxime {
namespace update {
namespace {

bool is_lower_hex(const std::string& text, size_t length) {
    if (text.size() != length) return false;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && (ch < 'a' || ch > 'f')) return false;
    }
    return true;
}

// zhiyi-v<version>-setup.exe: no path separators or other surprises in a file name the
// installer is saved under.
bool is_installer_name(const std::string& file, const std::string& version) {
    return file == "zhiyi-v" + version + "-setup.exe";
}

bool is_release_version(const std::string& version) {
    installer::VersionOrder order;
    return installer::compare_semantic_versions(version, version, &order);
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

std::wstring default_state_path() {
    return utf8_to_wide(cxxime::user_data_path("update-state.json"));
}

}  // namespace

std::string Manifest::notes_for(const std::string& language) const {
    for (const std::string& key : {language, std::string("en-US")}) {
        const auto found = notes.find(key);
        if (found != notes.end() && !found->second.empty()) return found->second;
    }
    for (const auto& [key, text] : notes) {
        if (!text.empty()) return text;
    }
    return {};
}

bool parse_manifest(const std::string& text, const std::string& download_prefix,
                    Manifest* manifest) {
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (!manifest || !json.is_object()) return false;
    Manifest parsed;
    auto get_string = [&](const char* key, std::string* value) {
        const auto found = json.find(key);
        if (found == json.end() || !found->is_string()) return false;
        *value = found->get<std::string>();
        return !value->empty();
    };
    if (!get_string("version", &parsed.version) || !get_string("file", &parsed.file) ||
        !get_string("url", &parsed.url) || !get_string("sha256", &parsed.sha256)) {
        return false;
    }
    get_string("published", &parsed.published);
    const auto size = json.find("size");
    if (size == json.end() || !size->is_number_unsigned()) return false;
    parsed.size = size->get<std::uint64_t>();
    const auto notes = json.find("notes");
    if (notes != json.end() && notes->is_object()) {
        for (const auto& [language, value] : notes->items()) {
            if (value.is_string()) parsed.notes[language] = value.get<std::string>();
        }
    }
    if (!is_release_version(parsed.version) || !is_installer_name(parsed.file, parsed.version) ||
        !is_lower_hex(parsed.sha256, 64) || parsed.size == 0 ||
        parsed.url != download_prefix + "v" + parsed.version + "/" + parsed.file) {
        return false;
    }
    *manifest = std::move(parsed);
    return true;
}

bool is_newer_release(const std::string& candidate, const std::string& current) {
    if (candidate.find('-') != std::string::npos) return false;  // pre-releases are not offered
    installer::VersionOrder order;
    return installer::compare_semantic_versions(candidate, current, &order) &&
           order == installer::VersionOrder::kNewer;
}

State read_state(const std::wstring& path) {
    State state;
    std::ifstream stream(std::filesystem::path(path.empty() ? default_state_path() : path),
                         std::ios::binary);
    if (!stream) return state;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (!json.is_object()) return state;
    if (json.contains("skipped_version") && json["skipped_version"].is_string()) {
        state.skipped_version = json["skipped_version"].get<std::string>();
    }
    if (json.contains("pending_version") && json["pending_version"].is_string()) {
        state.pending_version = json["pending_version"].get<std::string>();
    }
    return state;
}

bool write_state(const State& state, const std::wstring& path) {
    const std::filesystem::path target(path.empty() ? default_state_path() : path);
    nlohmann::json json = nlohmann::json::object();
    if (!state.skipped_version.empty()) json["skipped_version"] = state.skipped_version;
    if (!state.pending_version.empty()) json["pending_version"] = state.pending_version;
    std::filesystem::path temp = target;
    temp += L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << json.dump(2);
        if (!stream) return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring download_directory() {
    const std::wstring directory = utf8_to_wide(cxxime::user_data_path("updates"));
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

void clean_downloads(const std::wstring& directory, const std::wstring& keep) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::wstring name = entry.path().filename().wstring();
        if (!entry.is_regular_file(error) || name.rfind(L"zhiyi-v", 0) != 0) continue;
        if (!keep.empty() && (name == keep || name == keep + L".part")) continue;
        std::filesystem::remove(entry.path(), error);
    }
}

void clean_installed_downloads(const std::wstring& directory, const std::string& current_version) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::wstring name = entry.path().filename().wstring();
        if (!entry.is_regular_file(error)) continue;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, L".part") == 0)
            name.resize(name.size() - 5);
        const std::wstring prefix = L"zhiyi-v", suffix = L"-setup.exe";
        if (name.size() <= prefix.size() + suffix.size() || name.rfind(prefix, 0) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::wstring wide = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        std::string version;
        for (const wchar_t ch : wide) {
            if (ch > 0x7f) {
                version.clear();
                break;
            }
            version.push_back(static_cast<char>(ch));
        }
        installer::VersionOrder order;
        if (version.empty() || !installer::compare_semantic_versions(version, current_version, &order) ||
            order == installer::VersionOrder::kNewer)
            continue;
        std::filesystem::remove(entry.path(), error);
    }
}

}  // namespace update
}  // namespace cxxime
