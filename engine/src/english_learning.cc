// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/english_learning.h>

#include <algorithm>
#include <cctype>
#include <chrono>

#include <json.hpp>

#include "user_data_file.h"

namespace cxxime {

namespace {

constexpr std::uint64_t kMaxFileSize = 4 * 1024 * 1024;
constexpr auto kSaveDelay = std::chrono::seconds(3);  // batches the commits of a sentence

std::mutex g_shared_mutex;
std::shared_ptr<EnglishLearning> g_shared;

// Lowercase key of a learnable word: letters only, kMinWordLength..kMaxWordLength long.
bool learnable_key(const std::string& text, std::string* key) {
    if (text.size() < EnglishLearning::kMinWordLength || text.size() > EnglishLearning::kMaxWordLength)
        return false;
    key->clear();
    for (char c : text) {
        if (!std::isalpha(static_cast<unsigned char>(c))) return false;
        *key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

std::string lowercase(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

EnglishLearning::~EnglishLearning() { stop_writer(); }

std::shared_ptr<EnglishLearning> EnglishLearning::shared() {
    std::lock_guard<std::mutex> lock(g_shared_mutex);
    return g_shared;
}

bool EnglishLearning::open_shared(const std::string& path) {
    std::shared_ptr<EnglishLearning> previous;
    {
        std::lock_guard<std::mutex> lock(g_shared_mutex);
        if (g_shared && g_shared->path_ == path) return true;
        previous = std::move(g_shared);
    }
    if (previous) {
        previous->stop_writer();  // saves what it still holds
    }
    auto learning = std::make_shared<EnglishLearning>();
    const bool loaded = learning->load(path);
    learning->start_writer();
    std::lock_guard<std::mutex> lock(g_shared_mutex);
    g_shared = std::move(learning);
    return loaded;
}

bool EnglishLearning::close_shared() {
    std::shared_ptr<EnglishLearning> learning;
    {
        std::lock_guard<std::mutex> lock(g_shared_mutex);
        learning = g_shared;
    }
    if (!learning) return true;
    learning->stop_writer();
    return learning->flush();
}

bool EnglishLearning::load(const std::string& path) {
    std::string contents;
    const bool read = read_user_data_file(path, kMaxFileSize, &contents);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        corrections_.clear();
        user_words_.clear();
        clock_ = 0;
        dirty_ = false;
    }
    // A damaged file starts an empty store; it is replaced on the next save.
    return read && (contents.empty() || parse(contents));
}

bool EnglishLearning::parse(const std::string& contents) {
    nlohmann::json j = nlohmann::json::parse(contents, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    corrections_.clear();
    user_words_.clear();
    clock_ = j.value("clock", std::uint64_t{0});
    std::string key;
    if (j.contains("corrections") && j["corrections"].is_array()) {
        for (const auto& item : j["corrections"]) {
            if (!item.is_object()) continue;
            const std::string typed = item.value("typed", std::string());
            const std::string word = item.value("word", std::string());
            const int count = (std::min)(item.value("count", 0), kMaxCount);
            if (word.empty() || count <= 0 || !learnable_key(typed, &key)) continue;
            corrections_[key].push_back({word, count, item.value("last", std::uint64_t{0})});
        }
    }
    if (j.contains("user_words") && j["user_words"].is_array()) {
        for (const auto& item : j["user_words"]) {
            if (!item.is_object()) continue;
            const std::string text = item.value("text", std::string());
            const int count = (std::min)(item.value("count", 0), kMaxCount);
            if (count <= 0 || !learnable_key(text, &key)) continue;
            user_words_[key] = {text, count, item.value("last", std::uint64_t{0})};
        }
    }
    evict_locked();
    return true;
}

std::string EnglishLearning::serialize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serialize_locked();
}

std::string EnglishLearning::serialize_locked() const {
    nlohmann::json j;
    j["version"] = 1;
    j["clock"] = clock_;
    j["corrections"] = nlohmann::json::array();
    for (const auto& [typed, entries] : corrections_) {
        for (const auto& e : entries)
            j["corrections"].push_back(
                {{"typed", typed}, {"word", e.word}, {"count", e.count}, {"last", e.last}});
    }
    j["user_words"] = nlohmann::json::array();
    for (const auto& [key, e] : user_words_)
        j["user_words"].push_back({{"text", e.text}, {"count", e.count}, {"last", e.last}});
    return j.dump(1);
}

void EnglishLearning::record_kept(const std::string& typed, bool in_dictionary) {
    std::string key;
    if (!learnable_key(typed, &key)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++clock_;
    bool changed = false;
    auto found = corrections_.find(key);
    if (found != corrections_.end()) {
        for (auto& e : found->second) --e.count;
        auto& entries = found->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const CorrectionEntry& e) { return e.count <= 0; }),
                      entries.end());
        if (entries.empty()) corrections_.erase(found);
        changed = true;
    }
    if (!in_dictionary) {
        UserWordEntry& e = user_words_[key];
        e.text = typed;
        e.count = (std::min)(e.count + 1, kMaxCount);
        e.last = clock_;
        changed = true;
    }
    if (changed) changed_locked();
}

void EnglishLearning::record_correction(const std::string& typed, const std::string& word) {
    std::string key;
    if (word.empty() || !learnable_key(typed, &key)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++clock_;
    auto& entries = corrections_[key];
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const CorrectionEntry& e) { return e.word == word; });
    if (it == entries.end()) {
        entries.push_back({word, 1, clock_});
    } else {
        it->count = (std::min)(it->count + 1, kMaxCount);
        it->last = clock_;
    }
    auto user_word = user_words_.find(key);
    if (user_word != user_words_.end() && --user_word->second.count <= 0) {
        user_words_.erase(user_word);
    }
    changed_locked();
}

std::vector<EnglishLearning::Correction> EnglishLearning::corrections_for(
    const std::string& typed) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = corrections_.find(lowercase(typed));
    if (found == corrections_.end()) return {};
    std::vector<CorrectionEntry> entries = found->second;
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.count != b.count ? a.count > b.count : a.last > b.last;
    });
    std::vector<Correction> out;
    for (const auto& e : entries) out.push_back({e.word, e.count});
    return out;
}

