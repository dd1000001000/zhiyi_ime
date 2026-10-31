// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Candidates at fixed places on the page (docs/candidate-order.md): the user's custom phrases
// (phrases.ini, "dh,1=..."), the date and time (rq / sj / xq) and an English word typed in
// Chinese mode (english.position_in_pinyin). They go in after the ranking (the translator's
// order, learning, the user's manual order, the context model), which never sees or moves them,
// on every query of the first page alike, so paging (which re-queries it and continues after
// the last candidate shown) finds them where they were.
#ifndef CXXIME_CANDIDATE_PLACEMENT_H_
#define CXXIME_CANDIDATE_PLACEMENT_H_

#include <vector>

#include <cxxime/translation_result.h>

namespace cxxime {

// Who wins a slot two requests ask for.
enum class PlacementPriority : int {
    kEnglish = 1,
    kBuiltinPhrase = 2,  // rq / sj / xq
    kUserPhrase = 3,     // phrases.ini
};

struct SlotRequest {
    CandidateEntry entry;
    int slot = 1;  // the place on the page, from 1, counting every candidate
    PlacementPriority priority = PlacementPriority::kEnglish;
};

// Puts each request at its slot, the highest priority first (equal ones in order); a slot taken
// passes to the next free one, and one past the end goes last. A ranked candidate with the text
// of a request goes (shown once), as does a lower request with the text of a higher one. The
// ranked candidates fill the other places in their order; the list is then cut to `limit` (0:
// not cut): the candidates pushed out reappear on the next page.
void place_slot_requests(std::vector<SlotRequest> requests, std::vector<CandidateEntry>& entries,
                         int limit);

}  // namespace cxxime

#endif  // CXXIME_CANDIDATE_PLACEMENT_H_
