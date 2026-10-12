// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The user's word lists (word_list.h) kept by the server in user_data_path("wordlists"):
// index.json (each list's name, source, counts, enabled) and one <id>.tsv per list (the resolved
// words, Zhiyi's own format). The enabled lists are applied to the dictionaries at start, after a
// dictionary reload, and after every change made from the settings program (word_list_control.h).
#ifndef CXXIME_SERVER_WORD_LIST_SERVICE_H_
#define CXXIME_SERVER_WORD_LIST_SERVICE_H_

#include <cstddef>
#include <string>

#include <cxxime/dict.h>

class SessionManager;

class WordListService {
public:
    explicit WordListService(SessionManager* sessions) : sessions_(sessions) {}

    // A kWordListRequest payload -> the kWordListResult payload.
    bool handle_request(const std::string& payload, std::string* response);

private:
    SessionManager* sessions_;
};

// Applies the enabled lists to these dictionaries (and the English list); a disabled list's
// words are withheld (what was learned of them stays hidden).
void apply_word_lists(cxxime::Dict* pinyin, cxxime::Dict* wubi);

// Backups (user_backup_service.cc): every kept list in one JSON document, empty when there is
// none; a backup's document checked before anything is imported; and its lists merged in (the
// ones this computer does not have, by id), applied at once.
std::string snapshot_word_lists();
bool validate_word_lists_backup(const std::string& contents);
void merge_word_lists_backup(SessionManager* sessions, const std::string& contents,
                             std::size_t* imported, std::size_t* skipped);

#endif  // CXXIME_SERVER_WORD_LIST_SERVICE_H_
