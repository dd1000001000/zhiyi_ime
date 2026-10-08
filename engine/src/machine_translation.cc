// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/machine_translation.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <thread>

#include <json.hpp>

#include <cxxime/data_path.h>
#include <cxxime/gpu_adapters.h>
#include <cxxime/translator_files.h>

#include "laya/text_util.h"

namespace cxxime {
namespace mt {
namespace {

constexpr int kContextSize = 512 * kParallel;  // tokens: 512 per slot
constexpr int kStartTimeoutMs = 60000;
constexpr int kMaxCharacters = 32;

std::wstring wide(const std::string& s) { return laya::utf8_to_wide(s); }
std::string utf8(const std::wstring& s) { return laya::wide_to_utf8(s); }

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Code points of a UTF-8 string.
std::vector<char32_t> code_points(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) n = 1;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

bool is_letter(char32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (c < 0xC0) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;   // punctuation, arrows, shapes
    if (c >= 0x3000 && c <= 0x303F) return false;   // CJK punctuation
    if (c >= 0xFF00 && c <= 0xFF20) return false;   // full-width punctuation and digits
    if (c >= 0xFE30 && c <= 0xFE4F) return false;
    if (c >= 0x1F000) return c >= 0x20000 && c <= 0x3134F;  // emoji no; CJK extensions yes
    return true;
}

const std::map<std::string, const char*>& language_names() {
    static const std::map<std::string, const char*> names = {
        {"en", "英语"}, {"ja", "日语"}, {"ko", "韩语"}, {"fr", "法语"},
        {"de", "德语"}, {"es", "西班牙语"}, {"ru", "俄语"}, {"zh", "中文"},
    };
    return names;
}

std::string random_key() {
    unsigned char bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string key;
    char hex[3] = {};
    for (unsigned char b : bytes) {
        std::snprintf(hex, sizeof(hex), "%02x", b);
        key += hex;
    }
    return key;
}

// A port nobody listens on now (the OS picks it).
int free_port() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    int port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}

// Runs `command` without a window and returns what it printed (stdout and stderr).
std::string run_and_capture(const std::wstring& command, const std::wstring& directory, int timeout_ms) {
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};
    std::wstring line = command;
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, directory.c_str(), &startup, &process);
    CloseHandle(write);
    std::string out;
    if (started) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(read);
    return out;
}

// "Vulkan1" for the card `device_key` (its name, or "<name> #2" for the second of a model),
// matched by name against llama-server --list-devices.
std::string vulkan_device(const InstalledModel& installed, const std::string& device_key,
                          std::string* error) {
    std::string name = device_key;
    int nth = 1;
    const std::regex numbered(R"((.*) #(\d+))");
    std::smatch m;
    if (std::regex_match(device_key, m, numbered)) {
        name = m[1];
        nth = std::stoi(m[2]);
    }
    const std::wstring dir = installed.dir + wide(kTranslatorRuntimeDir);
    const std::string listing =
        run_and_capture(L"\"" + installed.server + L"\" --list-devices", dir, 30000);
    const std::regex device_line(R"(^\s*(Vulkan\d+):\s*(.+?)\s*\(\d+ MiB)");
    int seen = 0;
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        if (end == std::string::npos) end = listing.size();
        const std::string line = listing.substr(start, end - start);
        start = end + 1;
        if (std::regex_search(line, m, device_line) && m[2] == name && ++seen == nth) return m[1];
    }
    if (error) *error = "graphics card not found by Vulkan: " + device_key;
    return {};
}

struct Http {
    HINTERNET session = nullptr;
    int port = 0;
    std::wstring key;

