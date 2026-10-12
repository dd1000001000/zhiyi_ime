// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "word_list_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/english_lexicon.h>
#include <cxxime/logging.h>
#include <cxxime/word_list.h>
#include <cxxime/word_list_control.h>

#include "session_manager.h"

namespace {

using json = nlohmann::json;

constexpr char kIndexName[] = "index.json";
// kGuessed: the words shown (the payload is at most 64 KiB).
constexpr std::size_t kMaxGuessedWords = 500;

std::mutex& store_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string directory() { return cxxime::user_data_path("wordlists"); }

std::string list_path(const std::string& id) { return directory() + "\\" + id + ".tsv"; }

bool ensure_directory() {
    const std::wstring dir = widen(directory());
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

json info_json(const cxxime::WordListInfo& info) {
    return {{"id", info.id},           {"name", info.name},         {"file_name", info.file_name},
            {"format", info.format},   {"language", info.language}, {"imported_at", info.imported_at},
            {"enabled", info.enabled}, {"read", info.read},         {"added", info.added},
            {"existing", info.existing}, {"invalid", info.invalid}, {"pinyin", info.pinyin},
            {"wubi", info.wubi},       {"guessed", info.guessed},   {"source_hash", info.source_hash}};
}

// An id names a file in the folder: letters, digits and '-' only.
bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 48 &&
           std::all_of(id.begin(), id.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

cxxime::WordListInfo parse_info(const json& item) {
    cxxime::WordListInfo info;
    info.id = item.at("id").get<std::string>();
    info.name = item.value("name", std::string{});
    info.file_name = item.value("file_name", std::string{});
    info.format = item.value("format", std::string{});
    info.language = item.value("language", std::string{"zh"});
    info.imported_at = item.value("imported_at", std::string{});
    info.enabled = item.value("enabled", true);
    info.read = item.value("read", std::uint64_t{0});
    info.added = item.value("added", std::uint64_t{0});
    info.existing = item.value("existing", std::uint64_t{0});
    info.invalid = item.value("invalid", std::uint64_t{0});
    info.pinyin = item.value("pinyin", std::uint64_t{0});
    info.wubi = item.value("wubi", std::uint64_t{0});
    info.guessed = item.value("guessed", std::uint64_t{0});
    info.source_hash = item.value("source_hash", std::string{});
    return info;
}

bool save_index(const std::vector<cxxime::WordListInfo>& lists);

std::string file_time_text(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    SYSTEMTIME utc = {}, local = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToSystemTime(&data.ftLastWriteTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return {};
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth,
                  local.wDay, local.wHour, local.wMinute);
    return text;
}

// The lists of the files in the folder, enabled (index.json lost or damaged: what it said of
// them is gone, their words are not).
std::vector<cxxime::WordListInfo> lists_from_files() {
    std::vector<cxxime::WordListInfo> lists;
    WIN32_FIND_DATAW found = {};
    const HANDLE find = FindFirstFileW(widen(directory() + "\\*.tsv").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return lists;
    do {
        std::wstring name = found.cFileName;
        if (name.size() <= 4) continue;
        name.resize(name.size() - 4);
        const int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()),
                                          nullptr, 0, nullptr, nullptr);
        std::string id(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) {
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), id.data(),
                                n, nullptr, nullptr);
        }
        cxxime::ResolvedWordList list;
        if (!valid_id(id) || !cxxime::load_word_list(list_path(id), &list)) continue;
        cxxime::WordListInfo info;
        info.id = id;
        info.name = list.name.empty() ? id : list.name;
        info.language = list.language == cxxime::WordListLanguage::kEnglish ? "en" : "zh";
        info.imported_at = file_time_text(widen(list_path(id)));
        info.read = info.added = list.entries.size();
        info.pinyin = list.stats.pinyin;
        info.wubi = list.stats.wubi;
        info.guessed = list.stats.guessed;
        lists.push_back(std::move(info));
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(lists.begin(), lists.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    return lists;
}

// The kept lists. When index.json is unreadable, or missing beside list files, it is made again
// from those files (the unreadable one kept as index.json.damaged): false only when that fails,
// and then nothing may be changed (a change would write an index without the lists).
bool load_index(std::vector<cxxime::WordListInfo>* lists) {
    lists->clear();
    const std::wstring path = widen(directory() + "\\" + kIndexName);
    bool present = false;
    {
        std::ifstream file(path, std::ios::binary);
        present = static_cast<bool>(file);
        if (present) {
            try {
                const json root = json::parse(file);
                for (const json& item : root.at("lists")) {
                    cxxime::WordListInfo info = parse_info(item);
                    if (!valid_id(info.id)) throw std::invalid_argument("list id");
                    lists->push_back(std::move(info));
                }
                return true;
            } catch (const std::exception& e) {
                CXXIME_LOG(L"word lists: index unreadable (%S), made again from the lists", e.what());
                lists->clear();
            }
        }
    }
    *lists = lists_from_files();
    if (!present && lists->empty()) return true;  // nothing kept yet
    if (present) {
        MoveFileExW(path.c_str(), (path + L".damaged").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (!save_index(*lists)) {
        CXXIME_LOG(L"%s", L"word lists: index could not be written again");
        lists->clear();
        return false;
    }
    return true;
}

bool save_index(const std::vector<cxxime::WordListInfo>& lists) {
    if (!ensure_directory()) return false;
    json array = json::array();
    for (const auto& info : lists) array.push_back(info_json(info));
    std::string text;
    try {
        text = json{{"format", 1}, {"lists", std::move(array)}}.dump(1);
    } catch (const json::exception&) {
        return false;
    }
    const std::wstring target = widen(directory() + "\\" + kIndexName);
    const std::wstring temporary = target + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

const char* format_name(cxxime::WordListFormat format) {
    switch (format) {
    case cxxime::WordListFormat::kSogouCell:
        return "scel";
    case cxxime::WordListFormat::kRime:
        return "rime";
    case cxxime::WordListFormat::kPinyinText:
        return "pinyin_text";
    case cxxime::WordListFormat::kEnglishText:
        return "english_text";
    default:
        return "";
    }
}

std::uint32_t read_error_code(cxxime::WordListReadError error) {
    switch (error) {
    case cxxime::WordListReadError::kOpen:
        return ERROR_OPEN_FAILED;
    case cxxime::WordListReadError::kTooLarge:
        return cxxime::kWordListErrorTooLarge;
    case cxxime::WordListReadError::kNoWords:
        return cxxime::kWordListErrorNoWords;
    case cxxime::WordListReadError::kNoPinyin:
        return cxxime::kWordListErrorNoPinyin;
    case cxxime::WordListReadError::kExtension:
        return cxxime::kWordListErrorExtension;
    case cxxime::WordListReadError::kFormat:
    default:
        return cxxime::kWordListErrorFormat;
    }
}

std::string file_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "lol.scel" -> "lol"; "en.dict.yaml" -> "en".
std::string stem_of(const std::string& file_name) {
    std::string stem = file_name;
    for (const char* suffix : {".dict.yaml", ".yaml", ".yml", ".scel", ".txt", ".csv", ".tsv"}) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        if (stem.size() > n && _stricmp(stem.c_str() + stem.size() - n, suffix) == 0) {
            stem.resize(stem.size() - n);
            break;
        }
    }
    return stem;
}

std::string local_time_text() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute);
    return text;
}

std::string new_id(const std::vector<cxxime::WordListInfo>& lists) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    for (unsigned n = 0;; ++n) {
        char id[48];
        std::snprintf(id, sizeof(id), "wl%04u%02u%02u%02u%02u%02u-%u", now.wYear, now.wMonth, now.wDay,
                      now.wHour, now.wMinute, now.wSecond, n);
        if (std::none_of(lists.begin(), lists.end(), [&](const auto& l) { return l.id == id; })) return id;
    }
}

void fill_counts(const cxxime::ResolvedWordList& list, cxxime::WordListInfo* info) {
    info->language = list.language == cxxime::WordListLanguage::kEnglish ? "en" : "zh";
    info->read = list.stats.read;
    info->added = list.stats.added;
    info->existing = list.stats.existing;
    info->invalid = list.stats.invalid;
    info->pinyin = list.stats.pinyin;
    info->wubi = list.stats.wubi;
    info->guessed = list.stats.guessed;
}

std::size_t count_language(const std::vector<cxxime::WordListInfo>& lists, const std::string& language) {
    return static_cast<std::size_t>(std::count_if(
        lists.begin(), lists.end(), [&](const auto& l) { return l.language == language; }));
}

// FNV-1a of the file's bytes, as hex: the same file is recognized when imported again.
std::string file_checksum(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[65536];
    std::uint64_t size = 0;
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        const std::streamsize n = file.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
        size += static_cast<std::uint64_t>(n);
        if (size > cxxime::kMaxWordListBytes) return {};  // refused as too large when read
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%016llx-%llu", static_cast<unsigned long long>(hash),
                  static_cast<unsigned long long>(size));
    return text;
}

