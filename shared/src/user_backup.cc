// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/user_backup.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include <json.hpp>
#include <miniz.h>

#include <cxxime/version.h>

namespace cxxime {
namespace {

constexpr char kManifestPath[] = "manifest.json";
constexpr std::uint64_t kMaxManifestSize = 64ULL * 1024ULL;
constexpr std::size_t kMaxManifestTextFieldSize = 128;
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kLocalHeaderFlagsOffset = 6;
constexpr std::size_t kLocalHeaderMethodOffset = 8;
constexpr std::size_t kLocalHeaderCrcOffset = 14;
constexpr std::size_t kLocalHeaderCompressedSizeOffset = 18;
constexpr std::size_t kLocalHeaderUncompressedSizeOffset = 22;
constexpr std::size_t kLocalHeaderFilenameSizeOffset = 26;
constexpr std::size_t kLocalHeaderExtraSizeOffset = 28;
constexpr std::uint16_t kDataDescriptorFlag = 1U << 3;

struct KnownBackupPath {
    const char* path;
    UserBackupComponent component;
};

constexpr KnownBackupPath kKnownPaths[] = {
    {"config/settings.json", UserBackupComponent::kSettings},
    {"config/device.json", UserBackupComponent::kDeviceSettings},
    {"lexicon/user_pinyin.tsv", UserBackupComponent::kUserLexicon},
    {"lexicon/user_wubi.tsv", UserBackupComponent::kUserLexicon},
    {"ranking/candidate_order_pinyin.tsv", UserBackupComponent::kCandidateOrder},
    {"ranking/candidate_order_wubi.tsv", UserBackupComponent::kCandidateOrder},
    {"learning/learning_pinyin.tsv", UserBackupComponent::kLearning},
    {"learning/learning_wubi.tsv", UserBackupComponent::kLearning},
    {"learning/learning_composition.tsv", UserBackupComponent::kLearning},
    {"learning/learning_english.json", UserBackupComponent::kLearning},
    {"disabled/disabled_pinyin.tsv", UserBackupComponent::kDisabledSystemLexicon},
    {"disabled/disabled_wubi.tsv", UserBackupComponent::kDisabledSystemLexicon},
};

void set_error(unsigned long* error_code, unsigned long value) {
    if (error_code) {
        *error_code = value;
    }
}

std::string utc_timestamp() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);
    char text[32] = {};
    snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear, time.wMonth,
             time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}

bool sha256(const std::string& contents, std::string* output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD result_size = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                   &result_size, 0);
    }
    if (status >= 0) {
        status =
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                              sizeof(hash_size), &result_size, 0);
    }
    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    if (status >= 0 && (object_size == 0 || hash_size == 0)) {
        status = -1;
    }
    if (status >= 0) {
        status = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0);
    }
    if (status >= 0 && !contents.empty()) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(contents.data())),
                                static_cast<ULONG>(contents.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(), hash_size, 0);
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (status < 0) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    output->resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        (*output)[index * 2] = kHex[digest[index] >> 4];
        (*output)[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    return true;
}

bool write_file_atomically(const std::wstring& path, const void* data, std::size_t size,
                           unsigned long* error_code) {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error_code, GetLastError());
        return false;
    }
    DWORD written = 0;
    bool succeeded =
        size <= MAXDWORD &&
        (size == 0 ||
         (WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size));
    if (succeeded) {
        succeeded = FlushFileBuffers(file) != FALSE;
    }
    unsigned long result = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (succeeded) {
        succeeded = MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        result = succeeded ? ERROR_SUCCESS : GetLastError();
    }
    if (!succeeded) {
        DeleteFileW(temporary.c_str());
    }
    set_error(error_code, result);
    return succeeded;
}

bool read_file(const std::wstring& path, std::vector<unsigned char>* contents,
               unsigned long* error_code) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error_code, GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool succeeded = GetFileSizeEx(file, &size) != FALSE;
    unsigned long result = succeeded ? ERROR_SUCCESS : GetLastError();
    if (succeeded &&
        (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(kMaxUserBackupArchiveSize) ||
         size.QuadPart > MAXDWORD)) {
        succeeded = false;
        result = ERROR_FILE_TOO_LARGE;
    }
    if (succeeded) {
        contents->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        succeeded =
            contents->empty() || (ReadFile(file, contents->data(),
                                           static_cast<DWORD>(contents->size()), &read, nullptr) &&
                                  read == contents->size());
        result = succeeded ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(file);
    set_error(error_code, result);
    return succeeded;
}

