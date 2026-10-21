// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/symbol_processor.h>

#include <windows.h>

#include <cxxime/symbol_table.h>

namespace cxxime {

bool SymbolProcessor::is_active(const Context& context) {
    return context.composition_scheme() == CompositionScheme::kSymbol;
}

bool SymbolProcessor::is_trigger(const KeyEvent& event) {
    return !event.is_key_up &&
           event.keycode == VK_OEM_2 &&
           !event.is_shift() &&
           !event.is_ctrl() &&
           !event.is_alt();
}

bool SymbolProcessor::edit_preedit(const KeyEvent& event, Context& context) {
    switch (event.keycode) {
    case VK_BACK:
        context.erase_preedit_before_cursor();
        if (context.active_input().empty()) {
            context.reset();
        }
        return true;
    case VK_DELETE:
        context.erase_preedit_at_cursor();
        return true;
    case VK_LEFT:
        if (context.preedit_cursor() > 1) {
            context.move_preedit_cursor_left();
        }
        return true;
    case VK_RIGHT:
        context.move_preedit_cursor_right();
        return true;
    case VK_HOME:
        while (context.preedit_cursor() > 1) {
            context.move_preedit_cursor_left();
        }
        return true;
    case VK_END:
        context.move_preedit_cursor_end();
        return true;
    default:
        return false;
    }
}

ProcessResult SymbolProcessor::select_candidate(Context& context, int index) {
    if (index < 0 || index >= context.selectable_candidate_count()) {
        return ProcessResult::ACCEPTED;
    }

    context.request_candidate_selection(index);
    return ProcessResult::CANDIDATE_SELECTED;
}

ProcessResult SymbolProcessor::process_key(const KeyEvent& event, Context& context,
                                           bool allow_trigger) const {
    if (event.is_key_up) {
        return ProcessResult::REJECTED;
    }

    if (!is_active(context)) {
        if (!allow_trigger || !is_trigger(event) || context.is_composing()) {
            return ProcessResult::REJECTED;
        }
        context.start_composition(
            CompositionScheme::kSymbol, std::string(1, kSymbolPrefix), 1);
        context.clear_translation();
        context.reset_pagination();
        return ProcessResult::ACCEPTED;
    }

    if (event.keycode == VK_ESCAPE) {
        context.reset();
        return ProcessResult::ACCEPTED;
    }
    if (edit_preedit(event, context)) {
        return ProcessResult::ACCEPTED;
    }

    if (event.keycode == VK_SPACE) {
        if (context.candidate_count() > 0) {
            const int index = context.highlighted() >= 0 ? context.highlighted() : 0;
            return select_candidate(context, index);
        }
        context.reset();
        return ProcessResult::ACCEPTED;
    }

    if (is_cycle_key(event)) {
        context.cycle_highlight(event.is_shift());
        return ProcessResult::ACCEPTED;
    }

    if (event.keycode == VK_UP || event.keycode == VK_DOWN) {
        if (context.candidate_count() > 0) {
            if (event.keycode == VK_DOWN) {
                context.move_to_next_candidate();
            } else {
                context.move_to_previous_candidate();
            }
        }
        return ProcessResult::ACCEPTED;
    }

    if (event.keycode >= '1' && event.keycode <= '9' && !event.is_shift() &&
        !event.is_ctrl() && !event.is_alt()) {
        if (context.candidate_count() > 0) {
            const int index = static_cast<int>(event.keycode - '1');
            return select_candidate(context, index);
        }
        context.insert_preedit(static_cast<char>(event.keycode));
        return ProcessResult::ACCEPTED;
    }

    if (const PageKey page = page_key(event, context); page != PageKey::kNone) {
        if (context.candidate_count() > 0) {
            if (page == PageKey::kNext) {
                context.request_next_page();
            } else {
                context.request_previous_page();
            }
            return ProcessResult::ACCEPTED;
        }
        return ProcessResult::REJECTED;
    }

    if (event.keycode >= 'A' && event.keycode <= 'Z') {
        context.insert_preedit(static_cast<char>(event.keycode - 'A' + 'a'));
        return ProcessResult::ACCEPTED;
    }

    return ProcessResult::REJECTED;
}

} // namespace cxxime
