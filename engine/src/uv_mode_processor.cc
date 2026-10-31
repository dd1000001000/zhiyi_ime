// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/uv_mode_processor.h>

#include <windows.h>

#include <cxxime/input_limits.h>
#include <cxxime/uv_mode.h>

namespace cxxime {
namespace {

bool v_mode(const Context& context) {
    return !context.active_input().empty() && context.active_input()[0] == kVModePrefix;
}

ProcessResult choose(Context& context, int index) {
    if (index < 0 || index >= context.selectable_candidate_count()) return ProcessResult::ACCEPTED;
    context.request_candidate_selection(index);
    return ProcessResult::CANDIDATE_SELECTED;
}

ProcessResult insert(Context& context, char ch) {
    if (context.active_input().size() < kMaxInputCodeLength) context.insert_preedit(ch);
    return ProcessResult::ACCEPTED;
}

}  // namespace

bool UvModeProcessor::is_active(const Context& context) {
    return context.composition_scheme() == CompositionScheme::kUvMode;
}

bool UvModeProcessor::digits_are_input(const Context& context) {
    return is_active(context) && v_mode(context);
}

ProcessResult UvModeProcessor::process_key(const KeyEvent& event, std::optional<char> ch,
                                           Context& context) const {
    if (event.is_key_up || !is_active(context)) return ProcessResult::REJECTED;
    const uint32_t vk = event.keycode;
    const bool v = v_mode(context);

    switch (vk) {
    case VK_ESCAPE:
        context.reset();
        return ProcessResult::ACCEPTED;
    case VK_BACK:
        context.erase_preedit_before_cursor();
        // The u or v gone: the mode ends.
        if (context.active_input().empty() || context.preedit_cursor() == 0) context.reset();
        return ProcessResult::ACCEPTED;
    case VK_DELETE:
        context.erase_preedit_at_cursor();
        return ProcessResult::ACCEPTED;
    case VK_LEFT:
        if (context.preedit_cursor() > 1) context.move_preedit_cursor_left();
        return ProcessResult::ACCEPTED;
    case VK_RIGHT:
        context.move_preedit_cursor_right();
        return ProcessResult::ACCEPTED;
    case VK_HOME:
        while (context.preedit_cursor() > 1) context.move_preedit_cursor_left();
        return ProcessResult::ACCEPTED;
    case VK_END:
        context.move_preedit_cursor_end();
        return ProcessResult::ACCEPTED;
    case VK_SPACE:
        if (context.candidate_count() > 0)
            return choose(context, context.highlighted() >= 0 ? context.highlighted() : 0);
        // Nothing to choose: the letters as typed.
        return context.finalize_raw(CommitSource::kRawCodePreserveCase) ? ProcessResult::COMMITTED
                                                                         : ProcessResult::ACCEPTED;
    case VK_UP:
    case VK_DOWN:
        if (context.candidate_count() > 0) {
            if (vk == VK_DOWN) context.move_to_next_candidate();
            else context.move_to_previous_candidate();
        }
        return ProcessResult::ACCEPTED;
    default:
        break;
    }
    if (is_cycle_key(event)) {
        context.cycle_highlight(event.is_shift());
        return ProcessResult::ACCEPTED;
    }

    if (vk >= 'A' && vk <= 'Z') {
        const char letter = static_cast<char>(vk - 'A' + 'a');
        // V mode labels its results a, b, c ... once there is something to choose.
        if (v && context.translation().letter_labels) {
            const int index = letter - 'a';
            if (index < context.selectable_candidate_count()) return choose(context, index);
        }
        return insert(context, letter);
    }
    if (ch && *ch >= '0' && *ch <= '9') {
        if (v) return insert(context, *ch);
        return choose(context, *ch == '0' ? 9 : *ch - '1');
    }
    if (ch && v && is_v_mode_symbol(*ch)) return insert(context, *ch);
    if (ch && !v && *ch == '\'') return insert(context, *ch);

    if (const PageKey page = page_key(event, context); page != PageKey::kNone) {
        if (context.candidate_count() > 0) {
            if (page == PageKey::kNext) context.request_next_page();
            else context.request_previous_page();
        }
        return ProcessResult::ACCEPTED;
    }
    return ProcessResult::REJECTED;
}

}  // namespace cxxime
