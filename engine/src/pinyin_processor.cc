// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/processor.h>

#include <windows.h>

namespace cxxime {

bool PinyinProcessor::accepts_code_key(const KeyEvent& event, const Context& context) const {
    return shuangpin_enabled_ && event.keycode == VK_OEM_1 && !event.is_shift() &&
           !event.is_ctrl() && !event.is_alt() && context.is_composing();
}

ProcessResult PinyinProcessor::process_key(const KeyEvent& event, Context& context) {
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

    // Space: select first candidate, or dismiss if no candidates
    if (vk == VK_SPACE) {
        if (context.is_composing() &&
            context.commit_source() == CommitSource::kRawCodePreserveCase) {
            return context.finalize_raw(CommitSource::kRawCodePreserveCase)
                       ? ProcessResult::COMMITTED
                       : ProcessResult::ACCEPTED;
        }
        if (context.is_composing() && context.candidate_count() > 0) {
            const int index = context.highlighted();
            if (index >= 0 && index < context.candidate_count()) {
                context.request_candidate_selection(index);
                return ProcessResult::CANDIDATE_SELECTED;
            }
        }
        if (context.is_composing()) {
            // Append mode: commit buffer text instead of discarding
            if (event.is_caps_lock() && context.caps_lock_style == AsciiModeSwitchStyle::APPEND) {
                return context.finalize_raw(CommitSource::kRawCodePreserveCase)
                           ? ProcessResult::COMMITTED
                           : ProcessResult::ACCEPTED;
            }
            context.reset();
            return ProcessResult::ACCEPTED;
        }
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
    if ((is_digit_key(vk) && vk >= '1' && vk <= '9') ||
        (vk == '0' && context.is_composing() && context.selectable_candidate_count() >= 10)) {
        if (context.is_composing() && context.candidate_count() > 0) {
            int index = vk == '0' ? 9 : vk - '1';
            if (index < context.selectable_candidate_count()) {
                context.request_candidate_selection(index);
                return ProcessResult::CANDIDATE_SELECTED;
            }
            return ProcessResult::ACCEPTED;
        }
        // If not composing, let the number pass through
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

    // Letter keys: append to pinyin buffer
    if (is_letter_key(vk)) {
        char ch;
        // Append mode with CapsLock: preserve original case
        if (event.is_caps_lock() && context.caps_lock_style == AsciiModeSwitchStyle::APPEND) {
            ch = static_cast<char>(vk);  // uppercase VK code
            if (event.is_shift())
                ch = static_cast<char>(vk - 'A' + 'a');  // Shift+CapsLock → lowercase
            context.set_commit_source(CommitSource::kRawCodePreserveCase);
            context.clear_translation();
            context.reset_pagination();
        } else {
            ch = static_cast<char>(vk - 'A' + 'a');  // force lowercase
        }
        context.insert_preedit(ch);
        return ProcessResult::ACCEPTED;
    }

    if (accepts_code_key(event, context)) {
        context.insert_preedit(';');
        return ProcessResult::ACCEPTED;
    }

    // Other keys: reject (pass to system)
    return ProcessResult::REJECTED;
}

} // namespace cxxime
