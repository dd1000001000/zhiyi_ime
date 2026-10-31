// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_ENGINE_RESPONSE_H_
#define CXXIME_TSF_ENGINE_RESPONSE_H_

#include <cstddef>
#include <string>

#include <cxxime/candidate_presentation.h>
#include <cxxime/ipc_protocol.h>

#include "preedit_mode.h"

namespace cxxime_tsf {

struct DecodedEnginePresentation {
    PreeditText logical_preedit;
    PreeditText display_preedit;
    std::size_t display_focused_preedit_start_bytes = 0;
    std::size_t display_focused_preedit_end_bytes = 0;
    bool has_syllable_boundaries = false;
    bool letter_labels = false;  // V mode: candidates labelled a, b, c ...
    cxxime::CandidatePresentationPage candidates;
};

bool decode_engine_presentation(const cxxime::IPCResponse& response,
                                DecodedEnginePresentation* presentation);
bool decode_engine_commit_text(const cxxime::IPCResponse& response, std::wstring* commit_text);

} // namespace cxxime_tsf

#endif // CXXIME_TSF_ENGINE_RESPONSE_H_
