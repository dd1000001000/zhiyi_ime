// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/mapped_file.h>

#include <windows.h>

namespace cxxime {

bool MappedFile::open(const std::string& path) {
    close();
    // FILE_SHARE_DELETE: a new dictionary is put in place by renaming the mapped file away and
    // moving the new one in (a mapped file cannot be truncated or deleted, only renamed).
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<ULONGLONG>(size.QuadPart) > static_cast<ULONGLONG>(SIZE_MAX)) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return false;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<const char*>(view);
    size_ = static_cast<std::size_t>(size.QuadPart);

    // Bring the whole file in now: the dictionaries are read all over from the first key.
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<char*>(data_), size_};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
    return true;
}

bool MappedFile::rename_away(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;  // nothing there
    }
    if (DeleteFileA(path.c_str())) {
        return true;
    }
    const std::string aside = path + ".old-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64());
    if (!MoveFileExA(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }
    DeleteFileA(aside.c_str());  // best effort; fails while the old file is still mapped
    return true;
}

void MappedFile::close() {
    if (data_) {
        UnmapViewOfFile(data_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
    if (file_) {
        CloseHandle(file_);
    }
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
    size_ = 0;
}

} // namespace cxxime
