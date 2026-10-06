// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Read-only view of a whole file. The dictionary files are mapped rather than copied onto the
// heap: their pages stay file-backed (shared, not private commit, droppable under memory
// pressure) and are prefetched once so lookups do not fault on first touch.
#ifndef CXXIME_MAPPED_FILE_H_
#define CXXIME_MAPPED_FILE_H_

#include <cstddef>
#include <string>

namespace cxxime {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Maps the file and prefetches its pages. False (and no mapping) on any failure.
    bool open(const std::string& path);
    void close();

    // Moves an existing file at `path` out of the way (renamed, then deleted when possible) so a
    // writer can create a new one there: a mapped file cannot be truncated or deleted in place.
    static bool rename_away(const std::string& path);

    const char* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }

private:
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace cxxime

#endif // CXXIME_MAPPED_FILE_H_
