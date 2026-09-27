// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_UI_CANDIDATE_PAGE_CONVERSION_H_
#define CXXIME_UI_CANDIDATE_PAGE_CONVERSION_H_

#include <cxxime/candidate.h>
#include <cxxime/ui_protocol.h>

namespace cxxime {

// Construct the UI page directly from the validated packet snapshot.
CandidatePage candidate_page_from_snapshot(const UiPresentationSnapshot& snapshot);

} // namespace cxxime

#endif // CXXIME_UI_CANDIDATE_PAGE_CONVERSION_H_
