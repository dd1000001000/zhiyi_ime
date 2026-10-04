// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Checks a published release the way Settings > Updates does (set ZHIYI_UPDATE_TEST_BASE to
// try a local server). Never starts the installer.

#include <cstdio>
#include <cwchar>
#include <string>

#include <cxxime/update.h>

namespace {

const char* status_name(cxxime::update::Status status) {
    switch (status) {
    case cxxime::update::Status::kOk: return "ok";
    case cxxime::update::Status::kNotFound: return "not found";
    case cxxime::update::Status::kNetwork: return "network error";
    case cxxime::update::Status::kInvalid: return "invalid (manifest, signature or hash)";
    case cxxime::update::Status::kDisk: return "disk error";
    case cxxime::update::Status::kCancelled: return "cancelled";
    }
    return "?";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || (std::wcscmp(argv[1], L"check") != 0 &&
                     !(std::wcscmp(argv[1], L"download") == 0 && argc == 3))) {
        std::fprintf(stderr, "usage: zhiyi-update-tool check | download <folder>\n");
        return 2;
    }
    const auto result = cxxime::update::check_latest("0.0.0");
    std::printf("check: %s\n", status_name(result.status));
    if (result.status != cxxime::update::Status::kOk) return 1;
    const auto& manifest = result.manifest;
    std::printf("version %s (%s), %llu bytes\nsha256 %s\nurl %s\n", manifest.version.c_str(),
                manifest.published.c_str(), static_cast<unsigned long long>(manifest.size),
                manifest.sha256.c_str(), manifest.url.c_str());
    if (std::wcscmp(argv[1], L"download") != 0) return 0;
    std::wstring path;
    const auto status = cxxime::update::download_installer(
        manifest, argv[2],
        [](std::uint64_t done, std::uint64_t total) {
            std::printf("\r%llu / %llu", static_cast<unsigned long long>(done),
                        static_cast<unsigned long long>(total));
        },
        nullptr, &path);
    std::printf("\ndownload: %s\n", status_name(status));
    if (status == cxxime::update::Status::kOk) std::wprintf(L"%ls\n", path.c_str());
    return status == cxxime::update::Status::kOk ? 0 : 1;
}
