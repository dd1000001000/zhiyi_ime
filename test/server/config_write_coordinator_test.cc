// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

#include <windows.h>

#include <json.hpp>

#include "config_store.h"
#include "config_write_coordinator.h"
#include "support/testutil.h"

namespace {

std::string test_user_config_path(const char* suffix) {
    char directory[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, directory);
    return std::string(directory) + "zhiyi-config-write-" + std::to_string(GetCurrentProcessId()) +
           "-" + suffix + ".json";
}

bool initialize_store(ConfigStore* store, const std::string& user_path,
                      std::shared_ptr<const cxxime::Config>* config) {
    DeleteFileA(user_path.c_str());
    DeleteFileA((user_path + ".tmp").c_str());
    return store->initialize(std::string(CXXIME_DATA_DIR) + "default.json", user_path,
                             std::string(CXXIME_DATA_DIR) + "themes.json", config);
}

nlohmann::json read_json(const std::string& path) {
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

} // namespace

TEST(ConfigWriteCoordinator, batches_ordered_patches_without_dropping_fields) {
    const std::string user_path = test_user_config_path("batch");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));

    std::atomic<int> apply_count{0};
    std::atomic<bool> file_was_visible{false};
    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(&store, [&](const std::shared_ptr<const cxxime::Config>&) {
        try {
            nlohmann::json persisted = read_json(user_path);
            file_was_visible.store(persisted.is_object());
        } catch (const nlohmann::json::exception&) {
            file_was_visible.store(false);
        }
        apply_count.fetch_add(1);
    }));

    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"enable":false}})"));
    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"x":120}})"));
    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"y":240}})"));
    std::string snapshot;
    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(coordinator.snapshot_user_config(&snapshot, &error_code));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_SUCCESS));
    ASSERT_EQ(apply_count.load(), 1);
    ASSERT_TRUE(file_was_visible.load());
    ASSERT_EQ(nlohmann::json::parse(snapshot)["candidate_window"]["x"].get<int>(), 120);

    nlohmann::json saved = read_json(user_path);
    ASSERT_EQ(saved["candidate_window"]["enable"].get<bool>(), false);
    ASSERT_EQ(saved["candidate_window"]["x"].get<int>(), 120);
    ASSERT_EQ(saved["candidate_window"]["y"].get<int>(), 240);

    coordinator.stop();
    DeleteFileA(user_path.c_str());
}

TEST(ConfigWriteCoordinator, replacement_is_an_ordering_barrier) {
    const std::string user_path = test_user_config_path("barrier");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));

    std::atomic<int> apply_count{0};
    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(
        &store, [&](const std::shared_ptr<const cxxime::Config>&) { apply_count.fetch_add(1); }));

    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"x":1}})"));
    std::string runtime_json;
    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(coordinator.submit(cxxime::UserConfigMutationKind::kReplace,
                                   R"({"candidate_window":{"y":7},"engine":{"page_size":4}})",
                                   &runtime_json, &error_code));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_SUCCESS));
    ASSERT_EQ(apply_count.load(), 2);

    nlohmann::json saved = read_json(user_path);
    ASSERT_TRUE(!saved["candidate_window"].contains("x"));
    ASSERT_EQ(saved["candidate_window"]["y"].get<int>(), 7);
    nlohmann::json runtime = nlohmann::json::parse(runtime_json);
    ASSERT_EQ(runtime["engine"]["page_size"].get<int>(), 4);  // the runtime has known keys only

    coordinator.stop();
    DeleteFileA(user_path.c_str());
}

TEST(ConfigWriteCoordinator, invalid_patch_does_not_write_or_publish) {
    const std::string user_path = test_user_config_path("invalid");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));

    std::atomic<int> apply_count{0};
    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(
        &store, [&](const std::shared_ptr<const cxxime::Config>&) { apply_count.fetch_add(1); }));

    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(!coordinator.submit(cxxime::UserConfigMutationKind::kMergePatch, "{invalid",
                                    nullptr, &error_code));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_INVALID_DATA));
    ASSERT_EQ(apply_count.load(), 0);
    ASSERT_EQ(GetFileAttributesA(user_path.c_str()), INVALID_FILE_ATTRIBUTES);

    coordinator.stop();
}

