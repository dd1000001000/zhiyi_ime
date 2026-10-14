// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The settings program's requests about the user's word lists (word_list.h): list them, look at
// a file before importing it, import it, enable or disable a list, remove one, and see the words
// whose pinyin was worked out. The server keeps the lists (%USERPROFILE%\zhiyu\wordlists) and
// applies them to the dictionaries.
#ifndef CXXIME_WORD_LIST_CONTROL_H_
#define CXXIME_WORD_LIST_CONTROL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cxxime {

enum class WordListOperation {
    kUnknown,
    kList,
    kPreview,     // read and resolve `path`, keep nothing
    kImport,      // the same, then keep it as a new, enabled list
    kSetEnabled,  // `id`, `enabled`
    kRemove,      // `id` (and what was learned of its words)
    kGuessed,     // `id`: the words whose pinyin was worked out
};

struct WordListControlRequest {
    WordListOperation operation = WordListOperation::kUnknown;
    std::string path;  // kPreview / kImport: the file (UTF-8)
    std::string id;
    bool enabled = true;
};

// A kept list, or the file a preview read.
struct WordListInfo {
    std::string id;
    std::string name;       // the name the file gives, else its file name
    std::string file_name;  // the imported file's name
    std::string format;     // scel / rime / pinyin_text / english_text
    std::string language;   // zh / en
    std::string imported_at;  // local time, "2026-10-12 14:03"
    bool enabled = true;
    std::uint64_t read = 0;      // words in the file
    std::uint64_t added = 0;     // words kept
    std::uint64_t existing = 0;  // in the system dictionary already
    std::uint64_t invalid = 0;   // unusable or repeated
    std::uint64_t pinyin = 0;    // typed with pinyin
    std::uint64_t wubi = 0;      // typed with Wubi
    std::uint64_t guessed = 0;   // pinyin worked out from the characters
    // The imported file's checksum (kept by the server, not sent): the same file is not
    // imported twice.
    std::string source_hash;
};

// Lists kept, at most, of each language (Chinese, English).
inline constexpr std::size_t kMaxWordListsPerLanguage = 64;

// Error codes beside the Windows ones: the file's format (WordListReadError).
inline constexpr std::uint32_t kWordListErrorFormat = 0x20000001;     // not a supported format
inline constexpr std::uint32_t kWordListErrorNoWords = 0x20000002;    // no word in it
inline constexpr std::uint32_t kWordListErrorNoPinyin = 0x20000003;   // Chinese words, no pinyin
inline constexpr std::uint32_t kWordListErrorTooLarge = 0x20000004;   // over 64 MB
inline constexpr std::uint32_t kWordListErrorNothingNew = 0x20000005; // every word known
inline constexpr std::uint32_t kWordListErrorTooMany = 0x20000006;    // 64 of its language kept
inline constexpr std::uint32_t kWordListErrorDuplicate = 0x20000007;  // this file is kept already
// wordlists\index.json is damaged and could not be made again from the list files.
inline constexpr std::uint32_t kWordListErrorStorage = 0x20000008;
inline constexpr std::uint32_t kWordListErrorExtension = 0x20000009;  // not a supported extension

struct WordListControlResult {
    WordListOperation operation = WordListOperation::kUnknown;
    bool succeeded = false;
    std::uint32_t error_code = 0;
    std::vector<WordListInfo> lists;  // kList, and after a change: every kept list
    WordListInfo list;                // kPreview / kImport: the file's list
    std::vector<std::pair<std::string, std::string>> words;  // kGuessed: text, pinyin "a li"
    std::string existing_name;        // kWordListErrorDuplicate: the list made from the file
};

bool encode_word_list_request(const WordListControlRequest& request, std::string* payload);
bool decode_word_list_request(const std::string& payload, WordListControlRequest* request);
bool encode_word_list_result(const WordListControlResult& result, std::string* payload);
bool decode_word_list_result(const std::string& payload, WordListControlResult* result);

class WordListControlClient {
public:
    explicit WordListControlClient(int timeout_ms = 1500, const std::wstring& pipe_name = L"");

    bool list(WordListControlResult* result) const;
    bool preview(const std::string& path, WordListControlResult* result) const;
    bool import_list(const std::string& path, WordListControlResult* result) const;
    bool set_enabled(const std::string& id, bool enabled, WordListControlResult* result) const;
    bool remove(const std::string& id, WordListControlResult* result) const;
    bool guessed(const std::string& id, WordListControlResult* result) const;

private:
    bool execute(const WordListControlRequest& request, WordListControlResult* result) const;

    int timeout_ms_;
    std::wstring pipe_name_;
};

}  // namespace cxxime

#endif  // CXXIME_WORD_LIST_CONTROL_H_
