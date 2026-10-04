// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <fstream>
#include <string>

#include <windows.h>

#include <cxxime/installer_lifecycle.h>

#include "support/testutil.h"

namespace {

constexpr std::size_t kGenerationIdHexLength = 8;
constexpr wchar_t kGenerationPrefix[] = L"0.5.0.";

std::wstring test_root(const wchar_t* suffix) {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return std::wstring(directory) + L"zhiyi-lifecycle-" + std::to_wstring(GetCurrentProcessId()) +
           suffix;
}

void create_directory(const std::wstring& path) {
    ASSERT_TRUE(CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
                GetLastError() == ERROR_ALREADY_EXISTS);
}

void write_file(const std::wstring& path, const std::string& contents = "x") {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output.good());
}

void remove_tree(const std::wstring& root) {
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring path = root + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(path);
            } else {
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    RemoveDirectoryW(root.c_str());
}

} // namespace

TEST(InstallerLifecycle, persists_prepared_target_and_allocates_unique_generations) {
    const std::wstring root = test_root(L"-allocate");
    remove_tree(root);
    create_directory(root);

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, L"", "0.5.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.5.0.") == 0);
    ASSERT_EQ(result.install_target.size(),
              root.size() + 1 + std::wstring(kGenerationPrefix).size() + kGenerationIdHexLength);
    cxxime::installer::InstallLifecycleResult persisted;
    ASSERT_TRUE(cxxime::installer::get_prepared_install_target(root, &persisted, &error));
    ASSERT_EQ(persisted.install_target, result.install_target);

    const std::wstring first_target = result.install_target;
    create_directory(first_target);
    write_file(first_target + L"\\zhiyi-server.exe");
    write_file(first_target + L"\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,)"
               R"("files":["zhiyi-server.exe"]})");
    ASSERT_TRUE(
        cxxime::installer::commit_install_lifecycle(root, first_target, L"", &result, &error));
    ASSERT_TRUE(cxxime::installer::get_prepared_install_target(root, &persisted, &error));
    ASSERT_TRUE(persisted.install_target.empty());
    ASSERT_TRUE(
        cxxime::installer::prepare_install_lifecycle(root, first_target, "0.5.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.5.0.") == 0);
    ASSERT_NE(result.install_target, first_target);
    const std::wstring second_target = result.install_target;
    ASSERT_TRUE(cxxime::installer::validate_uninstall_lifecycle(root, first_target, &error));
    ASSERT_TRUE(
        cxxime::installer::prepare_install_lifecycle(root, first_target, "0.5.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.5.0.") == 0);
    ASSERT_NE(result.install_target, first_target);
    ASSERT_NE(result.install_target, second_target);
    remove_tree(root);
}

TEST(InstallerLifecycle, manifest_drives_cleanup_and_preserves_unknown_files) {
    const std::wstring root = test_root(L"-manifest");
    remove_tree(root);
    create_directory(root);
    create_directory(root + L"\\0.5.0");
    write_file(root + L"\\0.5.0\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,"files":[]})");
    create_directory(root + L"\\0.4.0");
    create_directory(root + L"\\0.4.0\\licenses");
    write_file(root + L"\\0.4.0\\owned.dll");
    write_file(root + L"\\0.4.0\\licenses\\owned.txt");
    write_file(root + L"\\0.4.0\\.zhiyi-install-transaction");
    write_file(root + L"\\0.4.0\\unknown.txt");
    write_file(root + L"\\0.4.0\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,)"
               R"("files":["owned.dll","licenses/owned.txt"]})");

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, root + L"\\0.5.0", "0.6.0",
                                                             &result, &error));
    ASSERT_EQ(result.unknown_files, 1U);
    ASSERT_TRUE(GetFileAttributesW((root + L"\\0.4.0\\owned.dll").c_str()) ==
                INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(GetFileAttributesW((root + L"\\0.4.0\\.zhiyi-install-transaction").c_str()) ==
                INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(GetFileAttributesW((root + L"\\0.4.0\\unknown.txt").c_str()) !=
                INVALID_FILE_ATTRIBUTES);
    remove_tree(root);
}

TEST(InstallerLifecycle, invalid_retired_manifest_does_not_block_install) {
    const std::wstring root = test_root(L"-invalid-manifest");
    remove_tree(root);
    create_directory(root);
    create_directory(root + L"\\0.4.0");
    write_file(root + L"\\0.4.0\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,)"
               R"("files":["../outside.txt"]})");

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(!cxxime::installer::validate_uninstall_lifecycle(root, root + L"\\0.4.0", &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_INVALID_DATA));
    error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, L"", "0.5.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.5.0.") == 0);
    ASSERT_EQ(result.remaining_generations, 1U);
    remove_tree(root);
}

TEST(InstallerLifecycle, commit_requires_complete_active_payload) {
    const std::wstring root = test_root(L"-complete-active-payload");
    remove_tree(root);
    create_directory(root);

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, L"", "0.5.0", &result, &error));
    const std::wstring target = result.install_target;
    ASSERT_TRUE(!cxxime::installer::commit_install_lifecycle(root, target, L"", &result, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));

    create_directory(target);
    write_file(target + L"\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,)"
               R"("files":["owned.dll"]})");
    ASSERT_TRUE(!cxxime::installer::commit_install_lifecycle(root, target, L"", &result, &error));
    ASSERT_EQ(error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
    write_file(target + L"\\owned.dll");
    ASSERT_TRUE(cxxime::installer::commit_install_lifecycle(root, target, L"", &result, &error));
    remove_tree(root);
}

