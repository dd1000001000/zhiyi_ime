// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include <cxxime/pinyin_scheme.h>

#include "support/testutil.h"

TEST(PinyinScheme, built_in_descriptors_are_stable) {
    struct ExpectedScheme {
        const char* id;
        cxxime::PinyinSchemeKind kind;
        const char* role;
        const char* filename;
        const char* input_example;
    };
    const ExpectedScheme expected[] = {
        {"full_pinyin", cxxime::PinyinSchemeKind::kFullPinyin, "pinyin_spellings",
         "pinyin.spellings.bin", "ni'hao"},
    };

    const auto& schemes = cxxime::built_in_pinyin_schemes();
    ASSERT_EQ(schemes.size(), sizeof(expected) / sizeof(expected[0]));
    for (std::size_t i = 0; i < schemes.size(); ++i) {
        ASSERT_EQ(std::string(schemes[i].id), expected[i].id);
        ASSERT_EQ(schemes[i].kind, expected[i].kind);
        ASSERT_EQ(std::string(schemes[i].manifest_role), expected[i].role);
        ASSERT_EQ(std::string(schemes[i].spelling_filename), expected[i].filename);
        ASSERT_EQ(std::string(schemes[i].input_example), expected[i].input_example);
        ASSERT_TRUE(std::string(schemes[i].input_example).find('\'') != std::string::npos);
    }
    // Full pinyin is the only scheme; removed shuangpin ids fall back to it.
    ASSERT_EQ(cxxime::normalize_pinyin_scheme_id("unknown"), "full_pinyin");
    ASSERT_EQ(cxxime::normalize_pinyin_scheme_id("xiaohe_shuangpin"), "full_pinyin");
}

TEST(PinyinScheme, built_in_descriptors_are_complete_and_unique) {
    std::vector<std::string> ids;
    std::vector<std::string> roles;
    std::vector<std::string> filenames;
    for (const auto& scheme : cxxime::built_in_pinyin_schemes()) {
        ASSERT_TRUE(scheme.id && *scheme.id);
        ASSERT_TRUE(scheme.display_name && *scheme.display_name);
        ASSERT_TRUE(scheme.manifest_role && *scheme.manifest_role);
        ASSERT_TRUE(scheme.spelling_filename && *scheme.spelling_filename);
        ASSERT_TRUE(scheme.input_example && *scheme.input_example);
        ASSERT_TRUE(std::find(ids.begin(), ids.end(), scheme.id) == ids.end());
        ASSERT_TRUE(std::find(roles.begin(), roles.end(), scheme.manifest_role) == roles.end());
        ASSERT_TRUE(std::find(filenames.begin(), filenames.end(), scheme.spelling_filename) ==
                    filenames.end());
        ids.emplace_back(scheme.id);
        roles.emplace_back(scheme.manifest_role);
        filenames.emplace_back(scheme.spelling_filename);
    }
}