bool components_are_complete(const std::set<std::string>& paths, std::uint32_t components) {
    return std::all_of(std::begin(kKnownPaths), std::end(kKnownPaths),
                       [&](const KnownBackupPath& item) {
                           return (components & user_backup_component_flag(item.component)) == 0 ||
                                  paths.find(item.path) != paths.end();
                       });
}

bool valid_entries(const std::vector<UserBackupEntry>& entries, std::uint32_t* components,
                   std::uint64_t* total_size) {
    std::set<std::string> paths;
    *components = 0;
    *total_size = 0;
    if (entries.empty() || entries.size() > std::size(kKnownPaths)) {
        return false;
    }
    for (const UserBackupEntry& entry : entries) {
        UserBackupComponent expected;
        if (!user_backup_component_for_path(entry.path, &expected) || expected != entry.component ||
            !paths.insert(entry.path).second || entry.contents.size() > kMaxUserBackupEntrySize ||
            *total_size > kMaxUserBackupArchiveSize - entry.contents.size()) {
            return false;
        }
        *components |= user_backup_component_flag(entry.component);
        *total_size += entry.contents.size();
    }
    return (*components & ~kAllUserBackupComponents) == 0 &&
           components_are_complete(paths, *components);
}

bool make_manifest(const std::vector<UserBackupEntry>& entries, const UserBackupSummary& summary,
                   std::string* output) {
    nlohmann::json manifest = {
        {"format", "zhiyi-user-backup"},     {"format_version", summary.format_version},
        {"app_version", summary.app_version}, {"created_at_utc", summary.created_at_utc},
        {"components", summary.components},   {"files", nlohmann::json::array()},
    };
    for (const UserBackupEntry& entry : entries) {
        std::string digest;
        if (!sha256(entry.contents, &digest)) {
            return false;
        }
        manifest["files"].push_back({
            {"path", entry.path},
            {"component", user_backup_component_name(entry.component)},
            {"size", entry.contents.size()},
            {"sha256", digest},
        });
    }
    *output = manifest.dump(2) + "\n";
    return true;
}

bool valid_local_entry(mz_zip_archive* zip, const mz_zip_archive_file_stat& stat,
                       const std::string& path) {
    std::array<unsigned char, kLocalHeaderSize> header = {};
    const std::uint64_t archive_size = mz_zip_get_archive_size(zip);
    if (archive_size < kLocalHeaderSize ||
        stat.m_local_header_ofs > archive_size - kLocalHeaderSize ||
        mz_zip_read_archive_data(zip, stat.m_local_header_ofs, header.data(), header.size()) !=
            header.size() ||
        MZ_READ_LE32(header.data()) != kLocalHeaderSignature ||
        MZ_READ_LE16(header.data() + kLocalHeaderMethodOffset) != stat.m_method) {
        return false;
    }
    const std::uint16_t flags = MZ_READ_LE16(header.data() + kLocalHeaderFlagsOffset);
    const std::uint16_t filename_size =
        MZ_READ_LE16(header.data() + kLocalHeaderFilenameSizeOffset);
    const std::uint16_t extra_size = MZ_READ_LE16(header.data() + kLocalHeaderExtraSizeOffset);
    const std::uint64_t data_offset =
        stat.m_local_header_ofs + kLocalHeaderSize + filename_size + extra_size;
    if (filename_size != path.size() || data_offset > archive_size ||
        stat.m_comp_size > archive_size - data_offset) {
        return false;
    }
    std::string local_path(filename_size, '\0');
    if (mz_zip_read_archive_data(zip, stat.m_local_header_ofs + kLocalHeaderSize, &local_path[0],
                                 local_path.size()) != local_path.size() ||
        local_path != path) {
        return false;
    }
    if ((flags & kDataDescriptorFlag) == 0 &&
        (MZ_READ_LE32(header.data() + kLocalHeaderCrcOffset) != stat.m_crc32 ||
         MZ_READ_LE32(header.data() + kLocalHeaderCompressedSizeOffset) != stat.m_comp_size ||
         MZ_READ_LE32(header.data() + kLocalHeaderUncompressedSizeOffset) != stat.m_uncomp_size)) {
        return false;
    }
    return true;
}

} // namespace

