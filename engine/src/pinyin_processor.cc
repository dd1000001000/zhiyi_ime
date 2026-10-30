// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/processor.h>

#include <windows.h>

namespace cxxime {

// Symbol keys that belong to the code while composing, not punctuation: ; in Shuangpin (a zero
// initial in some schemes), the apostrophe separating syllables in full pinyin (xi'an).
bool PinyinProcessor::accepts_code_key(const KeyEvent& event, const Context& context) const {
    if (event.is_shift() || event.is_ctrl() || event.is_alt() || !context.is_composing()) {
        return false;
    }
    return event.keycode == (shuangpin_enabled_ ? VK_OEM_1 : VK_OEM_7);
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

    // Tab / Shift+Tab: round the candidates on the page
    if (is_cycle_key(event)) {
        if (context.is_composing() && context.candidate_count() > 0) {
            context.cycle_highlight(event.is_shift());
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

    // PageUp / PageDown and the configured page keys: pagination
    if (const PageKey page = page_key(event, context); page != PageKey::kNone) {
        if (context.is_composing() && context.candidate_count() > 0) {
            if (page == PageKey::kNext) {
                context.request_next_page();
            } else {
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

    // An apostrophe in full pinyin separates two syllables (xi'an: 西安, not 先), as in Rime and
    // Microsoft Pinyin; it was a Chinese quotation mark that committed the first candidate.
    // Only after a letter: not to start the input, nor twice in a row.
    if (accepts_code_key(event, context) && !shuangpin_enabled_) {
        const std::string& input = context.active_input();
        const std::size_t cursor = context.preedit_cursor();
        if (cursor > 0 && cursor <= input.size() && input[cursor - 1] != '\'' &&
            (cursor == input.size() || input[cursor] != '\'')) {
            context.insert_preedit('\'');
        }
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
