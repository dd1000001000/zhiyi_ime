// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "candidate_placement.h"

#include <algorithm>

namespace cxxime {

void place_slot_requests(std::vector<SlotRequest> requests, std::vector<CandidateEntry>& entries,
                         int limit) {
    std::stable_sort(requests.begin(), requests.end(), [](const SlotRequest& a, const SlotRequest& b) {
        return static_cast<int>(a.priority) > static_cast<int>(b.priority);
    });
    std::vector<SlotRequest> kept;
    for (SlotRequest& request : requests) {
        const std::string& text = request.entry.candidate.text;
        if (text.empty() || std::any_of(kept.begin(), kept.end(), [&](const SlotRequest& k) {
                return k.entry.candidate.text == text;
            })) {
            continue;
        }
        kept.push_back(std::move(request));
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const CandidateEntry& entry) {
                                     return std::any_of(kept.begin(), kept.end(), [&](const SlotRequest& k) {
                                         return k.entry.candidate.text == entry.candidate.text;
                                     });
                                 }),
                  entries.end());

    if (!kept.empty()) {
        // Every place is filled: the requests take theirs, the ranked candidates the rest.
        const std::size_t total = entries.size() + kept.size();
        std::vector<int> owner(total, -1);
        for (std::size_t k = 0; k < kept.size(); ++k) {
            std::size_t place = (std::min)(static_cast<std::size_t>((std::max)(1, kept[k].slot) - 1),
                                           total - 1);
            while (place < total && owner[place] >= 0) ++place;
            if (place == total) {  // past the end: the last free place
                place = total - 1;
                while (owner[place] >= 0) --place;
            }
            owner[place] = static_cast<int>(k);
        }
        std::vector<CandidateEntry> placed;
        placed.reserve(total);
        std::size_t next = 0;
        for (std::size_t place = 0; place < total; ++place) {
            if (owner[place] >= 0) {
                placed.push_back(std::move(kept[static_cast<std::size_t>(owner[place])].entry));
            } else {
                placed.push_back(std::move(entries[next++]));
            }
        }
        entries = std::move(placed);
    }
    if (limit > 0 && static_cast<int>(entries.size()) > limit) {
        entries.resize(static_cast<std::size_t>(limit));
    }
}

}  // namespace cxxime
