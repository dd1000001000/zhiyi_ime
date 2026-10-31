// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The user's custom phrases (custom_phrases.h, docs/custom-phrases.md) in
// user_data_path("phrases\\phrases.ini"): read at start and again whenever the file changes,
// written by the settings program or by hand, then shared with every session
// (CustomPhraseTable::set_shared).
#ifndef CXXIME_SERVER_PHRASE_SERVICE_H_
#define CXXIME_SERVER_PHRASE_SERVICE_H_

#include <cstddef>
#include <string>
#include <thread>

#include <windows.h>

class PhraseService {
public:
    PhraseService() = default;
    ~PhraseService() { stop(); }
    PhraseService(const PhraseService&) = delete;
    PhraseService& operator=(const PhraseService&) = delete;

    void start();
    void stop();

private:
    void watch();

    std::thread thread_;
    HANDLE stop_event_ = nullptr;
};

// The phrases file's path (UTF-8).
std::string custom_phrases_path();
// Reads the file into the shared table (none: an empty table).
void load_custom_phrases();

// Backups (user_backup_service.cc): the file's text, empty when there is no phrase; a backup's
// text checked before anything is imported; and its phrases merged in (those this computer does
// not have, by letters and text).
std::string snapshot_custom_phrases();
bool validate_custom_phrases_backup(const std::string& contents);
bool merge_custom_phrases_backup(const std::string& contents, std::size_t* imported);

#endif  // CXXIME_SERVER_PHRASE_SERVICE_H_
