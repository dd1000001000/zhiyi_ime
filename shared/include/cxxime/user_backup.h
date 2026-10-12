// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_USER_BACKUP_H_
#define CXXIME_USER_BACKUP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cxxime {

enum class UserBackupComponent : std::uint32_t {
    kSettings = 1u << 0,
    kUserLexicon = 1u << 1,
    kCandidateOrder = 1u << 2,
    kLearning = 1u << 3,
    kDisabledSystemLexicon = 1u << 4,
    kDeviceSettings = 1u << 5,
    // The imported word lists (server/src/word_list_service.cc), one entry for all of them;
    // left out of a backup when there is none, so older versions still read it.
    kWordLists = 1u << 6,
};

constexpr std::uint32_t user_backup_component_flag(UserBackupComponent component) {
    return static_cast<std::uint32_t>(component);
}

constexpr std::uint32_t kPortableUserBackupComponents =
    user_backup_component_flag(UserBackupComponent::kSettings) |
    user_backup_component_flag(UserBackupComponent::kUserLexicon) |
    user_backup_component_flag(UserBackupComponent::kCandidateOrder) |
    user_backup_component_flag(UserBackupComponent::kLearning) |
    user_backup_component_flag(UserBackupComponent::kDisabledSystemLexicon) |
    user_backup_component_flag(UserBackupComponent::kWordLists);
constexpr std::uint32_t kAllUserBackupComponents =
    kPortableUserBackupComponents |
    user_backup_component_flag(UserBackupComponent::kDeviceSettings);
constexpr std::uint32_t kUserBackupFormatVersion = 1;
constexpr std::uint64_t kMaxUserBackupEntrySize = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxUserBackupArchiveSize = 80ULL * 1024ULL * 1024ULL;

struct UserBackupEntry {
    UserBackupComponent component = UserBackupComponent::kSettings;
    std::string path;
    std::string contents;
};

struct UserBackupSummary {
    std::uint32_t format_version = 0;
    std::uint32_t components = 0;
    std::string app_version;
    std::string created_at_utc;
    std::size_t entry_count = 0;
    std::uint64_t total_size = 0;
};

struct UserBackupArchive {
    UserBackupSummary summary;
    std::vector<UserBackupEntry> entries;
};

const char* user_backup_component_name(UserBackupComponent component);
bool user_backup_component_for_path(const std::string& path, UserBackupComponent* component);
bool write_user_backup_archive(const std::wstring& path,
                               const std::vector<UserBackupEntry>& entries,
                               UserBackupSummary* summary, unsigned long* error_code = nullptr);
bool read_user_backup_archive(const std::wstring& path, UserBackupArchive* archive,
                              unsigned long* error_code = nullptr);

} // namespace cxxime

#endif // CXXIME_USER_BACKUP_H_
