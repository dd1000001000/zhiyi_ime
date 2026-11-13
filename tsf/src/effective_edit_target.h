// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TSF_EFFECTIVE_EDIT_TARGET_H_
#define CXXIME_TSF_EFFECTIVE_EDIT_TARGET_H_

#include <cstdint>

namespace cxxime_tsf {

struct EffectiveEditTargetSnapshot {
    std::uintptr_t document_identity = 0;
    std::uintptr_t context_identity = 0;
    std::uintptr_t view_window = 0;
    bool editable = false;

    bool valid() const { return editable && document_identity != 0 && context_identity != 0; }
};

struct EffectiveEditTargetBindings {
    bool has_bound_resources = false;
    bool input_state_matches = true;
    bool target_resources_match = true;
    bool edit_sink_matches = true;
    bool layout_sink_matches = true;
    bool candidate_document_matches = true;

    bool healthy() const {
        return input_state_matches && target_resources_match && edit_sink_matches &&
               layout_sink_matches && candidate_document_matches;
    }
};

enum class EffectiveEditTargetAction {
    kUnchanged,
    kRebind,
    kRepairUi,
    kClear,
};

bool same_effective_edit_target(const EffectiveEditTargetSnapshot& current,
                                const EffectiveEditTargetSnapshot& next);

EffectiveEditTargetAction
classify_effective_edit_target_change(const EffectiveEditTargetSnapshot& current,
                                      const EffectiveEditTargetSnapshot& next,
                                      const EffectiveEditTargetBindings& bindings);

const char* effective_edit_target_action_name(EffectiveEditTargetAction action);

// The composition's target kept over a document focus gap (the focus on no document for a
// moment, #45): the focus coming back to it resumes the composition. The same document and
// context, whatever its view window (Excel shows the cell editor in another window).
bool resumes_after_focus_gap(const EffectiveEditTargetSnapshot& kept,
                             const EffectiveEditTargetSnapshot& next);

} // namespace cxxime_tsf

#endif // CXXIME_TSF_EFFECTIVE_EDIT_TARGET_H_
