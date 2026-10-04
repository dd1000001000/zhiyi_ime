// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_TOPK_COLLECTOR_H_
#define CXXIME_TOPK_COLLECTOR_H_

#include <vector>
#include <algorithm>
#include <cxxime/candidate.h>

namespace cxxime {

// Fixed-capacity collector that keeps the top-K candidates by frequency.
// Uses linear scan for min-replacement — no heap allocation beyond the
// internal vector.  K is typically small (< 100) so O(K) per offer() is fine.
class TopKCollector {
public:
    explicit TopKCollector(size_t k) : k_(k) { items_.reserve(k); }

    void offer(Candidate&& c) {
        if (k_ == 0) return;
        if (items_.size() < k_) {
            items_.push_back(std::move(c));
            return;
        }
        // Find the minimum-frequency item
        size_t min_i = 0;
        for (size_t i = 1; i < items_.size(); ++i) {
            if (items_[i].frequency < items_[min_i].frequency)
                min_i = i;
        }
        if (c.frequency > items_[min_i].frequency)
            items_[min_i] = std::move(c);
    }

    // Like offer(), but an item with the same text keeps the higher frequency.
    void offer_unique(Candidate&& c) {
        for (auto& item : items_) {
            if (item.text == c.text) {
                if (c.frequency > item.frequency) item = std::move(c);
                return;
            }
        }
        offer(std::move(c));
    }

    size_t size() const { return items_.size(); }
    bool full() const { return k_ > 0 && items_.size() >= k_; }
    const std::vector<Candidate>& items() const { return items_; }

    // Sort by frequency descending and move results out.
    std::vector<Candidate> finish() {
        std::sort(items_.begin(), items_.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.frequency > b.frequency;
            });
        return std::move(items_);
    }

private:
    size_t k_;
    std::vector<Candidate> items_;
};

} // namespace cxxime

#endif // CXXIME_TOPK_COLLECTOR_H_
