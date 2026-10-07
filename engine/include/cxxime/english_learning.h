// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// What English word mode learns from commits (user data learning_english.json):
//   - corrections: the user picked a spelling correction (teh -> the). Once picked twice the
//     correction takes the recommended slot (second, after the typed text).
//   - user words: the user committed a word that is not in the dictionary as typed (kubectl).
//     After two commits it is no longer corrected and completes like a dictionary word.
// Each commit moves the balance between the two: keeping the typed text takes one count from
// its corrections, picking a correction takes one from the typed text as a user word, so the
// most recent habit wins.
#ifndef CXXIME_ENGLISH_LEARNING_H_
#define CXXIME_ENGLISH_LEARNING_H_

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cxxime {

class EnglishLearning {
public:
    static constexpr int kActiveCount = 2;           // commits before a habit takes effect
    static constexpr int kMaxCount = 20;             // so an old habit can still be unlearned
    static constexpr std::size_t kMaxCorrections = 2000;
    static constexpr std::size_t kMaxUserWords = 5000;
    static constexpr std::size_t kMinWordLength = 3;
    static constexpr std::size_t kMaxWordLength = 32;

    struct Correction {
        std::string word;  // dictionary form
        int count = 0;
    };
    struct UserWord {
        std::string text;  // as committed, e.g. "useState"
        int count = 0;
    };

    EnglishLearning() = default;
    ~EnglishLearning();
    EnglishLearning(const EnglishLearning&) = delete;
    EnglishLearning& operator=(const EnglishLearning&) = delete;

    // The server's instance (nullptr until open_shared): engines learn into it.
    static std::shared_ptr<EnglishLearning> shared();
    // Loads `path` (a missing file is an empty store) and starts saving changes in the
    // background. Opening the path already open keeps the current instance.
    static bool open_shared(const std::string& path);
    // Saves pending changes and stops the background writer (server shutdown).
    static bool close_shared();

    bool load(const std::string& path);
    bool flush();            // writes pending changes now
    bool clear_and_save();   // forgets everything

    // A commit of the typed text itself (`in_dictionary`: the list has it, so it is not a new
    // user word, but its corrections are still weakened).
    void record_kept(const std::string& typed, bool in_dictionary);
    // A commit of the correction `word` for `typed`.
    void record_correction(const std::string& typed, const std::string& word);

    // Corrections picked for `typed` (case-insensitive), most picked first.
    std::vector<Correction> corrections_for(const std::string& typed) const;
    // An active user word equal to / starting with `typed` (case-insensitive).
    bool is_user_word(const std::string& typed) const;
    bool is_user_word_prefix(const std::string& typed) const;
    // Active user words starting with `typed`, most used first.
    std::vector<UserWord> user_words_with_prefix(const std::string& typed, int limit) const;

    std::string serialize() const;
    bool parse(const std::string& contents);

    // Backup import (server/src/user_backup_service.cc): whether `contents` is a learning file
    // (empty is), and adding its habits to these, the higher count winning; then saved.
    static bool validate_contents(const std::string& contents);
    bool merge_contents_and_save(const std::string& contents, std::size_t* imported_count);

private:
    struct CorrectionEntry {
        std::string word;
        int count = 0;
        std::uint64_t last = 0;
    };
    struct UserWordEntry {
        std::string text;
        int count = 0;
        std::uint64_t last = 0;
    };

    void changed_locked();
    void evict_locked();
    std::string serialize_locked() const;
    void start_writer();
    void stop_writer();
    bool write_contents(const std::string& contents) const;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread writer_;
    bool stopping_ = false;
    bool dirty_ = false;
    std::uint64_t clock_ = 0;  // logical time: larger = more recent
    std::string path_;
    std::map<std::string, std::vector<CorrectionEntry>> corrections_;  // typed (lowercase)
    std::map<std::string, UserWordEntry> user_words_;                  // lowercase key
};

}  // namespace cxxime

#endif  // CXXIME_ENGLISH_LEARNING_H_