// At most `limit` characters (names show in one row and travel in a 64 KiB reply).
std::string first_characters(const std::string& text, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80 && count++ == limit) {
            return text.substr(0, i);
        }
    }
    return text;
}

constexpr std::size_t kMaxNameCharacters = 48;
constexpr std::size_t kMaxFileNameCharacters = 64;

// Caller holds store_mutex().
void apply_locked(cxxime::Dict* pinyin, cxxime::Dict* wubi) {
    cxxime::ExtensionWords words;
    std::vector<std::string> disabled_texts;
    std::unordered_set<std::string> enabled_texts;
    std::vector<cxxime::WordListInfo> lists;
    if (!load_index(&lists)) {
        CXXIME_LOG(L"%s", L"word lists: index damaged, no list applied");
    }
    for (const cxxime::WordListInfo& info : lists) {
        cxxime::ResolvedWordList list;
        if (!cxxime::load_word_list(list_path(info.id), &list)) {
            CXXIME_LOG(L"word lists: %S unreadable", info.id.c_str());
            continue;
        }
        if (info.enabled) {
            cxxime::collect_extension_words(list, &words);
            for (const auto& e : list.entries) enabled_texts.insert(e.text);
        } else {
            for (const auto& e : list.entries) disabled_texts.push_back(e.text);
        }
    }
    cxxime::deduplicate_extension_words(&words);
    std::vector<std::string> withheld;
    for (std::string& text : disabled_texts) {
        if (!enabled_texts.count(text)) withheld.push_back(std::move(text));
    }
    std::sort(withheld.begin(), withheld.end());
    withheld.erase(std::unique(withheld.begin(), withheld.end()), withheld.end());
    CXXIME_LOG(L"word lists: applying pinyin=%zu wubi=%zu english=%zu withheld=%zu",
               words.pinyin.size(), words.wubi.size(), words.english.size(), withheld.size());
    if (pinyin) pinyin->set_extension(std::move(words.pinyin), withheld);
    if (wubi) wubi->set_extension(std::move(words.wubi), withheld);
    cxxime::EnglishLexicon::set_extension(std::move(words.english));
}

