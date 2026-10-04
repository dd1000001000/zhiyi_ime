// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <fstream>
#include <map>
#include <memory>
#include <string>

#include <windows.h>

#include <json.hpp>

#include <cxxime/user_backup.h>
#include <cxxime/user_backup_control.h>

#include "config_store.h"
#include "config_write_coordinator.h"
#include "session_manager_integration_test_support.h"
#include "user_backup_service.h"

namespace {

void reset_backup_test_files() {
    for (const char* name : {"default.json", "user_pinyin.tsv", "user_wubi.tsv"}) {
        DeleteFileA((test_user_data_dir + "\\" + name).c_str());
    }
}

} // namespace

TEST(UserBackupService, exports_and_imports_settings_and_user_lexicon) {
    reset_backup_test_files();
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict()));
    merge_user_data_for_test(manager,
                             {
                                 {"user_pinyin.tsv", "备份词\tbeifenci\t100\tbei:fen:ci\n"},
                                 {"user_wubi.tsv", ""},
                             });

    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    unsigned long error = ERROR_SUCCESS;
    const std::string user_config = test_user_data_dir + "\\default.json";
    ASSERT_TRUE(store.initialize(std::string(CXXIME_DATA_DIR) + "default.json", user_config,
                                 std::string(CXXIME_DATA_DIR) + "themes.json", &initial, &error));
    ConfigWriteCoordinator writer;
    ASSERT_TRUE(writer.start(&store, [&](const std::shared_ptr<const cxxime::Config>& config) {
        manager.apply_config(config);
    }));
    std::string runtime;
    ASSERT_TRUE(writer.submit(cxxime::UserConfigMutationKind::kMergePatch,
                              R"({"status_window":{"enable":true}})", &runtime, &error));
    UserBackupService service(&manager, &writer);
    const std::string backup = make_temp_path("user-backup-service.zhiyi-backup");
    DeleteFileA(backup.c_str());
    const std::uint32_t components =
        cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kSettings) |
        cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kUserLexicon);

    cxxime::UserBackupControlRequest request;
    request.operation = cxxime::UserBackupOperation::kExport;
    request.path = backup;
    request.components = components;
    std::string payload;
    std::string response_payload;
    cxxime::UserBackupControlResult result;
    request.path = user_config;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    ASSERT_TRUE(service.handle_request(payload, &response_payload));
    ASSERT_TRUE(cxxime::decode_user_backup_result(response_payload, &result));
    ASSERT_TRUE(!result.succeeded);
    ASSERT_EQ(result.error_code, static_cast<std::uint32_t>(ERROR_ACCESS_DENIED));

    request.path = backup;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    ASSERT_TRUE(service.handle_request(payload, &response_payload));
    ASSERT_TRUE(cxxime::decode_user_backup_result(response_payload, &result));
    ASSERT_TRUE(result.succeeded);

    ASSERT_EQ(manager.delete_user_entries(cxxime::UserDictKind::PINYIN,
                                          {{"备份词", "beifenci"}}),
              cxxime::IPCStatus::OK);
    ASSERT_TRUE(writer.submit(cxxime::UserConfigMutationKind::kMergePatch,
                              R"({"status_window":{"enable":false,"x":321,"y":654}})", &runtime,
                              &error));
    merge_user_data_for_test(manager, {
                                          {"user_pinyin.tsv", "新词\txinci\t100\txin:ci\n"},
                                          {"user_wubi.tsv", ""},
                                      });

    request.operation = cxxime::UserBackupOperation::kImport;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    ASSERT_TRUE(service.handle_request(payload, &response_payload));
    ASSERT_TRUE(cxxime::decode_user_backup_result(response_payload, &result));
    ASSERT_TRUE(result.succeeded);
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(2));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(0));
    const auto imported = manager.query_lexicon_entries(
        cxxime::LexiconResource::kUserLexicon, "备份词", cxxime::UserDictKind::PINYIN, 0, 16, true);
    ASSERT_EQ(imported.entries.size(), static_cast<std::size_t>(1));
    const auto retained = manager.query_lexicon_entries(
        cxxime::LexiconResource::kUserLexicon, "新词", cxxime::UserDictKind::PINYIN, 0, 16, true);
    ASSERT_EQ(retained.entries.size(), static_cast<std::size_t>(1));

    std::ifstream config_file(user_config);
    const nlohmann::json imported_config = nlohmann::json::parse(config_file);
    ASSERT_EQ(imported_config["status_window"]["x"].get<int>(), 321);
    ASSERT_EQ(imported_config["status_window"]["y"].get<int>(), 654);
    ASSERT_TRUE(imported_config["status_window"]["enable"].get<bool>());

    writer.stop();
    DeleteFileA(backup.c_str());
}

