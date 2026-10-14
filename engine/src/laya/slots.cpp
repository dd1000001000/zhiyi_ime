// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "slots.h"

#include <fstream>
#include <stdexcept>

#include <json.hpp>

#include "text_util.h"

namespace laya {

namespace {

bool walk(const std::vector<std::string>& syl, const std::string& code, size_t i, size_t pos,
          std::vector<Unit>& out) {
  if (i == syl.size()) return pos == code.size();
  const std::string& s = syl[i];
  const bool last = i + 1 == syl.size();
  // The full syllable first, then the prefixes from the longest: the first reading that spells
  // the rest of the code wins (p2c_common.slot_units).
  if (code.compare(pos, s.size(), s) == 0) {
    out.push_back({'f', s});
    if (walk(syl, code, i + 1, pos + s.size(), out)) return true;
    out.pop_back();
  }
  for (size_t k = s.size() - 1; k >= 1; --k) {
    const bool initial = k == 1 || (k == 2 && s[1] == 'h' && (s[0] == 'z' || s[0] == 'c' || s[0] == 's'));
    if (!last && !initial) continue;
    if (code.compare(pos, k, s, 0, k) != 0) continue;
    out.push_back({'p', s.substr(0, k)});
    if (walk(syl, code, i + 1, pos + k, out)) return true;
    out.pop_back();
  }
  return false;
}

}  // namespace

std::vector<Unit> slot_units(const std::vector<std::string>& syllables, const std::string& code) {
  std::vector<Unit> out;
  if (syllables.empty()) return out;
  for (const auto& s : syllables)
    if (s.empty()) return out;
  if (!walk(syllables, code, 0, 0, out)) out.clear();
  return out;
}

std::vector<Unit> letter_units(const std::string& typed, size_t max_slots) {
  std::vector<Unit> out;
  for (char c : typed) {
    if (out.size() >= max_slots) break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out.push_back({'l', std::string(1, c)});
  }
  return out;
}

std::vector<std::string> split_syllables(std::string_view syllables) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : syllables) {
    if (c == ':' || c == '\'') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

UnifiedVocab::UnifiedVocab(const std::string& json_path) {
  std::ifstream in(utf8_to_wide(json_path));
  if (!in) throw std::runtime_error("cannot read " + json_path);
  const auto j = nlohmann::json::parse(in);
  unit_names_ = j.at("units").get<std::vector<std::string>>();
  for (size_t i = 0; i < unit_names_.size(); ++i) units_[unit_names_[i]] = static_cast<int64_t>(i);
  letters_ = j.value("letters", std::string());
  letter_base_ = static_cast<int64_t>(unit_names_.size());
  chars_ = j.at("chars").get<std::vector<std::string>>();
  readings_.resize(chars_.size());
  const auto& readings = j.at("readings");
  for (size_t i = 0; i < chars_.size(); ++i) {
    char_index_[chars_[i]] = static_cast<int>(i);
    const auto it = readings.find(chars_[i]);
    if (it != readings.end()) readings_[i] = it->get<std::vector<std::string>>();
  }
  max_slots_ = j.value("max_slots", max_slots_);
  max_context_tokens_ = j.value("max_context_tokens", max_context_tokens_);
  context_prefix_ = j.value("context_prefix", std::string(u8"已输入的上文: "));
  decoder_file_ = j.value("decoder", std::string());
  decoder_memory_file_ = j.value("decoder_memory", std::string());
  decoder_step_file_ = j.value("decoder_step", std::string());
}

int64_t UnifiedVocab::unit_id(const Unit& unit) const {
  if (unit.kind == 'l') {
    if (unit.text.size() != 1) return 0;
    const size_t at = letters_.find(unit.text[0]);
    return at == std::string::npos ? 0 : letter_base_ + static_cast<int64_t>(at);
  }
  const auto it = units_.find(std::string(1, unit.kind) + ":" + unit.text);
  return it == units_.end() ? 0 : it->second;
}

int UnifiedVocab::char_index(std::string_view utf8_char) const {
  const auto it = char_index_.find(std::string(utf8_char));
  return it == char_index_.end() ? -1 : it->second;
}

const std::vector<uint8_t>& UnifiedVocab::allowed(int64_t unit_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = allowed_.find(unit_id);
  if (it != allowed_.end()) return it->second;
  std::vector<uint8_t> mask(chars_.size(), 0);
  if (unit_id > 0 && unit_id < static_cast<int64_t>(unit_names_.size())) {
    const std::string& name = unit_names_[static_cast<size_t>(unit_id)];
    const bool full = name.size() > 2 && name[0] == 'f';
    const std::string s = name.size() > 2 ? name.substr(2) : std::string();
    for (size_t i = 0; i < chars_.size(); ++i) {
      for (const auto& r : readings_[i]) {
        if (full ? r == s : r.compare(0, s.size(), s) == 0) {
          mask[i] = 1;
          break;
        }
      }
    }
  }
  return allowed_.emplace(unit_id, std::move(mask)).first->second;
}

}  // namespace laya
