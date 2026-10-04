// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <string>
#include <tuple>
#include <vector>

#include <windows.h>

#include <cxxime/dict.h>
#include <cxxime/query_trace.h>

#include "support/testutil.h"
#include "support/wubi_index_test_data.h"

namespace {

std::string temp_path(const char* filename) {
    char directory[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, directory);
    return std::string(directory) + filename;
}

} // namespace

TEST(WubiPrefixQuery, ranking_is_independent_of_query_limit) {
    const std::string dict_path = temp_path("zhiyi_wubi_indexed_ranking.bin");
    const std::string index_path = dict_path + ".idx";
    const std::string user_path = dict_path + ".user.tsv";
    const std::vector<std::tuple<std::string, std::string, int>> entries = {
        {"d", "exact-primary", 20},
        {"d", "exact-secondary", 10},
        {"da", "left", 10},
        {"daaa", "long-high-frequency", 10000},
        {"db", "care", 10},
        {"dc", "friend", 10},
        {"dd", "large", 10},
        {"de", "beard", 10},
    };
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, entries));
    ASSERT_TRUE(cxxime::test::create_test_wubi_index(index_path, entries));
    DeleteFileA(user_path.c_str());

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open_wubi_bundle(dict_path, user_path, index_path));
    ASSERT_TRUE(dict.has_wubi_prefix_index());

    const auto narrow = dict.lookup("d", 5);
    const auto wide = dict.lookup("d", 20);
    ASSERT_EQ(narrow.size(), 5u);
    ASSERT_GE(wide.size(), narrow.size());
    for (size_t index = 0; index < narrow.size(); ++index) {
        ASSERT_EQ(narrow[index].text, wide[index].text);
    }
    ASSERT_EQ(narrow[0].text, "exact-primary");
    ASSERT_EQ(narrow[1].text, "exact-secondary");
    ASSERT_EQ(narrow[2].text, "left");
    ASSERT_EQ(narrow[3].text, "care");
    ASSERT_EQ(narrow[4].text, "friend");

    dict.close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(index_path.c_str());
    DeleteFileA(user_path.c_str());
}

TEST(WubiPrefixQuery, disabled_filter_has_a_fixed_posting_scan_bound) {
    const std::string dict_path = temp_path("zhiyi_wubi_disabled_bound.bin");
    const std::string index_path = dict_path + ".idx";
    const std::string user_path = dict_path + ".user.tsv";
    const std::string disabled_path = dict_path + ".disabled.tsv";
    std::vector<std::tuple<std::string, std::string, int>> entries;
    for (int index = 0; index < 50; ++index) {
        entries.push_back({"a", "entry-" + std::to_string(index), 1000 - index});
    }
    ASSERT_TRUE(cxxime::Dict::create_test_dict(dict_path, entries));
    ASSERT_TRUE(cxxime::test::create_test_wubi_index(index_path, entries));
    DeleteFileA(user_path.c_str());
    DeleteFileA(disabled_path.c_str());

    cxxime::Dict dict{cxxime::UserDictKind::WUBI};
    ASSERT_TRUE(dict.open_wubi_bundle(dict_path, user_path, index_path));
    ASSERT_TRUE(dict.load_disabled_system_entries(disabled_path));
    for (int index = 0; index < 20; ++index) {
        ASSERT_TRUE(dict.disable_system_entry("entry-" + std::to_string(index)));
    }

    cxxime::QueryTrace trace;
    const auto results = dict.lookup("a", 10, &trace);
    ASSERT_EQ(results.size(), 6u);
    ASSERT_EQ(trace.prefix_scan_count, 26u);

    dict.close();
    DeleteFileA(dict_path.c_str());
    DeleteFileA(index_path.c_str());
    DeleteFileA(user_path.c_str());
    DeleteFileA(disabled_path.c_str());
}

RUN_ALL_TESTS()
