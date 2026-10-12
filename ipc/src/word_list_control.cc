// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/word_list_control.h>

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
    default:
        return "unknown";
    }
}

WordListOperation parse_operation(const std::string& name) {
    for (WordListOperation operation :
         {WordListOperation::kList, WordListOperation::kPreview, WordListOperation::kImport,
          WordListOperation::kSetEnabled, WordListOperation::kRemove, WordListOperation::kGuessed}) {
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

bool valid_request(const WordListControlRequest& request) {
    if (request.operation == WordListOperation::kUnknown || request.path.size() > 32768 ||
        request.id.size() > 64) {
        return false;
    }
    if (needs_path(request.operation) && request.path.empty()) return false;
    if (needs_id(request.operation) && request.id.empty()) return false;
    return true;
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
    *payload = object.dump();
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

}  // namespace cxxime
