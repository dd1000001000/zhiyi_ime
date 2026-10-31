// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "phrase_service.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <cxxime/custom_phrases.h>
#include <cxxime/data_path.h>
#include <cxxime/logging.h>

namespace {

constexpr DWORD kSettleMs = 300;  // a save often comes as several writes
constexpr std::size_t kMaxFileBytes = 64ULL * 1024ULL * 1024ULL;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>((std::max)(0, size)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string directory() { return cxxime::user_data_path("phrases"); }

bool read_file(const std::string& path, std::string* contents) {
    std::ifstream in(widen(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxFileBytes) return false;
    in.seekg(0);
    contents->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// Written whole to a temporary file first, then moved over: the watcher never reads half a file.
bool write_file(const std::string& path, const std::string& contents) {
    CreateDirectoryW(widen(directory()).c_str(), nullptr);
    const std::wstring target = widen(path);
    const std::wstring temporary = target + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "\xEF\xBB\xBF" << contents;
        if (!out) return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace

std::string custom_phrases_path() { return cxxime::user_data_path("phrases\\phrases.ini"); }

void load_custom_phrases() {
    std::string contents;
    std::vector<cxxime::CustomPhrase> phrases;
    if (read_file(custom_phrases_path(), &contents)) phrases = cxxime::parse_custom_phrases(contents);
    const std::size_t count = phrases.size();
    cxxime::CustomPhraseTable::set_shared(
        std::make_shared<const cxxime::CustomPhraseTable>(std::move(phrases)));
    CXXIME_LOG(L"custom_phrases event=load count=%zu", count);
}

void PhraseService::start() {
    load_custom_phrases();
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) return;
    thread_ = std::thread([this]() { watch(); });
}

void PhraseService::stop() {
    if (stop_event_) SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    if (stop_event_) CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

void PhraseService::watch() {
    // The folder is watched (the file may not exist yet, and saves replace it).
    CreateDirectoryW(widen(directory()).c_str(), nullptr);
    HANDLE change = FindFirstChangeNotificationW(
        widen(directory()).c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (change == INVALID_HANDLE_VALUE) {
        CXXIME_LOG(L"%s", L"custom_phrases event=watch result=failed");
        return;
    }
    const HANDLE handles[] = {stop_event_, change};
    while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        // Let the writes settle, then read once.
        if (WaitForSingleObject(stop_event_, kSettleMs) == WAIT_OBJECT_0) break;
        if (!FindNextChangeNotification(change)) break;
        load_custom_phrases();
    }
    FindCloseChangeNotification(change);
}

std::string snapshot_custom_phrases() {
    std::string contents;
    if (!read_file(custom_phrases_path(), &contents)) return {};
    const auto phrases = cxxime::parse_custom_phrases(contents);
    return phrases.empty() ? std::string() : cxxime::serialize_custom_phrases(phrases);
}

bool validate_custom_phrases_backup(const std::string& contents) {
    return contents.size() <= kMaxFileBytes && !cxxime::parse_custom_phrases(contents).empty();
}

bool merge_custom_phrases_backup(const std::string& contents, std::size_t* imported) {
    std::string current;
    std::vector<cxxime::CustomPhrase> phrases;
    if (read_file(custom_phrases_path(), &current)) phrases = cxxime::parse_custom_phrases(current);
    std::size_t added = 0;
    for (cxxime::CustomPhrase& phrase : cxxime::parse_custom_phrases(contents)) {
        const bool known = std::any_of(phrases.begin(), phrases.end(), [&](const cxxime::CustomPhrase& p) {
            return p.code == phrase.code && p.text == phrase.text;
        });
        if (known || phrases.size() >= cxxime::kMaxCustomPhrases) continue;
        phrases.push_back(std::move(phrase));
        ++added;
    }
    if (imported) *imported = added;
    if (added == 0) return true;
    if (!write_file(custom_phrases_path(), cxxime::serialize_custom_phrases(phrases))) return false;
    load_custom_phrases();
    return true;
}