TEST(UserBackupService, imports_other_data_when_one_config_section_is_rejected) {
    reset_backup_test_files();
    const std::string user_config = test_user_data_dir + "\\default.json";
    {
        std::ofstream config_file(user_config, std::ios::binary);
        config_file << R"({"engine":{"candidate_learning":true},)"
                       R"("status_window":{"enable":true}})";
    }
    SessionManager manager;
    ASSERT_TRUE(manager.initialize(setup_test_dict()));
    merge_user_data_for_test(manager,
                             {
                                 {"user_pinyin.tsv", "备份词\tbeifenci\t100\tbei:fen:ci\n"},
                                 {"user_wubi.tsv", ""},
                             });

    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    unsigned long error = ERROR_SUCCESS;
    ASSERT_TRUE(store.initialize(std::string(CXXIME_DATA_DIR) + "default.json", user_config,
                                 std::string(CXXIME_DATA_DIR) + "themes.json", &initial, &error));
    ConfigWriteCoordinator writer;
    ASSERT_TRUE(writer.start(
        &store,
        [&](const std::shared_ptr<const cxxime::Config>& config) { manager.apply_config(config); },
        [](const std::shared_ptr<const cxxime::Config>& config, unsigned long* prepare_error) {
            if (config->status_window.enable) {
                *prepare_error = ERROR_HOTKEY_ALREADY_REGISTERED;
                return false;
            }
            return true;
        },
        []() {}));
    UserBackupService service(&manager, &writer);
    const std::string backup = make_temp_path("user-backup-best-effort.zhiyi-backup");
    DeleteFileA(backup.c_str());
    const std::uint32_t components =
        cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kSettings) |
        cxxime::user_backup_component_flag(cxxime::UserBackupComponent::kUserLexicon);
    cxxime::UserBackupControlRequest request = {cxxime::UserBackupOperation::kExport, backup,
                                                components};
    std::string payload;
    std::string response_payload;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    ASSERT_TRUE(service.handle_request(payload, &response_payload));

    ASSERT_EQ(manager.delete_user_entries(cxxime::UserDictKind::PINYIN,
                                          {{"备份词", "beifenci"}}),
              cxxime::IPCStatus::OK);
    std::string runtime;
    ASSERT_TRUE(writer.submit(cxxime::UserConfigMutationKind::kMergePatch,
                              R"({"engine":{"candidate_learning":false},)"
                              R"("status_window":{"enable":false}})",
                              &runtime, &error));
    merge_user_data_for_test(manager, {
                                          {"user_pinyin.tsv", "新词\txinci\t100\txin:ci\n"},
                                          {"user_wubi.tsv", ""},
                                      });

    request.operation = cxxime::UserBackupOperation::kImport;
    ASSERT_TRUE(cxxime::encode_user_backup_request(request, &payload));
    ASSERT_TRUE(service.handle_request(payload, &response_payload));
    cxxime::UserBackupControlResult result;
    ASSERT_TRUE(cxxime::decode_user_backup_result(response_payload, &result));
    ASSERT_TRUE(result.succeeded);
    ASSERT_EQ(result.imported_count, static_cast<std::size_t>(2));
    ASSERT_EQ(result.skipped_count, static_cast<std::size_t>(1));
    const auto retained = manager.query_lexicon_entries(
        cxxime::LexiconResource::kUserLexicon, "新词", cxxime::UserDictKind::PINYIN, 0, 16, true);
    ASSERT_EQ(retained.entries.size(), static_cast<std::size_t>(1));
    const auto imported = manager.query_lexicon_entries(
        cxxime::LexiconResource::kUserLexicon, "备份词", cxxime::UserDictKind::PINYIN, 0, 16, true);
    ASSERT_EQ(imported.entries.size(), static_cast<std::size_t>(1));
    std::ifstream config_file(user_config);
    const nlohmann::json merged_config = nlohmann::json::parse(config_file);
    ASSERT_TRUE(merged_config["engine"]["candidate_learning"].get<bool>());
    ASSERT_TRUE(!merged_config["status_window"]["enable"].get<bool>());

    writer.stop();
    DeleteFileA(backup.c_str());
}
