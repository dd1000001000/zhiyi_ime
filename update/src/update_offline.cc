// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Offline packages (update.h, docs/offline-packages.md): a zip holding a release's signed manifest
// and the files it lists, or those files downloaded one by one into a folder. Read with Win32 file
// handles (any path, files over 4 GB) through miniz's reader callbacks.

#include <cxxime/update.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "miniz.h"

namespace cxxime {
namespace update {
namespace {

constexpr size_t kManifestLimit = 256 * 1024;
constexpr size_t kSignatureLimit = 4 * 1024;

struct Handle {
    HANDLE file = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : file(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    bool ok() const { return file != INVALID_HANDLE_VALUE; }
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

// A plain file name: no folders, no drive, nothing hidden.
bool plain_name(const std::string& name) {
    return !name.empty() && name.size() <= 128 && name[0] != '.' &&
           name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

// The zip, read through a Win32 handle.
class Zip {
public:
    bool open(const std::wstring& path) {
        file_.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size = {};
        if (!file_.ok() || !GetFileSizeEx(file_.file, &size)) return false;
        zip_.m_pRead = &Zip::read;
        zip_.m_pIO_opaque = this;
        open_ = mz_zip_reader_init(&zip_, static_cast<mz_uint64>(size.QuadPart), 0) != 0;
        return open_;
    }
    ~Zip() {
        if (open_) mz_zip_reader_end(&zip_);
    }

    // The entry's index (-1 when absent) and uncompressed size.
    int find(const std::string& name, std::uint64_t* size) const {
        const int index = mz_zip_reader_locate_file(const_cast<mz_zip_archive*>(&zip_), name.c_str(), nullptr, 0);
        if (index < 0) return -1;
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&zip_), static_cast<mz_uint>(index), &stat) ||
            stat.m_is_directory || !stat.m_is_supported) {
            return -1;
        }
        if (size) *size = stat.m_uncomp_size;
        return index;
    }

    // A small entry into memory (at most `limit` bytes).
    bool read_small(const std::string& name, size_t limit, std::string* out) {
        std::uint64_t size = 0;
        const int index = find(name, &size);
        if (index < 0 || size > limit) return false;
        out->assign(static_cast<size_t>(size), '\0');
        return size == 0 ||
               mz_zip_reader_extract_to_mem(&zip_, static_cast<mz_uint>(index), out->data(), out->size(), 0) != 0;
    }

    // An entry through `write(data, size)` (false stops it).
    template <typename Write>
    bool extract(int index, Write& write) {
        return mz_zip_reader_extract_to_callback(
                   &zip_, static_cast<mz_uint>(index),
                   [](void* opaque, mz_uint64, const void* data, size_t n) -> size_t {
                       return (*static_cast<Write*>(opaque))(data, n) ? n : 0;
                   },
                   &write, 0) != 0;
    }

private:
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t n) {
        auto* self = static_cast<Zip*>(opaque);
        OVERLAPPED at = {};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        size_t done = 0;
        while (done < n) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>((std::min)(n - done, static_cast<size_t>(1) << 30));
            if (!ReadFile(self->file_.file, static_cast<char*>(buffer) + done, want, &got, &at) || got == 0) break;
            done += got;
            const mz_uint64 next = offset + done;
            at.Offset = static_cast<DWORD>(next);
            at.OffsetHigh = static_cast<DWORD>(next >> 32);
        }
        return done;
    }

    Handle file_;
    mz_zip_archive zip_ = {};
    bool open_ = false;
};

bool read_small_file(const std::wstring& path, size_t limit, std::string* out) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size = {};
    if (!file.ok() || !GetFileSizeEx(file.file, &size) || size.QuadPart > static_cast<LONGLONG>(limit)) return false;
    out->assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    return out->empty() ||
           (ReadFile(file.file, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
}

std::uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return static_cast<std::uint64_t>(-1);
    }
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

const char* const kManifests[] = {"translator.json", "voice.json", "voice-qwen3-asr.json", "glossary.json"};
const PackageKind kKinds[] = {PackageKind::kTranslator, PackageKind::kVoice, PackageKind::kQwenVoice,
                              PackageKind::kGlossary};

// The size of `name` in the package, or -1 when it does not have it.
std::uint64_t entry_size(const OfflinePackage& package, Zip* zip, const std::string& name) {
    if (!plain_name(name)) return static_cast<std::uint64_t>(-1);
    if (zip) {
        std::uint64_t size = 0;
        return zip->find(name, &size) >= 0 ? size : static_cast<std::uint64_t>(-1);
    }
    return file_size(package.path + L"\\" + utf8_to_wide(name));
}

}  // namespace

