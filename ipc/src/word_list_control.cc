// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/word_list_control.h>

#include <algorithm>
#include <utility>

#include <windows.h>

#include <json.hpp>

#include <cxxime/control_client.h>
#include <cxxime/control_protocol.h>

namespace cxxime {
namespace {

using json = nlohmann::json;

const char* operation_name(WordListOperation operation) {
    switch (operation) {
    case WordListOperation::kList:
        return "list";
    case WordListOperation::kPreview:
        return "preview";
    case WordListOperation::kImport:
        return "import";
    case WordListOperation::kSetEnabled:
        return "set_enabled";
    case WordListOperation::kRemove:
        return "remove";
    case WordListOperation::kGuessed:
        return "guessed";
    case WordListOperation::kPreviewWords:
        return "preview_words";
    case WordListOperation::kAddWords:
        return "add_words";
    case WordListOperation::kQueryWords:
        return "query_words";
    case WordListOperation::kSetReading:
        return "set_reading";
    case WordListOperation::kRemoveWords:
        return "remove_words";
    default:
        return "unknown";
    }
}

WordListOperation parse_operation(const std::string& name) {
    for (WordListOperation operation :
         {WordListOperation::kList, WordListOperation::kPreview, WordListOperation::kImport,
          WordListOperation::kSetEnabled, WordListOperation::kRemove, WordListOperation::kGuessed,
          WordListOperation::kPreviewWords, WordListOperation::kAddWords, WordListOperation::kQueryWords,
          WordListOperation::kSetReading, WordListOperation::kRemoveWords}) {
        if (name == operation_name(operation)) return operation;
    }
    return WordListOperation::kUnknown;
}

bool needs_path(WordListOperation operation) {
    return operation == WordListOperation::kPreview || operation == WordListOperation::kImport;
}

bool needs_id(WordListOperation operation) {
    return operation == WordListOperation::kSetEnabled || operation == WordListOperation::kRemove ||
           operation == WordListOperation::kGuessed;
}

bool needs_text(WordListOperation operation) {
    return operation == WordListOperation::kPreviewWords || operation == WordListOperation::kAddWords;
}

// The box's text, a word or a reading: bounded so that a request fits the payload.
constexpr std::size_t kMaxBoxTextBytes = 32 * 1024;  // word_list.h kMaxMyWordTextBytes
constexpr std::size_t kMaxWordBytes = 256;

bool valid_word(const MyWordInfo& word) {
    return !word.text.empty() && word.text.size() <= kMaxWordBytes && word.syllables.size() <= kMaxWordBytes &&
           word.wubi.size() <= 16 && (word.language == "zh" || word.language == "en");
}

bool valid_request(const WordListControlRequest& request) {
    if (request.operation == WordListOperation::kUnknown || request.path.size() > 32768 ||
        request.id.size() > 64 || request.text.size() > kMaxBoxTextBytes ||
        request.query.size() > kMaxWordBytes || request.pinyin.size() > kMaxWordBytes ||
        request.words.size() > kMaxMyWordsPage) {
        return false;
    }
    if (needs_path(request.operation) && request.path.empty()) return false;
    if (needs_id(request.operation) && request.id.empty()) return false;
    if (needs_text(request.operation) && request.text.empty()) return false;
    if (request.operation == WordListOperation::kQueryWords &&
        (request.limit == 0 || request.limit > kMaxMyWordsPage)) {
        return false;
    }
    if (request.operation == WordListOperation::kSetReading &&
        (!valid_word(request.word) || request.word.language != "zh" || request.pinyin.empty())) {
        return false;
    }
    if (request.operation == WordListOperation::kRemoveWords &&
        (request.words.empty() ||
         !std::all_of(request.words.begin(), request.words.end(), valid_word))) {
        return false;
    }
    return true;
}

json word_json(const MyWordInfo& word) {
    json object = {{"language", word.language}, {"text", word.text}};
    if (!word.syllables.empty()) object["syllables"] = word.syllables;
    if (!word.wubi.empty()) object["wubi"] = word.wubi;
    if (word.guessed) object["guessed"] = true;
    return object;
}

MyWordInfo parse_word(const json& object) {
    MyWordInfo word;
    word.language = object.value("language", std::string{});
    word.text = object.value("text", std::string{});
    word.syllables = object.value("syllables", std::string{});
    word.wubi = object.value("wubi", std::string{});
    word.guessed = object.value("guessed", false);
    return word;
}

json line_json(const MyWordLineInfo& line) {
    json object = {{"line", line.line}, {"text", line.text}, {"language", line.language},
                   {"status", line.status}};
    if (!line.problem.empty()) object["problem"] = line.problem;
    if (!line.syllables.empty()) object["syllables"] = line.syllables;
    if (!line.wubi.empty()) object["wubi"] = line.wubi;
    if (line.guessed) object["guessed"] = true;
    if (line.pinyin_known) object["pinyin_known"] = true;
    if (line.wubi_known) object["wubi_known"] = true;
    if (!line.code.empty()) object["code"] = line.code;
    return object;
}

MyWordLineInfo parse_line(const json& object) {
    MyWordLineInfo line;
    line.line = object.value("line", std::uint32_t{0});
    line.text = object.value("text", std::string{});
    line.language = object.value("language", std::string{});
    line.status = object.value("status", std::string{});
    line.problem = object.value("problem", std::string{});
    line.syllables = object.value("syllables", std::string{});
    line.wubi = object.value("wubi", std::string{});
    line.guessed = object.value("guessed", false);
    line.pinyin_known = object.value("pinyin_known", false);
    line.wubi_known = object.value("wubi_known", false);
    line.code = object.value("code", std::string{});
    return line;
}

// `file_name` only for the file just read: the lists stay within the payload limit.
json info_json(const WordListInfo& info, bool with_file_name) {
    json object = {
        {"id", info.id},          {"name", info.name},
        {"format", info.format},  {"language", info.language}, {"imported_at", info.imported_at},
        {"enabled", info.enabled}, {"read", info.read},      {"added", info.added},
        {"existing", info.existing}, {"invalid", info.invalid}, {"pinyin", info.pinyin},
        {"wubi", info.wubi},      {"guessed", info.guessed},
    };
    if (with_file_name) object["file_name"] = info.file_name;
    return object;
}

WordListInfo parse_info(const json& object) {
    WordListInfo info;
    info.id = object.value("id", std::string{});
    info.name = object.value("name", std::string{});
    info.file_name = object.value("file_name", std::string{});
    info.format = object.value("format", std::string{});
    info.language = object.value("language", std::string{});
    info.imported_at = object.value("imported_at", std::string{});
    info.enabled = object.value("enabled", true);
    info.read = object.value("read", std::uint64_t{0});
    info.added = object.value("added", std::uint64_t{0});
    info.existing = object.value("existing", std::uint64_t{0});
    info.invalid = object.value("invalid", std::uint64_t{0});
    info.pinyin = object.value("pinyin", std::uint64_t{0});
    info.wubi = object.value("wubi", std::uint64_t{0});
    info.guessed = object.value("guessed", std::uint64_t{0});
    return info;
}

}  // namespace

bool encode_word_list_request(const WordListControlRequest& request, std::string* payload) {
    if (!payload) return false;
    payload->clear();
    if (!valid_request(request)) return false;
    json object = {{"operation", operation_name(request.operation)}};
    if (needs_path(request.operation)) object["path"] = request.path;
    if (needs_id(request.operation)) object["id"] = request.id;
    if (request.operation == WordListOperation::kSetEnabled) object["enabled"] = request.enabled;
    if (needs_text(request.operation)) object["text"] = request.text;
    if (request.operation == WordListOperation::kQueryWords) {
        object["query"] = request.query;
        object["offset"] = request.offset;
        object["limit"] = request.limit;
    }
    if (request.operation == WordListOperation::kSetReading) {
        object["word"] = word_json(request.word);
        object["pinyin"] = request.pinyin;
    }
    if (request.operation == WordListOperation::kRemoveWords) {
        json words = json::array();
        for (const MyWordInfo& word : request.words) words.push_back(word_json(word));
        object["words"] = std::move(words);
    }
    try {
        *payload = object.dump();
    } catch (const json::exception&) {  // text that is not UTF-8
        payload->clear();
        return false;
    }
    if (payload->size() > CONTROL_MAX_PAYLOAD) {
        payload->clear();
        return false;
    }
    return true;
}

bool decode_word_list_request(const std::string& payload, WordListControlRequest* request) {
    if (!request || payload.empty() || payload.size() > CONTROL_MAX_PAYLOAD) return false;
    try {
        const json object = json::parse(payload);
        if (!object.is_object() || !object.contains("operation") || !object["operation"].is_string()) {
            return false;
        }
        WordListControlRequest parsed;
        parsed.operation = parse_operation(object["operation"].get<std::string>());
        parsed.path = object.value("path", std::string{});
        parsed.id = object.value("id", std::string{});
        parsed.enabled = object.value("enabled", true);
        parsed.text = object.value("text", std::string{});
        parsed.query = object.value("query", std::string{});
        parsed.offset = object.value("offset", std::size_t{0});
        parsed.limit = object.value("limit", std::size_t{0});
        parsed.pinyin = object.value("pinyin", std::string{});
        if (object.contains("word") && object["word"].is_object()) parsed.word = parse_word(object["word"]);
        if (object.contains("words") && object["words"].is_array()) {
            if (object["words"].size() > kMaxMyWordsPage) return false;
            for (const json& word : object["words"]) {
                if (!word.is_object()) return false;
                parsed.words.push_back(parse_word(word));
            }
        }
        if (!valid_request(parsed)) return false;
        *request = std::move(parsed);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool encode_word_list_result(const WordListControlResult& result, std::string* payload) {
    if (!payload || result.operation == WordListOperation::kUnknown) return false;
    json object = {
        {"operation", operation_name(result.operation)},
        {"succeeded", result.succeeded},
        {"error_code", result.error_code},
    };
    json lists = json::array();
    for (const WordListInfo& info : result.lists) lists.push_back(info_json(info, false));
    object["lists"] = std::move(lists);
    if (result.operation == WordListOperation::kPreview || result.operation == WordListOperation::kImport) {
        object["list"] = info_json(result.list, true);
    }
    if (!result.existing_name.empty()) object["existing_name"] = result.existing_name;
    if (!result.words.empty()) {
        json words = json::array();
        for (const auto& [text, pinyin] : result.words) words.push_back({text, pinyin});
        object["words"] = std::move(words);
    }
    if (!result.lines.empty()) {
        json lines = json::array();
        for (const MyWordLineInfo& line : result.lines) lines.push_back(line_json(line));
        object["lines"] = std::move(lines);
    }
    if (result.operation == WordListOperation::kQueryWords) {
        json words = json::array();
        for (const MyWordInfo& word : result.my_words) words.push_back(word_json(word));
        object["my_words"] = std::move(words);
        object["total"] = result.total;
    }
    if (!result.problem.empty()) object["problem"] = result.problem;
    try {
        *payload = object.dump();
    } catch (const json::exception&) {  // a name that is not UTF-8
        payload->clear();
        return false;
    }
    if (payload->size() > CONTROL_MAX_PAYLOAD) {
        payload->clear();
        return false;
    }
    return true;
}

bool decode_word_list_result(const std::string& payload, WordListControlResult* result) {
    if (!result || payload.empty() || payload.size() > CONTROL_MAX_PAYLOAD) return false;
    try {
        const json object = json::parse(payload);
        if (!object.is_object() || !object.contains("operation") || !object["operation"].is_string() ||
            !object.contains("succeeded") || !object["succeeded"].is_boolean() ||
            !object.contains("error_code") || !object["error_code"].is_number_unsigned()) {
            return false;
        }
        WordListControlResult parsed;
        parsed.operation = parse_operation(object["operation"].get<std::string>());
        if (parsed.operation == WordListOperation::kUnknown) return false;
        parsed.succeeded = object["succeeded"].get<bool>();
        parsed.error_code = object["error_code"].get<std::uint32_t>();
        if (object.contains("lists") && object["lists"].is_array()) {
            for (const json& info : object["lists"]) {
                if (info.is_object()) parsed.lists.push_back(parse_info(info));
            }
        }
        if (object.contains("list") && object["list"].is_object()) parsed.list = parse_info(object["list"]);
        parsed.existing_name = object.value("existing_name", std::string{});
        if (object.contains("words") && object["words"].is_array()) {
            for (const json& word : object["words"]) {
                if (word.is_array() && word.size() == 2 && word[0].is_string() && word[1].is_string()) {
                    parsed.words.emplace_back(word[0].get<std::string>(), word[1].get<std::string>());
                }
            }
        }
        if (object.contains("lines") && object["lines"].is_array()) {
            for (const json& line : object["lines"]) {
                if (line.is_object()) parsed.lines.push_back(parse_line(line));
            }
        }
        if (object.contains("my_words") && object["my_words"].is_array()) {
            for (const json& word : object["my_words"]) {
                if (word.is_object()) parsed.my_words.push_back(parse_word(word));
            }
        }
        parsed.total = object.value("total", std::uint64_t{0});
        parsed.problem = object.value("problem", std::string{});
        *result = std::move(parsed);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

WordListControlClient::WordListControlClient(int timeout_ms, const std::wstring& pipe_name)
    : timeout_ms_(timeout_ms), pipe_name_(pipe_name) {}

bool WordListControlClient::execute(const WordListControlRequest& request,
                                    WordListControlResult* result) const {
    if (!result) return false;
    std::string payload;
    if (!encode_word_list_request(request, &payload)) {
        result->operation = request.operation;
        result->error_code = ERROR_INVALID_DATA;
        return false;
    }
    // Reading and resolving a large list takes a few seconds: wait for it.
    constexpr int kWaitForResult = -1;
    ControlMessage response;
    unsigned long transport_error = ERROR_SUCCESS;
    if (!send_control_request(ControlMessageType::kWordListRequest, payload,
                              ControlMessageType::kWordListResult, &response, &transport_error,
                              timeout_ms_, pipe_name_, kWaitForResult) ||
        response.generation != ConfigGeneration{} ||
        !decode_word_list_result(response.payload, result) ||
        result->operation != request.operation) {
        result->operation = request.operation;
        result->succeeded = false;
        result->error_code = transport_error == ERROR_SUCCESS ? ERROR_INVALID_DATA : transport_error;
        return false;
    }
    return result->succeeded;
}

bool WordListControlClient::list(WordListControlResult* result) const {
    return execute({WordListOperation::kList, {}, {}, true}, result);
}

bool WordListControlClient::preview(const std::string& path, WordListControlResult* result) const {
    return execute({WordListOperation::kPreview, path, {}, true}, result);
}

bool WordListControlClient::import_list(const std::string& path, WordListControlResult* result) const {
    return execute({WordListOperation::kImport, path, {}, true}, result);
}

bool WordListControlClient::set_enabled(const std::string& id, bool enabled,
                                        WordListControlResult* result) const {
    return execute({WordListOperation::kSetEnabled, {}, id, enabled}, result);
}

bool WordListControlClient::remove(const std::string& id, WordListControlResult* result) const {
    return execute({WordListOperation::kRemove, {}, id, true}, result);
}

bool WordListControlClient::guessed(const std::string& id, WordListControlResult* result) const {
    return execute({WordListOperation::kGuessed, {}, id, true}, result);
}

bool WordListControlClient::preview_words(const std::string& text, WordListControlResult* result) const {
    WordListControlRequest request;
    request.operation = WordListOperation::kPreviewWords;
    request.text = text;
    return execute(request, result);
}

bool WordListControlClient::add_words(const std::string& text, WordListControlResult* result) const {
    WordListControlRequest request;
    request.operation = WordListOperation::kAddWords;
    request.text = text;
    return execute(request, result);
}

bool WordListControlClient::query_words(const std::string& query, std::size_t offset, std::size_t limit,
                                        WordListControlResult* result) const {
    WordListControlRequest request;
    request.operation = WordListOperation::kQueryWords;
    request.query = query;
    request.offset = offset;
    request.limit = limit;
    return execute(request, result);
}

bool WordListControlClient::set_reading(const MyWordInfo& word, const std::string& pinyin,
                                        WordListControlResult* result) const {
    WordListControlRequest request;
    request.operation = WordListOperation::kSetReading;
    request.word = word;
    request.pinyin = pinyin;
    return execute(request, result);
}

bool WordListControlClient::remove_words(const std::vector<MyWordInfo>& words,
                                         WordListControlResult* result) const {
    WordListControlRequest request;
    request.operation = WordListOperation::kRemoveWords;
    request.words = words;
    return execute(request, result);
}

}  // namespace cxxime
