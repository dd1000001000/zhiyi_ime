// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "glossary_packs.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <cxxime/data_path.h>
#include <cxxime/mapped_file.h>
#include <json.hpp>

namespace cxxime {
namespace settings {
namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::string builtin_path(const std::string& source, const std::string& target) {
    return data_path(("glossary\\" + glossary_pack_id(source, target) + ".gloss").c_str());
}

std::filesystem::path state_path() {
    return std::filesystem::path(wide(glossary_directory())) / L"state.json";
}

// Built-in packs the user removed: not copied back.
std::set<std::string> read_removed() {
    std::set<std::string> removed;
    std::ifstream stream(state_path(), std::ios::binary);
    if (!stream) return removed;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_object() && json.contains("removed") && json["removed"].is_array()) {
        for (const auto& id : json["removed"]) {
            if (id.is_string()) removed.insert(id.get<std::string>());
        }
    }
    return removed;
}

void write_removed(const std::set<std::string>& removed) {
    CreateDirectoryW(wide(glossary_directory()).c_str(), nullptr);
    nlohmann::json json = nlohmann::json::object();
    json["removed"] = nlohmann::json::array();
    for (const std::string& id : removed) json["removed"].push_back(id);
    std::ofstream stream(state_path(), std::ios::binary | std::ios::trunc);
    stream << json.dump(2);
}

// Replaces the pack at `path` with `source_file` (copied or moved).
bool replace_pack(const std::wstring& source_file, const std::string& path, bool move) {
    CreateDirectoryW(wide(glossary_directory()).c_str(), nullptr);
    const std::wstring temp = wide(path) + L".new";
    if (move) {
        if (!MoveFileExW(source_file.c_str(), temp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return false;
        }
    } else if (!CopyFileW(source_file.c_str(), temp.c_str(), FALSE)) {
        return false;
    }
    if (!MappedFile::rename_away(path) ||
        !MoveFileExW(temp.c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

}  // namespace

InstalledPack installed_pack(const std::string& source, const std::string& target) {
    InstalledPack pack;
    const std::string path = glossary_pack_path(source, target);
    if (!Glossary::read_header(path, &pack.header) || pack.header.source != source ||
        pack.header.target != target) {
        return pack;
    }
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &data)) {
        pack.size = (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
    }
    pack.installed = true;
    return pack;
}

bool is_builtin_pack(const std::string& source, const std::string& target) {
    return builtin_pack_version(source, target) != 0;
}

std::uint32_t builtin_pack_version(const std::string& source, const std::string& target) {
    GlossaryHeader header;
    return Glossary::read_header(builtin_path(source, target), &header) ? header.content_version
                                                                         : 0;
}

void seed_builtin_packs() {
    const std::filesystem::path directory = wide(glossary_directory());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().filename().wstring().find(L".old-") != std::wstring::npos) {
            std::filesystem::remove(entry.path(), error);  // fails while still mapped
        }
    }
    const std::set<std::string> removed = read_removed();
    for (const std::string source : {"zh", "en"}) {
        for (const std::string& target : glossary_targets(source)) {
            GlossaryHeader builtin;
            if (!Glossary::read_header(builtin_path(source, target), &builtin) ||
                removed.count(glossary_pack_id(source, target)) != 0) {
                continue;
            }
            const InstalledPack installed = installed_pack(source, target);
            if (!installed.installed ||
                installed.header.content_version < builtin.content_version) {
                replace_pack(wide(builtin_path(source, target)),
                             glossary_pack_path(source, target), false);
            }
        }
    }
}

bool install_pack(const std::wstring& downloaded, const std::string& source,
                  const std::string& target) {
    if (!replace_pack(downloaded, glossary_pack_path(source, target), true)) return false;
    std::set<std::string> removed = read_removed();
    if (removed.erase(glossary_pack_id(source, target)) != 0) write_removed(removed);
    return true;
}

bool restore_builtin_pack(const std::string& source, const std::string& target) {
    if (!replace_pack(wide(builtin_path(source, target)), glossary_pack_path(source, target),
                      false)) {
        return false;
    }
    std::set<std::string> removed = read_removed();
    if (removed.erase(glossary_pack_id(source, target)) != 0) write_removed(removed);
    return true;
}

bool remove_pack(const std::string& source, const std::string& target) {
    if (!MappedFile::rename_away(glossary_pack_path(source, target))) return false;
    if (is_builtin_pack(source, target)) {
        std::set<std::string> removed = read_removed();
        removed.insert(glossary_pack_id(source, target));
        write_removed(removed);
    }
    return true;
}

}  // namespace settings
}  // namespace cxxime