Status open_offline_package(const std::wstring& path, OfflinePackage* package) {
    *package = OfflinePackage();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return Status::kDisk;
    auto ends_with = [&](const wchar_t* ext) {
        const size_t n = wcslen(ext);
        return path.size() > n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
    };
    Zip zip;
    std::string manifest, signature;
    size_t which = std::size(kManifests);
    if (ends_with(L".zip")) {
        if (!zip.open(path)) return Status::kInvalid;
        package->zip = true;
        package->path = path;
        for (size_t i = 0; i < std::size(kManifests); ++i) {
            if (zip.find(kManifests[i], nullptr) < 0) continue;
            if (which != std::size(kManifests)) return Status::kInvalid;  // one manifest a package
            which = i;
        }
        if (which == std::size(kManifests) ||
            !zip.read_small(kManifests[which], kManifestLimit, &manifest) ||
            !zip.read_small(std::string(kManifests[which]) + ".sig", kSignatureLimit, &signature)) {
            return Status::kInvalid;
        }
    } else {
        // A manifest in a folder of downloaded files: the folder is the package.
        const size_t slash = path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        for (size_t i = 0; i < std::size(kManifests); ++i)
            if (_wcsicmp(name.c_str(), utf8_to_wide(kManifests[i]).c_str()) == 0) which = i;
        if (which == std::size(kManifests)) return Status::kInvalid;
        package->path = slash == std::wstring::npos ? L"." : path.substr(0, slash);
        if (!read_small_file(path, kManifestLimit, &manifest) ||
            !read_small_file(path + L".sig", kSignatureLimit, &signature)) {
            return Status::kInvalid;
        }
    }
    if (!verify_signature(manifest, trim(signature), builtin_public_key())) return Status::kInvalid;
    package->kind = kKinds[which];
    Zip* source = package->zip ? &zip : nullptr;
    auto has = [&](const std::string& name, std::uint64_t size) { return entry_size(*package, source, name) == size; };
    switch (package->kind) {
        case PackageKind::kTranslator:
            if (!parse_translator_manifest(manifest, "", &package->translator) ||
                !has(package->translator.model.file, package->translator.model.size) ||
                !has(package->translator.runtime.file, package->translator.runtime.size)) {
                return Status::kInvalid;
            }
            break;
        case PackageKind::kVoice:
        case PackageKind::kQwenVoice:
            if (!parse_voice_manifest(manifest, "", &package->voice, package->kind == PackageKind::kQwenVoice))
                return Status::kInvalid;
            for (const TranslatorFile& f : package->voice.files)
                if (!has(f.file, f.size)) return Status::kInvalid;
            break;
        case PackageKind::kGlossary: {
            std::vector<GlossaryPack> all;
            if (!parse_glossary_manifest(manifest, "", &all)) return Status::kInvalid;
            for (GlossaryPack& p : all)
                if (has(p.file, p.size)) package->glossaries.push_back(std::move(p));
            if (package->glossaries.empty()) return Status::kInvalid;
            break;
        }
        case PackageKind::kNone:
            return Status::kInvalid;
    }
    return Status::kOk;
}

Status extract_offline_file(const OfflinePackage& package, const std::string& file, std::uint64_t size,
                            const std::string& sha256, const std::wstring& path, const Progress& progress,
                            const std::atomic<bool>* cancel) {
    if (!plain_name(file)) return Status::kInvalid;
    const std::wstring directory = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(directory.c_str(), nullptr);
    ULARGE_INTEGER free_bytes = {};
    if (GetDiskFreeSpaceExW(directory.c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < size)
        return Status::kDisk;
    const std::wstring part = path + L".part";
    Handle out(CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out.ok()) return Status::kDisk;

    std::uint64_t done = 0;
    bool disk_error = false, cancelled = false;
    auto last_report = std::chrono::steady_clock::now();
    auto write = [&](const void* data, size_t n) -> bool {
        if (cancel && cancel->load()) {
            cancelled = true;
            return false;
        }
        if (done + n > size) return false;
        const char* bytes = static_cast<const char*>(data);
        while (n > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>((std::min)(n, static_cast<size_t>(1) << 30));
            if (!WriteFile(out.file, bytes, chunk, &written, nullptr) || written != chunk) {
                disk_error = true;
                return false;
            }
            bytes += chunk;
            n -= chunk;
            done += chunk;
        }
        const auto now = std::chrono::steady_clock::now();
        if (progress && now - last_report >= std::chrono::milliseconds(100)) {
            progress(done, size);
            last_report = now;
        }
        return true;
    };
    bool copied = false;
    if (progress) progress(0, size);
    if (package.zip) {
        Zip zip;
        std::uint64_t entry = 0;
        int index = -1;
        if (!zip.open(package.path) || (index = zip.find(file, &entry)) < 0 || entry != size) {
            CloseHandle(out.file);
            out.file = INVALID_HANDLE_VALUE;
            DeleteFileW(part.c_str());
            return Status::kInvalid;
        }
        copied = zip.extract(index, write);
    } else {
        Handle in(CreateFileW((package.path + L"\\" + utf8_to_wide(file)).c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        copied = in.ok();
        std::vector<char> buffer(1 << 20);
        while (copied) {
            DWORD got = 0;
            if (!ReadFile(in.file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
                copied = false;
                break;
            }
            if (got == 0) break;
            copied = write(buffer.data(), got);
        }
    }
    if (progress) progress(done, size);
    std::string hash;
    const bool good = copied && done == size && sha256_file(out.file, &hash) && hash == sha256;
    CloseHandle(out.file);
    out.file = INVALID_HANDLE_VALUE;
    if (!good) {
        DeleteFileW(part.c_str());
        return cancelled ? Status::kCancelled : disk_error ? Status::kDisk : Status::kInvalid;
    }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return Status::kDisk;
    return Status::kOk;
}

}  // namespace update
}  // namespace cxxime