// What was learned of these words (picks, sentences), in both dictionaries.
void forget_learned(cxxime::Dict* dict, const std::unordered_set<std::string>& texts) {
    if (!dict || texts.empty()) return;
    std::vector<cxxime::LexiconEntryKey> keys;
    for (const auto& entry : dict->query_candidate_preferences({}, 0, SIZE_MAX)) {
        if (texts.count(entry.text)) keys.push_back({entry.text, entry.code});
    }
    if (!keys.empty() && !dict->delete_candidate_preferences_and_save(keys)) {
        CXXIME_LOG(L"%s", L"word lists: learned entries not deleted");
    }
}

}  // namespace

void apply_word_lists(cxxime::Dict* pinyin, cxxime::Dict* wubi) {
    std::lock_guard<std::mutex> lock(store_mutex());
    apply_locked(pinyin, wubi);
}

std::string snapshot_word_lists() {
    std::lock_guard<std::mutex> lock(store_mutex());
    json lists = json::array();
    std::vector<cxxime::WordListInfo> kept;
    load_index(&kept);
    for (const cxxime::WordListInfo& info : kept) {
        std::ifstream file(widen(list_path(info.id)), std::ios::binary);
        if (!file) continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        json item = info_json(info);
        item["contents"] = contents.str();
        lists.push_back(std::move(item));
    }
    if (lists.empty()) return {};
    try {
        return json{{"format", 1}, {"lists", std::move(lists)}}.dump();
    } catch (const json::exception&) {
        return {};
    }
}