TEST(InstallerLifecycle, missing_active_manifest_allows_repair_target) {
    const std::wstring root = test_root(L"-missing-manifest");
    const std::wstring active = root + L"\\0.5.0.00000000000000000000000000000000";
    remove_tree(root);
    create_directory(root);
    create_directory(active);
    write_file(active + L"\\zhiyi-server.exe");

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(
        cxxime::installer::prepare_install_lifecycle(root, active, "0.6.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.6.0.") == 0);
    remove_tree(root);
}

TEST(InstallerLifecycle, corrupt_state_allows_repair_target) {
    const std::wstring root = test_root(L"-corrupt-state");
    const std::wstring active = root + L"\\0.5.0.00000000000000000000000000000000";
    remove_tree(root);
    create_directory(root);
    create_directory(root + L"\\maintenance");
    create_directory(active);
    write_file(active + L"\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,"files":[]})");
    write_file(root + L"\\maintenance\\install-state.json", "not json");

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(
        cxxime::installer::prepare_install_lifecycle(root, active, "0.6.0", &result, &error));
    ASSERT_TRUE(result.install_target.find(root + L"\\0.6.0.") == 0);
    remove_tree(root);
}

TEST(InstallerLifecycle, uninstall_cleans_active_and_discovered_retired_generations) {
    const std::wstring root = test_root(L"-uninstall");
    remove_tree(root);
    create_directory(root);
    create_directory(root + L"\\0.5.0");
    create_directory(root + L"\\0.4.0");
    const std::string manifest =
        R"({"format":"zhiyi-install-manifest","version":1,"files":["owned.dll"]})";
    write_file(root + L"\\0.5.0\\owned.dll");
    write_file(root + L"\\0.5.0\\install-manifest.json", manifest);
    write_file(root + L"\\0.4.0\\owned.dll");
    write_file(root + L"\\0.4.0\\install-manifest.json", manifest);

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(
        cxxime::installer::uninstall_install_lifecycle(root, root + L"\\0.5.0", &result, &error));
    ASSERT_EQ(result.cleaned_generations, 2U);
    ASSERT_EQ(result.remaining_generations, 0U);
    ASSERT_EQ(GetFileAttributesW((root + L"\\0.5.0").c_str()), INVALID_FILE_ATTRIBUTES);
    ASSERT_EQ(GetFileAttributesW((root + L"\\0.4.0").c_str()), INVALID_FILE_ATTRIBUTES);
    remove_tree(root);
}

TEST(InstallerLifecycle, missing_registration_retires_stale_active_generation) {
    const std::wstring root = test_root(L"-stale-active");
    remove_tree(root);
    create_directory(root);
    create_directory(root + L"\\0.5.0");
    write_file(root + L"\\0.5.0\\zhiyi-server.exe");
    write_file(root + L"\\0.5.0\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,)"
               R"("files":["zhiyi-server.exe"]})");

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, root + L"\\0.5.0", "0.6.0",
                                                             &result, &error));
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, L"", "0.6.0", &result, &error));
    ASSERT_EQ(GetFileAttributesW((root + L"\\0.5.0").c_str()), INVALID_FILE_ATTRIBUTES);
    ASSERT_TRUE(result.install_target.find(root + L"\\0.6.0.") == 0);
    remove_tree(root);
}

TEST(InstallerLifecycle, retries_generation_after_directory_handle_blocks_cleanup) {
    const std::wstring root = test_root(L"-retry-directory");
    remove_tree(root);
    create_directory(root);

    cxxime::installer::InstallLifecycleResult result;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(cxxime::installer::prepare_install_lifecycle(root, L"", "0.5.0", &result, &error));
    const std::wstring first = result.install_target;
    create_directory(first);
    write_file(first + L"\\owned.dll");
    write_file(first + L"\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,"files":["owned.dll"]})");
    ASSERT_TRUE(cxxime::installer::commit_install_lifecycle(root, first, L"", &result, &error));

    ASSERT_TRUE(
        cxxime::installer::prepare_install_lifecycle(root, first, "0.6.0", &result, &error));
    const std::wstring second = result.install_target;
    create_directory(second);
    write_file(second + L"\\owned.dll");
    write_file(second + L"\\install-manifest.json",
               R"({"format":"zhiyi-install-manifest","version":1,"files":["owned.dll"]})");
    ASSERT_TRUE(cxxime::installer::commit_install_lifecycle(root, second, first, &result, &error));

    HANDLE directory = CreateFileW(first.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(directory, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(cxxime::installer::collect_install_garbage(root, &result, &error));
    ASSERT_EQ(result.remaining_generations, 1U);
    ASSERT_TRUE(GetFileAttributesW(first.c_str()) != INVALID_FILE_ATTRIBUTES);
    CloseHandle(directory);

    ASSERT_TRUE(cxxime::installer::collect_install_garbage(root, &result, &error));
    ASSERT_EQ(result.remaining_generations, 0U);
    ASSERT_EQ(GetFileAttributesW(first.c_str()), INVALID_FILE_ATTRIBUTES);
    remove_tree(root);
}

RUN_ALL_TESTS()