    // POST (or GET when body is empty) on 127.0.0.1; the response body, or nullopt-ish empty
    // with *status 0 on failure.
    std::string request(const wchar_t* path, const std::string* body, int timeout_ms, DWORD* status) {
        *status = 0;
        std::string response;
        HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
        if (!connection) return response;
        HINTERNET request = WinHttpOpenRequest(connection, body ? L"POST" : L"GET", path, nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (request) {
            WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
            const std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + key;
            const BOOL sent = WinHttpSendRequest(
                request, headers.c_str(), static_cast<DWORD>(-1),
                body ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA,
                body ? static_cast<DWORD>(body->size()) : 0, body ? static_cast<DWORD>(body->size()) : 0, 0);
            if (sent && WinHttpReceiveResponse(request, nullptr)) {
                DWORD size = sizeof(*status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX);
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                    response.append(chunk.data(), read);
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
        return response;
    }
};

}  // namespace

bool find_installed(InstalledModel* installed) {
    InstalledModel found;
    found.dir = wide(local_data_dir()) + wide(kTranslatorDir) + L"\\";
    std::ifstream file(found.dir + wide(kTranslatorInstalled));
    const nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (!state.is_object() || !state.contains("model") || !state["model"].is_string() ||
        !state.contains("dir") || !state["dir"].is_string()) {
        return false;
    }
    // The version's own folder (v<N>): a newer one is installed beside it while this one runs.
    const std::wstring version_dir = found.dir + wide(state["dir"].get<std::string>()) + L"\\";
    found.model = version_dir + wide(state["model"].get<std::string>());
    found.server = version_dir + wide(kTranslatorRuntimeDir) + L"\\" + wide(kTranslatorServerExe);
    found.dir = version_dir;
    found.version = state.value("version", 0u);
    if (!file_exists(found.model) || !file_exists(found.server)) return false;
    if (installed) *installed = std::move(found);
    return true;
}

bool translatable(const std::string& text) {
    const std::vector<char32_t> chars = code_points(text);
    if (chars.empty() || chars.size() > static_cast<size_t>(kMaxCharacters)) return false;
    return std::any_of(chars.begin(), chars.end(), is_letter);
}

std::string clean_translation(const std::string& source, const std::string& answer) {
    std::string line;
    size_t start = 0;
    while (start <= answer.size() && line.empty()) {
        size_t end = answer.find('\n', start);
        if (end == std::string::npos) end = answer.size();
        line = answer.substr(start, end - start);
        start = end + 1;
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (!line.empty() && blank(line.front())) line.erase(line.begin());
        while (!line.empty() && blank(line.back())) line.pop_back();
    }
    // Quotes around the whole answer.
    for (const char* pair : {"\"\"", "''"}) {
        if (line.size() >= 2 && line.front() == pair[0] && line.back() == pair[1]) {
            line = line.substr(1, line.size() - 2);
        }
    }
    for (const auto& [open, close] : {std::pair<std::string, std::string>{"\xE2\x80\x9C", "\xE2\x80\x9D"},
                                      {"\xE3\x80\x8C", "\xE3\x80\x8D"}}) {
        if (line.size() >= open.size() + close.size() && line.compare(0, open.size(), open) == 0 &&
            line.compare(line.size() - close.size(), close.size(), close) == 0) {
            line = line.substr(open.size(), line.size() - open.size() - close.size());
        }
    }
    for (char& c : line) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    // Long answers end at a character boundary.
    constexpr size_t kMaxBytes = 120;
    if (line.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        line.resize(cut);
    }
    std::string a = line, b = source;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a == b ? std::string() : line;
}

std::string translation_prompt(const std::string& target, const std::string& text,
                               const std::string& context) {
    const auto found = language_names().find(target);
    const std::string language = found == language_names().end() ? target : found->second;
    if (!context.empty()) {
        return "\xE3\x80\x90\xE8\x83\x8C\xE6\x99\xAF\xE4\xBF\xA1\xE6\x81\xAF\xE3\x80\x91\n" + context +  // 【背景信息】
               "\n\n\xE8\xAF\xB7\xE7\xBB\x93\xE5\x90\x88\xE8\x83\x8C\xE6\x99\xAF\xE4\xBF\xA1\xE6\x81\xAF"  // 请结合背景信息
               "\xE5\xB0\x86\xE4\xBB\xA5\xE4\xB8\x8B\xE6\x96\x87\xE6\x9C\xAC\xE7\xBF\xBB\xE8\xAF\x91\xE4\xB8\xBA" +  // 将以下文本翻译为
               language +
               "\xEF\xBC\x8C\xE6\xB3\xA8\xE6\x84\x8F\xE5\x8F\xAA\xE9\x9C\x80\xE8\xA6\x81\xE8\xBE\x93\xE5\x87\xBA"  // ，注意只需要输出
               "\xE7\xBF\xBB\xE8\xAF\x91\xE5\x90\x8E\xE7\x9A\x84\xE7\xBB\x93\xE6\x9E\x9C\xEF\xBC\x8C"  // 翻译后的结果，
               "\xE4\xB8\x8D\xE8\xA6\x81\xE9\xA2\x9D\xE5\xA4\x96\xE8\xA7\xA3\xE9\x87\x8A\xE3\x80\x82\n\n"  // 不要额外解释。
               "\xE3\x80\x90\xE5\xBE\x85\xE7\xBF\xBB\xE8\xAF\x91\xE6\x96\x87\xE6\x9C\xAC\xE3\x80\x91\n" +  // 【待翻译文本】
               text;
    }
    return "\xE5\xB0\x86\xE4\xBB\xA5\xE4\xB8\x8B\xE6\x96\x87\xE6\x9C\xAC\xE7\xBF\xBB\xE8\xAF\x91\xE4\xB8\xBA" +  // 将以下文本翻译为
           language +
           "\xEF\xBC\x8C\xE6\xB3\xA8\xE6\x84\x8F\xE5\x8F\xAA\xE9\x9C\x80\xE8\xA6\x81\xE8\xBE\x93\xE5\x87\xBA"  // ，注意只需要输出
           "\xE7\xBF\xBB\xE8\xAF\x91\xE5\x90\x8E\xE7\x9A\x84\xE7\xBB\x93\xE6\x9E\x9C\xEF\xBC\x8C"  // 翻译后的结果，
           "\xE4\xB8\x8D\xE8\xA6\x81\xE9\xA2\x9D\xE5\xA4\x96\xE8\xA7\xA3\xE9\x87\x8A\xEF\xBC\x9A\n\n" +  // 不要额外解释：
           text;
}

struct LlamaServer::Impl {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    Http http;
};

LlamaServer::LlamaServer() : impl_(new Impl) {
    impl_->http.session = WinHttpOpen(L"ZhiyiIME", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

LlamaServer::~LlamaServer() {
    stop();
    if (impl_->http.session) WinHttpCloseHandle(impl_->http.session);
}

bool LlamaServer::running() const {
    return impl_->process && WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}

void LlamaServer::stop() {
    if (impl_->job) {
        TerminateJobObject(impl_->job, 0);
        CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
    if (impl_->process) {
        WaitForSingleObject(impl_->process, 5000);
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
    device_.clear();
}

bool LlamaServer::start(const InstalledModel& installed, const std::string& device_key,
                        std::string* error) {
    stop();
    const std::string device = vulkan_device(installed, device_key, error);
    if (device.empty()) return false;
    const int port = free_port();
    if (port == 0) {
        if (error) *error = "no free port";
        return false;
    }
    impl_->http.port = port;
    impl_->http.key = wide(random_key());

    // A job that ends the model server with this process, whatever happens to it.
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const std::wstring dir = installed.dir + wide(kTranslatorRuntimeDir);
    std::wstring command = L"\"" + installed.server + L"\" -m \"" + installed.model + L"\" --device " +
                           wide(device) + L" -ngl 99 -np " + std::to_wstring(kParallel) + L" -c " +
                           std::to_wstring(kContextSize) + L" --host 127.0.0.1 --port " +
                           std::to_wstring(port) + L" --api-key " + impl_->http.key +
                           L" --no-webui --offline --log-disable";
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        dir.c_str(), &startup, &process)) {
        if (error) *error = "cannot start llama-server: " + std::to_string(GetLastError());
        stop();
        return false;
    }
    AssignProcessToJobObject(impl_->job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    impl_->process = process.hProcess;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStartTimeoutMs);
    for (;;) {
        DWORD status = 0;
        impl_->http.request(L"/health", nullptr, 1000, &status);
        if (status == 200) break;
        if (!running() || std::chrono::steady_clock::now() > deadline) {
            if (error) *error = running() ? "llama-server did not get ready" : "llama-server exited";
            stop();
            return false;
        }
        Sleep(100);
    }
    device_ = device_key;
    // The first requests of each batch size build the card's compute pipelines (seconds): do
    // that now, not on the first page.
    std::vector<Item> warm(kParallel, Item{"en", "\xE4\xBD\xA0\xE5\xA5\xBD"});  // 你好
    for (size_t n : {size_t{1}, size_t{3}, size_t{5}, size_t{kParallel}}) {
        warm.resize(n);
        translate(warm, {}, 30000);
    }
    return true;
}

std::vector<std::string> LlamaServer::translate(const std::vector<Item>& items,
                                                const std::string& context, int timeout_ms) {
    std::vector<std::string> results(items.size());
    if (!running()) return results;
    std::vector<std::thread> threads;
    for (size_t i = 0; i < items.size(); ++i) {
        threads.emplace_back([this, &items, &results, &context, timeout_ms, i] {
            const nlohmann::json body = {
                {"messages", {{{"role", "user"},
                               {"content", translation_prompt(items[i].target, items[i].text, context)}}}},
                {"max_tokens", 48},
                {"temperature", 0},
                {"top_k", 1},
                {"repetition_penalty", 1.05},
                {"cache_prompt", true},
            };
            const std::string text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            DWORD status = 0;
            const std::string response = impl_->http.request(L"/v1/chat/completions", &text, timeout_ms, &status);
            if (status != 200) return;
            const nlohmann::json answer = nlohmann::json::parse(response, nullptr, false);
            if (!answer.is_object() || !answer.contains("choices") || !answer["choices"].is_array() ||
                answer["choices"].empty()) {
                return;
            }
            const auto& message = answer["choices"][0]["message"];
            if (message.is_object() && message.contains("content") && message["content"].is_string()) {
                results[i] = clean_translation(items[i].text, message["content"].get<std::string>());
            }
        });
    }
    for (auto& thread : threads) thread.join();
    return results;
}

}  // namespace mt
}  // namespace cxxime
