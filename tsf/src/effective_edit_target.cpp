// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "effective_edit_target.h"

namespace cxxime_tsf {

bool same_effective_edit_target(const EffectiveEditTargetSnapshot& current,
                                const EffectiveEditTargetSnapshot& next) {
    return current.valid() && next.valid() &&
           current.document_identity == next.document_identity &&
           current.context_identity == next.context_identity &&
           current.view_window == next.view_window;
}

EffectiveEditTargetAction
classify_effective_edit_target_change(const EffectiveEditTargetSnapshot& current,
                                      const EffectiveEditTargetSnapshot& next,
                                      const EffectiveEditTargetBindings& bindings) {
    if (!next.valid()) {
        return current.valid() || bindings.has_bound_resources
                   ? EffectiveEditTargetAction::kClear
                   : EffectiveEditTargetAction::kUnchanged;
    }
    if (!same_effective_edit_target(current, next)) {
        return EffectiveEditTargetAction::kRebind;
    }
    if (!bindings.healthy()) {
        return EffectiveEditTargetAction::kRepairUi;
    }
    return EffectiveEditTargetAction::kUnchanged;
}

bool resumes_after_focus_gap(const EffectiveEditTargetSnapshot& kept,
                             const EffectiveEditTargetSnapshot& next) {
    return kept.valid() && next.valid() && kept.document_identity == next.document_identity &&
           kept.context_identity == next.context_identity;
}

const char* effective_edit_target_action_name(EffectiveEditTargetAction action) {
    switch (action) {
    case EffectiveEditTargetAction::kUnchanged:
        return "unchanged";
    case EffectiveEditTargetAction::kRebind:
        return "rebind";
    case EffectiveEditTargetAction::kRepairUi:
        return "repair_ui";
    case EffectiveEditTargetAction::kClear:
        return "clear";
    }
    return "unknown";
}

} // namespace cxxime_tsf