bool EnglishLearning::is_user_word(const std::string& typed) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = user_words_.find(lowercase(typed));
    return found != user_words_.end() && found->second.count >= kActiveCount;
}

bool EnglishLearning::is_user_word_prefix(const std::string& typed) const {
    const std::string key = lowercase(typed);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = user_words_.lower_bound(key);
         it != user_words_.end() && it->first.compare(0, key.size(), key) == 0; ++it) {
        if (it->second.count >= kActiveCount) return true;
    }
    return false;
}

std::vector<EnglishLearning::UserWord> EnglishLearning::user_words_with_prefix(
    const std::string& typed, int limit) const {
    const std::string key = lowercase(typed);
    std::vector<UserWordEntry> found;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = user_words_.lower_bound(key);
             it != user_words_.end() && it->first.compare(0, key.size(), key) == 0; ++it) {
            if (it->second.count >= kActiveCount) found.push_back(it->second);
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.count != b.count ? a.count > b.count : a.last > b.last;
    });
    std::vector<UserWord> out;
    for (const auto& e : found) {
        if (static_cast<int>(out.size()) >= limit) break;
        out.push_back({e.text, e.count});
    }
    return out;
}

void EnglishLearning::changed_locked() {
    evict_locked();
    dirty_ = true;
    wake_.notify_all();
}

void EnglishLearning::evict_locked() {
    // Least used, then least recent, go first.
    std::size_t corrections = 0;
    for (const auto& [typed, entries] : corrections_) corrections += entries.size();
    if (corrections > kMaxCorrections) {
        std::vector<std::pair<std::pair<int, std::uint64_t>, std::pair<std::string, std::string>>> all;
        for (const auto& [typed, entries] : corrections_)
            for (const auto& e : entries) all.push_back({{e.count, e.last}, {typed, e.word}});
        std::sort(all.begin(), all.end());
        for (std::size_t i = 0; i < corrections - kMaxCorrections; ++i) {
            auto& entries = corrections_[all[i].second.first];
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&](const CorrectionEntry& e) {
                                             return e.word == all[i].second.second;
                                         }),
                          entries.end());
            if (entries.empty()) corrections_.erase(all[i].second.first);
        }
    }
    if (user_words_.size() > kMaxUserWords) {
        std::vector<std::pair<std::pair<int, std::uint64_t>, std::string>> all;
        for (const auto& [key, e] : user_words_) all.push_back({{e.count, e.last}, key});
        std::sort(all.begin(), all.end());
        const std::size_t excess = user_words_.size() - kMaxUserWords;
        for (std::size_t i = 0; i < excess; ++i) user_words_.erase(all[i].second);
    }
}

bool EnglishLearning::write_contents(const std::string& contents) const {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = path_;
    }
    return !path.empty() && write_user_data_file_atomically(path, contents);
}

bool EnglishLearning::flush() {
    std::string contents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dirty_) return true;
        contents = serialize_locked();
        dirty_ = false;
    }
    if (write_contents(contents)) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
    return false;
}

bool EnglishLearning::clear_and_save() {
    std::string contents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        corrections_.clear();
        user_words_.clear();
        dirty_ = false;
        contents = serialize_locked();
    }
    return write_contents(contents);
}

void EnglishLearning::start_writer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (writer_.joinable()) return;
    stopping_ = false;
    writer_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            wake_.wait(lock, [this] { return stopping_ || dirty_; });
            if (stopping_) break;
            wake_.wait_for(lock, kSaveDelay, [this] { return stopping_; });
            lock.unlock();
            flush();
            lock.lock();
        }
    });
}

void EnglishLearning::stop_writer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writer_.joinable()) return;
        stopping_ = true;
    }
    wake_.notify_all();
    writer_.join();
    flush();
}

}  // namespace cxxime
