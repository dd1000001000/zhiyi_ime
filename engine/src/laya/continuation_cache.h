// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// The continuation head's scores of recent compositions (laya_rerank.cc), bounded in bytes: a
// composition of n syllables holds n x the vocabulary's characters (6163) floats, about 25 KB a
// syllable. Two generations: when the newer one would pass the limit it becomes the older one,
// so scores just computed in the background are still there when the page is composed again.
// Not thread safe (the caller locks).
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace laya {

class ContinuationCache {
 public:
  using Scores = std::shared_ptr<const std::vector<std::vector<float>>>;

  explicit ContinuationCache(size_t generation_bytes) : limit_(generation_bytes) {}

  // A score found in the older generation moves to the newer one: it is in use.
  Scores find(const std::string& key) {
    auto it = newer_.find(key);
    if (it != newer_.end()) return it->second;
    it = older_.find(key);
    if (it == older_.end()) return nullptr;
    Scores scores = it->second;
    older_bytes_ -= bytes_of(key, scores);
    older_.erase(it);
    insert(key, scores);
    return scores;
  }

  void insert(const std::string& key, Scores scores) {
    const size_t bytes = bytes_of(key, scores);
    auto it = newer_.find(key);
    if (it != newer_.end()) {
      newer_bytes_ -= bytes_of(key, it->second);
      newer_.erase(it);
    }
    if (!newer_.empty() && newer_bytes_ + bytes > limit_) {
      older_ = std::move(newer_);
      older_bytes_ = newer_bytes_;
      newer_.clear();
      newer_bytes_ = 0;
    }
    newer_bytes_ += bytes;
    newer_.emplace(key, std::move(scores));
  }

  void clear() {
    newer_.clear();
    older_.clear();
    newer_bytes_ = older_bytes_ = 0;
  }

  size_t bytes() const { return newer_bytes_ + older_bytes_; }

 private:
  static size_t bytes_of(const std::string& key, const Scores& scores) {
    size_t bytes = key.size();
    if (scores) {
      for (const auto& slot : *scores) bytes += slot.size() * sizeof(float);
    }
    return bytes;
  }

  size_t limit_;
  std::unordered_map<std::string, Scores> newer_, older_;
  size_t newer_bytes_ = 0;
  size_t older_bytes_ = 0;
};

}  // namespace laya
