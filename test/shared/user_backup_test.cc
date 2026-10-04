// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <miniz.h>

#include <cxxime/user_backup.h>

#include "support/testutil.h"

namespace {

std::wstring backup_path(const wchar_t* suffix) {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + L"zhiyi-user-backup-" +
           std::to_wstring(GetCurrentProcessId()) + suffix;
}

bool write_raw_archive(const std::wstring& path,
                       const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return false;
    }
    bool succeeded = true;
    for (const auto& entry : entries) {
        succeeded =
            succeeded && mz_zip_writer_add_mem(&zip, entry.first.c_str(), entry.second.data(),
                                               entry.second.size(), MZ_NO_COMPRESSION) != 0;
    }
    void* archive_data = nullptr;
    size_t archive_size = 0;
    if (succeeded) {
        succeeded = mz_zip_writer_finalize_heap_archive(&zip, &archive_data, &archive_size) != 0;
    }
    mz_zip_writer_end(&zip);
    FILE* file = nullptr;
    if (succeeded) {
        succeeded = _wfopen_s(&file, path.c_str(), L"wb") == 0 && file &&
                    fwrite(archive_data, 1, archive_size, file) == archive_size;
    }
    if (file) {
        fclose(file);
    }
    mz_free(archive_data);
    return succeeded;
}

std::string read_bytes(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::size_t find_zip_entry_name(const std::string& bytes, const std::string& path,
                                std::size_t header_size, char signature_third,
                                char signature_fourth) {
    std::size_t position = bytes.find(path);
    while (position != std::string::npos) {
        const std::size_t header = position >= header_size ? position - header_size : bytes.size();
        if (header + 4 <= bytes.size() && bytes[header] == 'P' && bytes[header + 1] == 'K' &&
            bytes[header + 2] == signature_third && bytes[header + 3] == signature_fourth) {
            return position;
        }
        position = bytes.find(path, position + path.size());
    }
    return std::string::npos;
}

} // namespace

TEST(UserBackup, round_trips_manifest_and_selected_components) {
    const std::wstring path = backup_path(L"-roundtrip.zhiyi-backup");
    DeleteFileW(path.c_str());
    const std::vector<cxxime::UserBackupEntry> entries = {
        {cxxime::UserBackupComponent::kSettings, "config/settings.json", "{}\n"},
        {cxxime::UserBackupComponent::kUserLexicon, "lexicon/user_pinyin.tsv",
         "你好\tnihao\t1\tni:hao\n"},
        {cxxime::UserBackupComponent::kUserLexicon, "lexicon/user_wubi.tsv", "世界\tlw\t1\n"},
    };
    cxxime::UserBackupSummary written;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::write_user_backup_archive(path, entries, &written, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_SUCCESS));
    ASSERT_EQ(written.entry_count, entries.size());

    cxxime::UserBackupArchive restored;
    ASSERT_TRUE(cxxime::read_user_backup_archive(path, &restored, &error));
    ASSERT_EQ(restored.summary.format_version, cxxime::kUserBackupFormatVersion);
    ASSERT_EQ(restored.summary.components,
              cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kSettings) |
                  cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kUserLexicon));
    ASSERT_EQ(restored.entries.size(), entries.size());
    ASSERT_EQ(restored.entries[1].contents, entries[1].contents);
    DeleteFileW(path.c_str());
}

TEST(UserBackup, rejects_unknown_paths_and_tampered_contents) {
    const std::wstring path = backup_path(L"-tampered.zhiyi-backup");
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(!cxxime::write_user_backup_archive(
        path, {{cxxime::UserBackupComponent::kSettings, "../default.json", "{}"}}, nullptr,
        &error));
    ASSERT_TRUE(!cxxime::write_user_backup_archive(
        path, {{cxxime::UserBackupComponent::kLearning, "learning/learning_pinyin.tsv", ""}},
        nullptr, &error));

    ASSERT_TRUE(cxxime::write_user_backup_archive(
        path,
        {{cxxime::UserBackupComponent::kSettings, "config/settings.json", "unique-backup-payload"}},
        nullptr, &error));
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t payload = bytes.find("unique-backup-payload");
    ASSERT_NE(payload, std::string::npos);
    bytes[payload] = 'X';
    input.close();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();

    cxxime::UserBackupArchive restored;
    ASSERT_TRUE(!cxxime::read_user_backup_archive(path, &restored, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_INVALID_DATA));
    DeleteFileW(path.c_str());
}

TEST(UserBackup, rejects_oversized_manifest_and_corrupt_empty_entries) {
    const std::wstring path = backup_path(L"-limits.zhiyi-backup");
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(write_raw_archive(path, {{"manifest.json", std::string(64 * 1024 + 1, ' ')}}));
    cxxime::UserBackupArchive restored;
    ASSERT_TRUE(!cxxime::read_user_backup_archive(path, &restored, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_INVALID_DATA));

    const std::vector<cxxime::UserBackupEntry> empty_lexicon = {
        {cxxime::UserBackupComponent::kUserLexicon, "lexicon/user_pinyin.tsv", ""},
        {cxxime::UserBackupComponent::kUserLexicon, "lexicon/user_wubi.tsv", ""},
    };
    ASSERT_TRUE(cxxime::write_user_backup_archive(path, empty_lexicon, nullptr, &error));
    std::string bytes = read_bytes(path);
    const std::string entry_path = "lexicon/user_pinyin.tsv";
    const std::size_t local_name = find_zip_entry_name(bytes, entry_path, 30, 3, 4);
    ASSERT_NE(local_name, std::string::npos);
    bytes[local_name - 30] = 0;
    write_bytes(path, bytes);
    ASSERT_TRUE(!cxxime::read_user_backup_archive(path, &restored, &error));

    ASSERT_TRUE(cxxime::write_user_backup_archive(path, empty_lexicon, nullptr, &error));
    bytes = read_bytes(path);
    const std::size_t first_name = find_zip_entry_name(bytes, entry_path, 30, 3, 4);
    const std::size_t central_name = find_zip_entry_name(bytes, entry_path, 46, 1, 2);
    ASSERT_NE(first_name, std::string::npos);
    ASSERT_NE(central_name, std::string::npos);
    bytes[first_name - 30 + 14] = 1;
    bytes[central_name - 46 + 16] = 1;
    write_bytes(path, bytes);
    ASSERT_TRUE(!cxxime::read_user_backup_archive(path, &restored, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_INVALID_DATA));
    DeleteFileW(path.c_str());
}

RUN_ALL_TESTS()