TEST(ConfigWriteCoordinator, prepare_rejection_does_not_persist_or_apply) {
    const std::string user_path = test_user_config_path("prepare-rejected");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));

    std::atomic<int> apply_count{0};
    std::atomic<int> prepare_count{0};
    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(
        &store, [&](const std::shared_ptr<const cxxime::Config>&) { apply_count.fetch_add(1); },
        [&](const std::shared_ptr<const cxxime::Config>&, unsigned long* error_code) {
            prepare_count.fetch_add(1);
            *error_code = ERROR_HOTKEY_ALREADY_REGISTERED;
            return false;
        },
        []() {}));

    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(!coordinator.submit(cxxime::UserConfigMutationKind::kMergePatch,
                                    R"({"candidate_window":{"x":99}})", nullptr, &error_code));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_HOTKEY_ALREADY_REGISTERED));
    ASSERT_EQ(prepare_count.load(), 1);
    ASSERT_EQ(apply_count.load(), 0);
    ASSERT_EQ(GetFileAttributesA(user_path.c_str()), INVALID_FILE_ATTRIBUTES);

    coordinator.stop();
}

TEST(ConfigWriteCoordinator, commit_failure_cancels_prepared_runtime_change) {
    const std::string user_path = test_user_config_path("commit-failure");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));
    ASSERT_TRUE(CreateDirectoryA(user_path.c_str(), nullptr) != FALSE);

    std::atomic<int> apply_count{0};
    std::atomic<int> cancel_count{0};
    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(
        &store, [&](const std::shared_ptr<const cxxime::Config>&) { apply_count.fetch_add(1); },
        [](const std::shared_ptr<const cxxime::Config>&, unsigned long*) { return true; },
        [&]() { cancel_count.fetch_add(1); }));

    unsigned long error_code = ERROR_SUCCESS;
    ASSERT_TRUE(!coordinator.submit(cxxime::UserConfigMutationKind::kMergePatch,
                                    R"({"candidate_window":{"x":99}})", nullptr, &error_code));
    ASSERT_TRUE(error_code != ERROR_SUCCESS);
    ASSERT_EQ(apply_count.load(), 0);
    ASSERT_EQ(cancel_count.load(), 1);

    coordinator.stop();
    RemoveDirectoryA(user_path.c_str());
}

TEST(ConfigWriteCoordinator, stop_persists_accepted_patches) {
    const std::string user_path = test_user_config_path("stop-drain");
    ConfigStore store;
    std::shared_ptr<const cxxime::Config> initial;
    ASSERT_TRUE(initialize_store(&store, user_path, &initial));

    ConfigWriteCoordinator coordinator;
    ASSERT_TRUE(coordinator.start(&store, [](const std::shared_ptr<const cxxime::Config>&) {}));
    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"x":321}})"));
    ASSERT_TRUE(coordinator.enqueue_patch(R"({"candidate_window":{"y":654}})"));

    coordinator.stop();

    nlohmann::json saved = read_json(user_path);
    ASSERT_EQ(saved["candidate_window"]["x"].get<int>(), 321);
    ASSERT_EQ(saved["candidate_window"]["y"].get<int>(), 654);
    DeleteFileA(user_path.c_str());
}

TEST(ConfigStore, identifies_theme_configuration_failure) {
    const std::string user_path = test_user_config_path("theme-failure-user");
    const std::string themes_path = test_user_config_path("theme-failure-themes");
    DeleteFileA(user_path.c_str());
    {
        std::ofstream themes(themes_path);
        themes << "{invalid";
    }

    ConfigStore store;
    std::shared_ptr<const cxxime::Config> config;
    unsigned long error_code = ERROR_SUCCESS;
    ConfigStoreFailure failure = ConfigStoreFailure::kNone;
    ASSERT_TRUE(!store.initialize(std::string(CXXIME_DATA_DIR) + "default.json", user_path,
                                  themes_path, &config, &error_code, &failure));
    ASSERT_EQ(error_code, static_cast<unsigned long>(ERROR_INVALID_DATA));
    ASSERT_EQ(failure, ConfigStoreFailure::kThemes);

    DeleteFileA(themes_path.c_str());
}

RUN_ALL_TESTS()
