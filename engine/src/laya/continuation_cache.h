// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
// The model's work on recent compositions (laya_rerank.cc), bounded in bytes: a continuation
// head's scores of n syllables hold n x the vocabulary's characters (6163) floats, about 25 KB
// a syllable; a decoder model's reading (Encoded) about 250 KB. Two generations: when the
// newer one would pass the limit it becomes the older one, so values just computed in the
// background are still there when the page is composed again. Not thread safe (the caller
// locks). The value type V has a size in bytes through cache_bytes(const V&).
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace laya {

inline size_t cache_bytes(const std::vector<std::vector<float>>& scores) {
  size_t bytes = 0;
  for (const auto& slot : scores) bytes += slot.size() * sizeof(float);
  return bytes;
}

template <class V>
class GenerationCache {
 public:
  using Value = std::shared_ptr<const V>;

  explicit GenerationCache(size_t generation_bytes) : limit_(generation_bytes) {}

  // A value found in the older generation moves to the newer one: it is in use.
  Value find(const std::string& key) {
    auto it = newer_.find(key);
    if (it != newer_.end()) return it->second;
    it = older_.find(key);
    if (it == older_.end()) return nullptr;
    Value value = it->second;
    older_bytes_ -= bytes_of(key, value);
    older_.erase(it);
    insert(key, value);
    return value;
  }

  void insert(const std::string& key, Value value) {
    const size_t bytes = bytes_of(key, value);
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
    newer_.emplace(key, std::move(value));
  }

  void clear() {
    newer_.clear();
    older_.clear();
    newer_bytes_ = older_bytes_ = 0;
  }

  size_t bytes() const { return newer_bytes_ + older_bytes_; }

 private:
  static size_t bytes_of(const std::string& key, const Value& value) {
    return key.size() + (value ? cache_bytes(*value) : 0);
  }

  size_t limit_;
  std::unordered_map<std::string, Value> newer_, older_;
  size_t newer_bytes_ = 0;
  size_t older_bytes_ = 0;
};

using ContinuationCache = GenerationCache<std::vector<std::vector<float>>>;

}  // namespace laya