const char* user_backup_component_name(UserBackupComponent component) {
    switch (component) {
    case UserBackupComponent::kSettings:
        return "settings";
    case UserBackupComponent::kUserLexicon:
        return "user_lexicon";
    case UserBackupComponent::kCandidateOrder:
        return "candidate_order";
    case UserBackupComponent::kLearning:
        return "learning";
    case UserBackupComponent::kDisabledSystemLexicon:
        return "disabled_system_lexicon";
    case UserBackupComponent::kDeviceSettings:
        return "device_settings";
    default:
        return "";
    }
}

bool user_backup_component_for_path(const std::string& path, UserBackupComponent* component) {
    const auto found = std::find_if(std::begin(kKnownPaths), std::end(kKnownPaths),
                                    [&](const KnownBackupPath& item) { return path == item.path; });
    if (found == std::end(kKnownPaths) || !component) {
        return false;
    }
    *component = found->component;
    return true;
}

bool write_user_backup_archive(const std::wstring& path,
                               const std::vector<UserBackupEntry>& entries,
                               UserBackupSummary* summary, unsigned long* error_code) {
    std::uint32_t components = 0;
    std::uint64_t total_size = 0;
    if (path.empty() || !valid_entries(entries, &components, &total_size)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    UserBackupSummary result;
    result.format_version = kUserBackupFormatVersion;
    result.components = components;
    result.app_version = CXXIME_VERSION_STRING;
    result.created_at_utc = utc_timestamp();
    result.entry_count = entries.size();
    result.total_size = total_size;
    std::string manifest;
    if (!make_manifest(entries, result, &manifest)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }

    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        set_error(error_code, ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    bool succeeded = mz_zip_writer_add_mem(&zip, kManifestPath, manifest.data(), manifest.size(),
                                           MZ_NO_COMPRESSION) != 0;
    for (const UserBackupEntry& entry : entries) {
        if (!succeeded) {
            break;
        }
        succeeded = mz_zip_writer_add_mem(&zip, entry.path.c_str(), entry.contents.data(),
                                          entry.contents.size(), MZ_NO_COMPRESSION) != 0;
    }
    void* archive_data = nullptr;
    size_t archive_size = 0;
    if (succeeded) {
        succeeded = mz_zip_writer_finalize_heap_archive(&zip, &archive_data, &archive_size) != 0;
    }
    mz_zip_writer_end(&zip);
    if (succeeded && archive_size <= kMaxUserBackupArchiveSize) {
        succeeded = write_file_atomically(path, archive_data, archive_size, error_code);
    } else if (succeeded) {
        set_error(error_code, ERROR_FILE_TOO_LARGE);
        succeeded = false;
    } else {
        set_error(error_code, ERROR_INVALID_DATA);
    }
    mz_free(archive_data);
    if (succeeded && summary) {
        *summary = std::move(result);
    }
    return succeeded;
}

bool read_user_backup_archive(const std::wstring& path, UserBackupArchive* archive,
                              unsigned long* error_code) {
    if (!archive || path.empty()) {
        set_error(error_code, ERROR_INVALID_PARAMETER);
        return false;
    }
    std::vector<unsigned char> data;
    if (!read_file(path, &data, error_code)) {
        return false;
    }
    mz_zip_archive zip = {};
    if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    UserBackupArchive result;
    std::set<std::string> archive_paths;
    std::map<std::string, std::string> extracted;
    std::uint64_t extracted_total = 0;
    bool succeeded = mz_zip_reader_get_num_files(&zip) <= std::size(kKnownPaths) + 1;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; succeeded && index < count; ++index) {
        mz_zip_archive_file_stat stat = {};
        succeeded = mz_zip_reader_file_stat(&zip, index, &stat) != 0 && stat.m_is_supported &&
                    !stat.m_is_directory && !stat.m_is_encrypted &&
                    (stat.m_method == 0 || stat.m_method == MZ_DEFLATED) &&
                    stat.m_uncomp_size <= (std::strcmp(stat.m_filename, kManifestPath) == 0
                                               ? kMaxManifestSize
                                               : kMaxUserBackupEntrySize) &&
                    extracted_total <= kMaxUserBackupArchiveSize - stat.m_uncomp_size;
        const std::string entry_path = succeeded ? stat.m_filename : std::string();
        succeeded = succeeded &&
                    mz_zip_reader_get_filename(&zip, index, nullptr, 0) == entry_path.size() + 1;
        UserBackupComponent ignored;
        succeeded =
            succeeded &&
            (entry_path == kManifestPath || user_backup_component_for_path(entry_path, &ignored)) &&
            archive_paths.insert(entry_path).second && valid_local_entry(&zip, stat, entry_path);
        if (!succeeded) {
            break;
        }
        extracted_total += stat.m_uncomp_size;
        size_t extracted_size = 0;
        void* extracted_data = mz_zip_reader_extract_to_heap(&zip, index, &extracted_size, 0);
        succeeded = extracted_data != nullptr || extracted_size == 0;
        if (succeeded) {
            if (extracted_size == 0) {
                extracted[entry_path].clear();
            } else {
                extracted[entry_path].assign(static_cast<const char*>(extracted_data),
                                             extracted_size);
            }
            const mz_ulong actual_crc = mz_crc32(
                MZ_CRC32_INIT, reinterpret_cast<const mz_uint8*>(extracted[entry_path].data()),
                extracted_size);
            succeeded = actual_crc == stat.m_crc32;
        }
        mz_free(extracted_data);
    }
    mz_zip_reader_end(&zip);
    auto manifest_entry = extracted.find(kManifestPath);
    if (!succeeded || manifest_entry == extracted.end()) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }

    try {
        const nlohmann::json manifest = nlohmann::json::parse(manifest_entry->second);
        if (!manifest.is_object() || manifest.value("format", "") != "zhiyi-user-backup" ||
            manifest.value("format_version", 0u) != kUserBackupFormatVersion ||
            manifest.value("app_version", "").empty() ||
            manifest.value("created_at_utc", "").empty() || !manifest.contains("files") ||
            !manifest["files"].is_array() || manifest["files"].size() + 1 != extracted.size()) {
            throw std::runtime_error("invalid backup manifest");
        }
        const std::string app_version = manifest["app_version"].get<std::string>();
        const std::string created_at_utc = manifest["created_at_utc"].get<std::string>();
        if (app_version.size() > kMaxManifestTextFieldSize ||
            created_at_utc.size() > kMaxManifestTextFieldSize) {
            throw std::runtime_error("oversized backup metadata");
        }
        result.summary.format_version = manifest["format_version"].get<std::uint32_t>();
        result.summary.components = manifest.value("components", 0u);
        result.summary.app_version = std::move(app_version);
        result.summary.created_at_utc = std::move(created_at_utc);
        std::set<std::string> manifest_paths;
        std::uint32_t actual_components = 0;
        for (const nlohmann::json& file : manifest["files"]) {
            const std::string entry_path = file.at("path").get<std::string>();
            UserBackupComponent component;
            auto contents = extracted.find(entry_path);
            std::string digest;
            if (!manifest_paths.insert(entry_path).second ||
                !user_backup_component_for_path(entry_path, &component) ||
                contents == extracted.end() ||
                file.at("component").get<std::string>() != user_backup_component_name(component) ||
                file.at("size").get<std::uint64_t>() != contents->second.size() ||
                !sha256(contents->second, &digest) ||
                file.at("sha256").get<std::string>() != digest) {
                throw std::runtime_error("invalid backup file");
            }
            actual_components |= user_backup_component_flag(component);
            result.summary.total_size += contents->second.size();
            result.entries.push_back({component, entry_path, std::move(contents->second)});
        }
        if (actual_components != result.summary.components ||
            (actual_components & ~kAllUserBackupComponents) != 0 ||
            !components_are_complete(manifest_paths, actual_components)) {
            throw std::runtime_error("invalid component mask");
        }
        result.summary.entry_count = result.entries.size();
    } catch (const std::exception&) {
        set_error(error_code, ERROR_INVALID_DATA);
        return false;
    }
    *archive = std::move(result);
    set_error(error_code, ERROR_SUCCESS);
    return true;
}

} // namespace cxxime