namespace {

// The lists of a backup's document, each with its words; false when it is damaged.
bool parse_backup(const std::string& contents,
                  std::vector<std::pair<cxxime::WordListInfo, std::string>>* lists) {
    try {
        const json root = json::parse(contents);
        if (!root.is_object() || root.value("format", 0) != 1 || !root.contains("lists") ||
            !root["lists"].is_array()) {
            return false;
        }
        for (const json& item : root["lists"]) {
            cxxime::WordListInfo info = parse_info(item);
            if (!valid_id(info.id)) return false;
            info.name = first_characters(info.name, kMaxNameCharacters);
            info.file_name = first_characters(info.file_name, kMaxFileNameCharacters);
            std::string words = item.at("contents").get<std::string>();
            cxxime::ResolvedWordList list;
            if (!cxxime::parse_word_list_contents(words, &list)) return false;
            lists->emplace_back(std::move(info), std::move(words));
        }
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

}  // namespace

bool validate_word_lists_backup(const std::string& contents) {
    std::vector<std::pair<cxxime::WordListInfo, std::string>> lists;
    return parse_backup(contents, &lists);
}

void merge_word_lists_backup(SessionManager* sessions, const std::string& contents,
                             std::size_t* imported, std::size_t* skipped) {
    std::vector<std::pair<cxxime::WordListInfo, std::string>> backup;
    if (!parse_backup(contents, &backup)) return;
    std::lock_guard<std::mutex> lock(store_mutex());
    std::vector<cxxime::WordListInfo> lists;
    if (!load_index(&lists)) {
        if (skipped) *skipped += backup.size();
        return;
    }
    bool changed = false;
    for (auto& [info, words] : backup) {
        // Kept already (the same list, or one made from the same file), or no room for it.
        const bool kept = std::any_of(lists.begin(), lists.end(), [&](const auto& l) {
            return l.id == info.id || (!info.source_hash.empty() && l.source_hash == info.source_hash);
        });
        if (kept || count_language(lists, info.language) >= cxxime::kMaxWordListsPerLanguage) {
            if (skipped) ++*skipped;
            continue;
        }
        if (!ensure_directory()) break;
        const std::wstring path = widen(list_path(info.id));
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(words.data(), static_cast<std::streamsize>(words.size()))) continue;
        file.close();
        lists.push_back(info);
        changed = true;
        if (imported) ++*imported;
    }
    if (changed && save_index(lists) && sessions) {
        apply_locked(sessions->dictionary(cxxime::UserDictKind::PINYIN).get(),
                     sessions->dictionary(cxxime::UserDictKind::WUBI).get());
    }
}

bool WordListService::handle_request(const std::string& payload, std::string* response) {
    cxxime::WordListControlRequest request;
    if (!response || !cxxime::decode_word_list_request(payload, &request)) return false;
    cxxime::WordListControlResult result;
    result.operation = request.operation;
    const std::shared_ptr<cxxime::Dict> pinyin = sessions_->dictionary(cxxime::UserDictKind::PINYIN);
    const std::shared_ptr<cxxime::Dict> wubi = sessions_->dictionary(cxxime::UserDictKind::WUBI);

    std::lock_guard<std::mutex> lock(store_mutex());
    std::vector<cxxime::WordListInfo> lists;
    if (!load_index(&lists)) {
        result.error_code = cxxime::kWordListErrorStorage;
        return cxxime::encode_word_list_result(result, response);
    }
    const auto find = [&](const std::string& id) {
        return std::find_if(lists.begin(), lists.end(), [&](const auto& l) { return l.id == id; });
    };

    switch (request.operation) {
    case cxxime::WordListOperation::kList:
        result.succeeded = true;
        break;

    case cxxime::WordListOperation::kPreview:
    case cxxime::WordListOperation::kImport: {
        const std::wstring path = widen(request.path);
        // Another extension: refused before the file is read at all.
        if (!cxxime::word_list_extension_supported(file_name_of(request.path))) {
            result.error_code = cxxime::kWordListErrorExtension;
            break;
        }
        const std::string checksum = file_checksum(path);
        const auto same = std::find_if(lists.begin(), lists.end(), [&](const auto& l) {
            return !checksum.empty() && l.source_hash == checksum;
        });
        if (same != lists.end()) {
            result.error_code = cxxime::kWordListErrorDuplicate;
            result.existing_name = same->name;
            break;
        }
        cxxime::WordListSource source;
        const cxxime::WordListReadError error = cxxime::read_word_list(path, &source);
        if (error != cxxime::WordListReadError::kNone) {
            result.error_code = read_error_code(error);
            break;
        }
        const char* language = source.language == cxxime::WordListLanguage::kEnglish ? "en" : "zh";
        if (count_language(lists, language) >= cxxime::kMaxWordListsPerLanguage) {
            result.list.language = language;
            result.error_code = cxxime::kWordListErrorTooMany;
            break;
        }
        const auto english = cxxime::EnglishLexicon::builtin();
        cxxime::ResolvedWordList list =
            cxxime::resolve_word_list(source, pinyin.get(), wubi.get(), english.get());
        cxxime::WordListInfo& info = result.list;
        info.file_name = first_characters(file_name_of(request.path), kMaxFileNameCharacters);
        info.name = first_characters(!list.name.empty() ? list.name : stem_of(file_name_of(request.path)),
                                     kMaxNameCharacters);
        info.source_hash = checksum;
        info.format = format_name(source.format);
        info.imported_at = local_time_text();
        fill_counts(list, &info);
        if (request.operation == cxxime::WordListOperation::kPreview) {
            result.succeeded = true;
            break;
        }
        if (list.entries.empty()) {
            result.error_code = cxxime::kWordListErrorNothingNew;
            break;
        }
        info.id = new_id(lists);
        list.name = info.name;
        if (!ensure_directory() || !cxxime::write_word_list(list_path(info.id), list)) {
            result.error_code = ERROR_WRITE_FAULT;
            break;
        }
        lists.push_back(info);
        if (!save_index(lists)) {
            DeleteFileW(widen(list_path(info.id)).c_str());
            lists.pop_back();
            result.error_code = ERROR_WRITE_FAULT;
            break;
        }
        apply_locked(pinyin.get(), wubi.get());
        result.succeeded = true;
        break;
    }

    case cxxime::WordListOperation::kSetEnabled: {
        const auto it = find(request.id);
        if (it == lists.end()) {
            result.error_code = ERROR_NOT_FOUND;
            break;
        }
        if (it->enabled != request.enabled) {
            it->enabled = request.enabled;
            if (!save_index(lists)) {
                result.error_code = ERROR_WRITE_FAULT;
                load_index(&lists);
                break;
            }
            apply_locked(pinyin.get(), wubi.get());
        }
        result.succeeded = true;
        break;
    }

    case cxxime::WordListOperation::kRemove: {
        const auto it = find(request.id);
        if (it == lists.end()) {
            result.error_code = ERROR_NOT_FOUND;
            break;
        }
        // The words only this list has: what was learned of them goes with it.
        std::unordered_set<std::string> texts;
        cxxime::ResolvedWordList removed;
        if (cxxime::load_word_list(list_path(it->id), &removed)) {
            for (const auto& e : removed.entries) texts.insert(e.text);
        }
        const std::string id = it->id;
        lists.erase(it);
        for (const auto& other : lists) {
            cxxime::ResolvedWordList list;
            if (!cxxime::load_word_list(list_path(other.id), &list)) continue;
            for (const auto& e : list.entries) texts.erase(e.text);
        }
        if (!save_index(lists)) {
            result.error_code = ERROR_WRITE_FAULT;
            load_index(&lists);
            break;
        }
        DeleteFileW(widen(list_path(id)).c_str());
        apply_locked(pinyin.get(), wubi.get());
        if (removed.language == cxxime::WordListLanguage::kChinese) {
            forget_learned(pinyin.get(), texts);
            forget_learned(wubi.get(), texts);
        }
        result.succeeded = true;
        break;
    }

    case cxxime::WordListOperation::kGuessed: {
        if (find(request.id) == lists.end()) {
            result.error_code = ERROR_NOT_FOUND;
            break;
        }
        cxxime::ResolvedWordList list;
        if (!cxxime::load_word_list(list_path(request.id), &list)) {
            result.error_code = ERROR_OPEN_FAILED;
            break;
        }
        for (const auto& e : list.entries) {
            if (!e.guessed_reading) continue;
            std::string pinyin_text = e.syllables;
            std::replace(pinyin_text.begin(), pinyin_text.end(), ':', ' ');
            result.words.emplace_back(e.text, std::move(pinyin_text));
            if (result.words.size() >= kMaxGuessedWords) break;
        }
        result.succeeded = true;
        break;
    }

    default:
        return false;
    }
    result.lists = std::move(lists);
    if (!cxxime::encode_word_list_result(result, response)) {
        // A list name that cannot be sent (not UTF-8): send the outcome alone.
        result.lists.clear();
        result.list = {};
        result.words.clear();
        return cxxime::encode_word_list_result(result, response);
    }
    return true;
}
