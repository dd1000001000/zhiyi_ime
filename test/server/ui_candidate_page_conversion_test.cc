// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cstring>

#include <cxxime/ui_protocol.h>

#include "ui_candidate_page_conversion.h"
#include "support/testutil.h"

TEST(UiCandidatePageConversion, preserves_snapshot_fields_and_candidate_text) {
    cxxime::UiPresentationSnapshot snapshot = {};
    snapshot.candidate_page.page_current = 2;
    snapshot.candidate_page.offset = 3;
    snapshot.candidate_page.count = 2;
    snapshot.candidate_page.highlighted = 1;
    snapshot.candidate_known_count = 8;
    snapshot.candidate_extent_state = cxxime::CandidateExtentState::kExhausted;
    snapshot.candidate_extent_complete = 1;

    const char text[] = "nihao";
    const char hint[] = "hint";
    snapshot.candidate_page.candidates[0].text_length = sizeof(text) - 1;
    snapshot.candidate_page.candidates[0].hint_length = sizeof(hint) - 1;
    std::memcpy(snapshot.candidate_page.candidates[0].text, text, sizeof(text) - 1);
    std::memcpy(snapshot.candidate_page.candidates[0].hint, hint, sizeof(hint) - 1);
    snapshot.candidate_page.candidates[1].text_length = 1;
    snapshot.candidate_page.candidates[1].hint_length = 0;
    snapshot.candidate_page.candidates[1].text[0] = 'x';

    const cxxime::CandidatePage page = cxxime::candidate_page_from_snapshot(snapshot);
    ASSERT_EQ(page.page_index, 1);
    ASSERT_EQ(page.page_offset, 3);
    ASSERT_EQ(page.page_size, 2);
    ASSERT_EQ(page.extent.known_count, 8);
    ASSERT_EQ(page.extent.state, cxxime::CandidateExtentState::kExhausted);
    ASSERT_TRUE(page.extent.complete);
    ASSERT_EQ(page.highlighted, 1);
    ASSERT_EQ(page.candidates.size(), 2u);
    ASSERT_EQ(page.candidates[0].text, "nihao");
    ASSERT_EQ(page.candidates[0].comment, "hint");
    ASSERT_EQ(page.candidates[1].text, "x");
    ASSERT_TRUE(page.candidates[1].comment.empty());
}

RUN_ALL_TESTS()
