// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The update check, the installer download (WinHTTP, the system proxy) and the start of the
// installer.

#include <cxxime/update.h>

#include <winhttp.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include <cxxime/version.h>

#pragma comment(lib, "winhttp.lib")

namespace cxxime {
namespace update {
namespace {

constexpr size_t kManifestLimit = 256 * 1024;
constexpr size_t kSignatureLimit = 4 * 1024;

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string narrow(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}

// The release base: GitHub, or the development override (kTestBaseVariable).
std::string release_base(bool* test) {
    wchar_t value[512] = {};
    const DWORD length = GetEnvironmentVariableW(kTestBaseVariable, value, 512);
    *test = length > 0 && length < 512;
    if (!*test) return kReleaseBase;
    std::string base = to_utf8(value);
    if (base.back() != '/') base.push_back('/');
    return base;
}

struct Internet {
    HINTERNET handle = nullptr;
    Internet() = default;
    explicit Internet(HINTERNET h) : handle(h) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

// One GET request: https anywhere, http only to this computer (development).
class Get {
public:
    // Sends the request (from byte `offset`) and reads the response headers.
    Status open(const std::string& url, std::uint64_t offset = 0) {
        const std::wstring wide_url = to_wide(url);
        URL_COMPONENTS parts = {sizeof(parts)};
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return Status::kInvalid;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        if (!secure && !(parts.nScheme == INTERNET_SCHEME_HTTP &&
                         (host == L"127.0.0.1" || host == L"localhost"))) {
            return Status::kInvalid;
        }
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const std::wstring agent = L"ZhiyiIME-Update/" CXXIME_VERSION_WSTRING;
        session_.handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.handle) {  // before Windows 8.1
            session_.handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (!session_.handle) return Status::kNetwork;
        WinHttpSetTimeouts(session_.handle, 15000, 15000, 30000, 30000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session_.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                              sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session_.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                             sizeof(protocols));
        }
        connection_.handle = WinHttpConnect(session_.handle, host.c_str(), parts.nPort, 0);
        if (!connection_.handle) return Status::kNetwork;
        request_.handle = WinHttpOpenRequest(connection_.handle, L"GET", path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request_.handle) return Status::kNetwork;
        std::wstring headers;
        if (offset > 0) headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
        if (!WinHttpSendRequest(request_.handle,
                                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request_.handle, nullptr)) {
            return Status::kNetwork;
        }
        DWORD size = sizeof(status_code_);
        if (!WinHttpQueryHeaders(request_.handle,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status_code_, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return Status::kNetwork;
        }
        return Status::kOk;
    }

    DWORD status_code() const { return status_code_; }

    // False on a connection error; *read is 0 at the end.
    bool read(void* buffer, DWORD size, DWORD* read) {
        return WinHttpReadData(request_.handle, buffer, size, read) != FALSE;
    }

