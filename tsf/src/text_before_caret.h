// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Reads the text before the caret of an input box, for the context the recommendation model
// uses (sent to the server with SET_CONTEXT when a composition starts).

#ifndef ZHIYU_TSF_TEXT_BEFORE_CARET_H_
#define ZHIYU_TSF_TEXT_BEFORE_CARET_H_

#include "pch.h"

#include <cstddef>
#include <string>

enum class TextBeforeCaret {
    kRead,         // `text` holds up to max_chars characters before the caret (or the selection)
    kPrivate,      // a password / private field: never read
    kUnavailable,  // the app did not let us read it, or its document is transitory (classic
                   // Win32 edit boxes; not Chromium's): the remembered context is used instead
};

// A classic Win32 program reaching TSF through the IMM compatibility layer: its transitory
// document holds only the composition (Chromium's transitory documents are real ones).
bool context_is_imm_document(ITfContext* context);

// A synchronous read-only edit session; call it while handling a key.
TextBeforeCaret read_text_before_caret(ITfContext* context, TfClientId client_id, size_t max_chars,
                                       std::wstring* text);

// As read_text_before_caret, but reads the selected text itself (empty when nothing is selected):
// what the selection translation translates.
TextBeforeCaret read_selection_text(ITfContext* context, TfClientId client_id, size_t max_chars,
                                    std::wstring* text);

#endif  // ZHIYU_TSF_TEXT_BEFORE_CARET_H_
