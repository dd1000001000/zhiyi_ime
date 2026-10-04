// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/wubi_processor.h>

#include <windows.h>

namespace cxxime {

ProcessResult WubiProcessor::process_key(const KeyEvent& event, Context& context) {
    if (event.is_key_up)
        return ProcessResult::REJECTED;

    uint32_t vk = event.keycode;

    // Escape: clear input
    if (vk == VK_ESCAPE) {
        context.reset();
        return ProcessResult::ACCEPTED;
    }

    if (context.edit_preedit(event)) {
        return ProcessResult::ACCEPTED;
    }

    // Space: select first candidate and commit
    if (vk == VK_SPACE) {
        if (context.is_composing() && context.candidate_count() > 0) {
            // 五笔模式：Space 选中第一候选（不高亮状态下）
            int index = context.highlighted() >= 0 ? context.highlighted() : 0;
            if (index < context.candidate_count()) {
                context.request_candidate_selection(index);
                return ProcessResult::CANDIDATE_SELECTED;
            }
        }
        if (context.is_composing()) {
            context.reset();
            return ProcessResult::ACCEPTED;
        }
        // 空输入时按 Space，透传给应用
        return ProcessResult::REJECTED;
    }

    // Up/Down arrows: navigate candidates
    if (vk == VK_UP || vk == VK_DOWN) {
        if (context.is_composing() && context.candidate_count() > 0) {
            if (vk == VK_DOWN) {
                context.move_to_next_candidate();
            } else {
                context.move_to_previous_candidate();
            }
            return ProcessResult::ACCEPTED;
        }
        return ProcessResult::REJECTED;
    }

    // Number keys 1-9: select candidate by index
    // 1-9 select the first nine candidates; 0 selects the tenth when the page has ten.
    if ((vk >= '1' && vk <= '9') ||
        (vk == '0' && context.is_composing() && context.selectable_candidate_count() >= 10)) {
        if (context.is_composing() && context.candidate_count() > 0) {
            int index = vk == '0' ? 9 : vk - '1';
            if (index < context.selectable_candidate_count()) {
                context.request_candidate_selection(index);
                return ProcessResult::CANDIDATE_SELECTED;
            }
            return ProcessResult::ACCEPTED;
        }
        return ProcessResult::REJECTED;
    }

    // Page Up / Page Down and unmodified - / =: pagination
    bool shortcut_page_up = vk == VK_OEM_MINUS && !event.is_shift() &&
                                  !event.is_ctrl() && !event.is_alt();
    bool shortcut_page_down = vk == VK_OEM_PLUS && !event.is_shift() &&
                                    !event.is_ctrl() && !event.is_alt();
    if (vk == VK_PRIOR || vk == VK_NEXT || shortcut_page_up || shortcut_page_down) {
        if (context.is_composing() && context.candidate_count() > 0) {
            if (vk == VK_NEXT || shortcut_page_down) {  // Page Down
                context.request_next_page();
            } else {  // Page Up
                context.request_previous_page();
            }
            return ProcessResult::ACCEPTED;
        }
        return ProcessResult::REJECTED;
    }

    // Letter keys: append to buffer
    if (vk >= 'A' && vk <= 'Z') {
        char ch = static_cast<char>(vk - 'A' + 'a');
        context.insert_preedit(ch);
        return ProcessResult::ACCEPTED;
    }

    // Other keys: reject
    return ProcessResult::REJECTED;
}

} // namespace cxxime
