// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <string>

#include <cxxime/ipc_protocol.h>

#include "ipc_response_builder.h"
#include "support/testutil.h"

TEST(IpcResponseBuilder, serializes_complete_segmented_presentation) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = "华锐jishu";
    result.preedit_cursor = result.preedit.size();
    result.converted_prefix_bytes = std::string("华锐").size();
    result.candidate_revision = 9;
    result.presentation.page_index = 1;
    result.presentation.page_offset = 5;
    result.presentation.page_size = 5;
    result.presentation.extent.known_count = 11;
    result.presentation.extent.state = cxxime::CandidateExtentState::kHasMore;
    result.presentation.highlighted = 1;
    result.presentation.items.push_back({"技术", "a"});

    cxxime::IPCResponse response = {};
    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(std::string(response.preedit), result.preedit);
    ASSERT_EQ(response.preedit_cursor, static_cast<uint32_t>(result.preedit.size()));
    ASSERT_EQ(response.converted_prefix_bytes, static_cast<uint32_t>(std::string("华锐").size()));
    ASSERT_EQ(response.focused_preedit_start_bytes,
              static_cast<uint32_t>(std::string("华锐").size()));
    ASSERT_EQ(response.focused_preedit_end_bytes,
              static_cast<uint32_t>(result.preedit.size()));
    ASSERT_EQ(response.candidate_revision, 9u);
    ASSERT_EQ(response.candidate_count, 1u);
    ASSERT_EQ(std::string(response.candidates[0]), "技术");
    ASSERT_EQ(std::string(response.candidate_hints[0]), "a");
    ASSERT_EQ(response.candidate_offset, 5u);
    ASSERT_EQ(response.candidate_total, 11u);
    ASSERT_EQ(response.candidate_known_count, 11u);
    ASSERT_EQ(response.candidate_extent_state, cxxime::CandidateExtentState::kHasMore);
    ASSERT_EQ(response.candidate_extent_complete, 1u);
    ASSERT_EQ(response.page_current, 2u);
    ASSERT_EQ(response.page_total, 3u);
    ASSERT_EQ(response.highlighted, 1u);
}

// A fuzzy pinyin annotation does not fit the 4-byte hint field; it used to fail the whole
// response, so the key went on to the application ("woshi" -> "i" in the document).
TEST(IpcResponseBuilder, long_candidate_hints_travel_as_comments) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = "wo'shi";
    result.preedit_cursor = result.preedit.size();
    result.presentation.page_size = 7;
    result.presentation.items.push_back({"我是", ""});
    result.presentation.items.push_back({"我司", "wo si"});
    result.presentation.items.push_back({"字", "abc"});
    const std::string long_hint(80, 'a');
    result.presentation.items.push_back({"长", long_hint});

    cxxime::IPCResponse response = {};
    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(response.candidate_count, 4u);
    ASSERT_EQ(std::string(response.candidate_comments[1]), "wo si");
    ASSERT_EQ(std::string(response.candidate_hints[1]), "");  // does not fit: left empty
    ASSERT_EQ(std::string(response.candidate_comments[2]), "abc");
    ASSERT_EQ(std::string(response.candidate_hints[2]), "abc");
    ASSERT_EQ(std::string(response.candidate_comments[3]).size(),
              cxxime::kCandidateCommentCapacity - 1);
}

TEST(IpcResponseBuilder, serializes_explicit_focused_preedit_range) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = "ji'shu";
    result.preedit_cursor = result.preedit.size();
    result.focused_preedit_end_bytes = 2;
    result.preedit_presentation_flags = cxxime::preedit_presentation_flag(
        cxxime::PreeditPresentationFlag::SyllableBoundaries);

    cxxime::IPCResponse response = {};
    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(response.focused_preedit_start_bytes, static_cast<uint32_t>(0));
    ASSERT_EQ(response.focused_preedit_end_bytes, static_cast<uint32_t>(2));
    ASSERT_EQ(response.preedit_presentation_flags, result.preedit_presentation_flags);
}

TEST(IpcResponseBuilder, indeterminate_short_page_projects_a_next_page) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = "huaruijishu";
    result.preedit_cursor = result.preedit.size();
    result.presentation.page_size = 7;
    result.presentation.extent.known_count = 4;
    result.presentation.extent.state = cxxime::CandidateExtentState::kIndeterminate;
    result.presentation.extent.complete = false;
    result.presentation.highlighted = 0;
    for (int index = 0; index < 4; ++index) {
        result.presentation.items.push_back({"candidate" + std::to_string(index), {}});
    }

    cxxime::IPCResponse response = {};
    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::OK);
    ASSERT_EQ(response.candidate_count, 4u);
    ASSERT_EQ(response.candidate_known_count, 4u);
    ASSERT_EQ(response.candidate_extent_state, cxxime::CandidateExtentState::kIndeterminate);
    ASSERT_EQ(response.candidate_extent_complete, 0u);
    ASSERT_EQ(response.page_current, 1u);
    ASSERT_EQ(response.page_total, 2u);
}

TEST(IpcResponseBuilder, rejects_non_utf8_preedit_boundaries) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = "华a";
    result.preedit_cursor = 1;
    result.converted_prefix_bytes = 3;

    cxxime::IPCResponse response = {};
    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
    ASSERT_EQ(response.candidate_count, 0u);
}

TEST(IpcResponseBuilder, rejects_converted_prefix_after_the_cursor) {
    ProcessKeyResult result;
    result.status = cxxime::IPCStatus::OK;
    result.composing = true;
    result.preedit = u8"华锐jishu";
    result.preedit_cursor = 0;
    result.converted_prefix_bytes = std::string(u8"华锐").size();
    cxxime::IPCResponse response;

    fill_process_response(result, &response);

    ASSERT_EQ(response.status, cxxime::IPCStatus::ERR_ENGINE_PROCESS_FAILED);
}