    // The whole body, at most `limit` bytes.
    Status read_all(size_t limit, std::string* body) {
        body->clear();
        char buffer[16 * 1024];
        for (;;) {
            DWORD read_size = 0;
            if (!read(buffer, sizeof(buffer), &read_size)) return Status::kNetwork;
            if (read_size == 0) return Status::kOk;
            if (body->size() + read_size > limit) return Status::kInvalid;
            body->append(buffer, read_size);
        }
    }

private:
    Internet session_;
    Internet connection_;
    Internet request_;
    DWORD status_code_ = 0;
};

Status fetch(const std::string& url, size_t limit, std::string* body) {
    Get get;
    const Status status = get.open(url);
    if (status != Status::kOk) return status;
    if (get.status_code() == 404) return Status::kNotFound;
    if (get.status_code() != 200) return Status::kNetwork;
    return get.read_all(limit, body);
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

bool truncate_file(HANDLE file) {
    LARGE_INTEGER start = {};
    return SetFilePointerEx(file, start, nullptr, FILE_BEGIN) && SetEndOfFile(file);
}

}  // namespace

CheckResult check_latest(const std::string& current_version) {
    CheckResult result;
    bool test = false;
    const std::string base = release_base(&test);
    std::string manifest_text;
    std::string signature;
    result.status = fetch(base + "latest.json", kManifestLimit, &manifest_text);
    if (result.status == Status::kOk) {
        result.status = fetch(base + "latest.json.sig", kSignatureLimit, &signature);
    }
    if (result.status != Status::kOk) return result;
    if (!verify_signature(manifest_text, trim(signature), builtin_public_key()) ||
        !parse_manifest(manifest_text, test ? base : std::string(kDownloadPrefix),
                        &result.manifest)) {
        result.status = Status::kInvalid;
        return result;
    }
    result.newer = is_newer_release(result.manifest.version, current_version);
    return result;
}

namespace {

// Downloads `url` into `part_path` (resuming what is there), checks size and SHA-256 and
// renames it to `final_path`.
Status download_checked(const std::string& url, std::uint64_t size, const std::string& sha256,
                        const std::wstring& part_path, const std::wstring& final_path,
                        const Progress& progress, const std::atomic<bool>* cancel) {
    const std::wstring directory = part_path.substr(0, part_path.find_last_of(L'\\'));
    std::string hash;
    HANDLE file = CreateFileW(part_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Status::kDisk;
    struct Closer {
        HANDLE file;
        ~Closer() {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    } closer{file};

    LARGE_INTEGER existing = {};
    GetFileSizeEx(file, &existing);
    std::uint64_t done = static_cast<std::uint64_t>(existing.QuadPart);
    if (done > size) {
        if (!truncate_file(file)) return Status::kDisk;
        done = 0;
    }
    ULARGE_INTEGER free_bytes = {};
    if (GetDiskFreeSpaceExW(directory.c_str(), &free_bytes, nullptr, nullptr) &&
        free_bytes.QuadPart < size - done) {
        return Status::kDisk;
    }

    if (done < size) {
        Get get;
        Status status = get.open(url, done);
        if (status != Status::kOk) return status;
        if (get.status_code() == 200 && done > 0) {  // no range support: from the start
            if (!truncate_file(file)) return Status::kDisk;
            done = 0;
        } else if (get.status_code() == 416) {  // the .part does not match: from the start
            if (!truncate_file(file)) return Status::kDisk;
            done = 0;
            status = get.open(url);
            if (status != Status::kOk) return status;
        }
        if (get.status_code() != 200 && get.status_code() != 206) return Status::kNetwork;
        LARGE_INTEGER end = {};
        end.QuadPart = static_cast<LONGLONG>(done);
        if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN)) return Status::kDisk;

        std::vector<char> buffer(256 * 1024);
        auto last_report = std::chrono::steady_clock::now();
        if (progress) progress(done, size);
        for (;;) {
            if (cancel && cancel->load()) return Status::kCancelled;
            DWORD read = 0;
            if (!get.read(buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                return Status::kNetwork;  // the .part stays: the next try resumes
            }
            if (read == 0) break;
            if (done + read > size) {
                truncate_file(file);
                return Status::kInvalid;
            }
            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
                return Status::kDisk;
            }
            done += read;
            const auto now = std::chrono::steady_clock::now();
            if (progress && now - last_report >= std::chrono::milliseconds(100)) {
                progress(done, size);
                last_report = now;
            }
        }
        if (progress) progress(done, size);
        if (done != size) return Status::kNetwork;
    }

    if (!sha256_file(file, &hash) || hash != sha256) {
        CloseHandle(file);
        closer.file = INVALID_HANDLE_VALUE;
        DeleteFileW(part_path.c_str());
        return Status::kInvalid;
    }
    CloseHandle(file);
    closer.file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(part_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return Status::kDisk;
    }
    return Status::kOk;
}

}  // namespace

Status download_installer(const Manifest& manifest, const std::wstring& directory,
                          const Progress& progress, const std::atomic<bool>* cancel,
                          std::wstring* path) {
    const std::wstring name = to_wide(manifest.file);
    const std::wstring final_path = directory + L"\\" + name;
    clean_downloads(directory, name);

    // Downloaded before (e.g. the installation was cancelled at the UAC prompt).
    std::string hash;
    if (sha256_file(final_path, &hash)) {
        if (hash == manifest.sha256) {
            if (progress) progress(manifest.size, manifest.size);
            *path = final_path;
            return Status::kOk;
        }
        DeleteFileW(final_path.c_str());
    }
    const Status status = download_checked(manifest.url, manifest.size, manifest.sha256,
                                           final_path + L".part", final_path, progress, cancel);
    if (status == Status::kOk) *path = final_path;
    return status;
}

GlossaryCheckResult check_glossaries() {
    GlossaryCheckResult result;
    bool test = false;
    const std::string base = release_base(&test);
    const std::string prefix = test ? base : std::string(kDownloadPrefix);
    std::string manifest_text;
    std::string signature;
    result.status = fetch(prefix + kGlossaryReleasePath + "glossary.json", kManifestLimit,
                          &manifest_text);
    if (result.status == Status::kOk) {
        result.status = fetch(prefix + kGlossaryReleasePath + "glossary.json.sig",
                              kSignatureLimit, &signature);
    }
    if (result.status != Status::kOk) return result;
    if (!verify_signature(manifest_text, trim(signature), builtin_public_key()) ||
        !parse_glossary_manifest(manifest_text, prefix, &result.packs)) {
        result.status = Status::kInvalid;
    }
    return result;
}

Status download_glossary(const GlossaryPack& pack, const std::wstring& path,
                         const Progress& progress, const std::atomic<bool>* cancel) {
    const std::wstring directory = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(directory.c_str(), nullptr);
    return download_checked(pack.url, pack.size, pack.sha256, path + L".part", path, progress,
                            cancel);
}

TranslatorCheckResult check_translator() {
    TranslatorCheckResult result;
    bool test = false;
    const std::string base = release_base(&test);
    const std::string prefix = test ? base : std::string(kDownloadPrefix);
    std::string manifest_text;
    std::string signature;
    result.status = fetch(prefix + kTranslatorReleasePath + "translator.json", kManifestLimit,
                          &manifest_text);
    if (result.status == Status::kOk) {
        result.status = fetch(prefix + kTranslatorReleasePath + "translator.json.sig",
                              kSignatureLimit, &signature);
    }
    if (result.status != Status::kOk) return result;
    if (!verify_signature(manifest_text, trim(signature), builtin_public_key()) ||
        !parse_translator_manifest(manifest_text, prefix, &result.manifest)) {
        result.status = Status::kInvalid;
    }
    return result;
}

Status download_translator_file(const TranslatorFile& file, const std::wstring& path,
                                const Progress& progress, const std::atomic<bool>* cancel) {
    const std::wstring directory = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(directory.c_str(), nullptr);
    return download_checked(file.url, file.size, file.sha256, path + L".part", path, progress,
                            cancel);
}

bool launch_installer(const std::wstring& path, const std::string& sha256, HWND owner,
                      DWORD* error) {
    *error = ERROR_SUCCESS;
    // Held without write or delete sharing until the installer has started, so the file that
    // starts is the file checked here.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    std::string hash;
    if (!sha256_file(file, &hash) || hash != sha256) {
        CloseHandle(file);
        *error = ERROR_INVALID_DATA;
        return false;
    }
    SHELLEXECUTEINFOW info = {sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = L"/UPDATE";
    info.nShow = SW_SHOWNORMAL;
    const bool started = ShellExecuteExW(&info) != FALSE;
    if (!started) *error = GetLastError();
    CloseHandle(file);
    return started;
}

}  // namespace update
}  // namespace cxxime
