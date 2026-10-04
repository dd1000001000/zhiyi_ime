// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/update.h>

#include <filesystem>
#include <fstream>

#include "support/testutil.h"

namespace {

using cxxime::update::Manifest;

constexpr char kPrefix[] = "https://github.com/dd1000001000/zhiyi_ime/releases/download/";
constexpr char kSha[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string manifest_json(const std::string& version, const std::string& file,
                          const std::string& url, const std::string& sha = kSha) {
    return R"({"version": ")" + version + R"(", "file": ")" + file + R"(", "url": ")" + url +
           R"(", "size": 3, "sha256": ")" + sha +
           R"(", "published": "2026-10-04", "notes": {"zh-CN": "修复", "en-US": "Fixes"}})";
}

std::filesystem::path temp_folder(const wchar_t* name) {
    const auto folder = std::filesystem::temp_directory_path() / L"zhiyi-update-test" / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

TEST(Update, parses_a_release_manifest) {
    const std::string url = std::string(kPrefix) + "v0.6.3/zhiyi-v0.6.3-setup.exe";
    Manifest manifest;
    ASSERT_TRUE(cxxime::update::parse_manifest(
        manifest_json("0.6.3", "zhiyi-v0.6.3-setup.exe", url), kPrefix, &manifest));
    ASSERT_EQ(manifest.version, "0.6.3");
    ASSERT_EQ(manifest.size, 3u);
    ASSERT_EQ(manifest.notes_for("zh-CN"), "修复");
    ASSERT_EQ(manifest.notes_for("ja-JP"), "Fixes");  // English when the language is missing
}

TEST(Update, rejects_unsafe_manifests) {
    Manifest manifest;
    const std::string file = "zhiyi-v0.6.3-setup.exe";
    const std::string url = std::string(kPrefix) + "v0.6.3/" + file;
    // Another host, another folder, plain http.
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6.3", file, "https://example.com/v0.6.3/" + file), kPrefix, &manifest));
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6.3", file, std::string(kPrefix) + "v0.6.4/" + file), kPrefix,
        &manifest));
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6.3", file, "http://github.com/dd1000001000/zhiyi_ime/releases/download/"
                                     "v0.6.3/" + file),
        kPrefix, &manifest));
    // A file name with a path, a bad hash, a bad version, not JSON.
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6.3", "..\\zhiyi-v0.6.3-setup.exe", url), kPrefix, &manifest));
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6.3", file, url, "ABC"), kPrefix, &manifest));
    ASSERT_TRUE(!cxxime::update::parse_manifest(
        manifest_json("0.6", "zhiyi-v0.6-setup.exe",
                      std::string(kPrefix) + "v0.6/zhiyi-v0.6-setup.exe"),
        kPrefix, &manifest));
    ASSERT_TRUE(!cxxime::update::parse_manifest("not json", kPrefix, &manifest));
}

TEST(Update, offers_only_newer_releases) {
    ASSERT_TRUE(cxxime::update::is_newer_release("0.6.3", "0.6.2"));
    ASSERT_TRUE(cxxime::update::is_newer_release("0.7.0", "0.6.10"));
    ASSERT_TRUE(cxxime::update::is_newer_release("0.6.2", "0.6.2-dev"));
    ASSERT_TRUE(!cxxime::update::is_newer_release("0.6.2", "0.6.2"));
    ASSERT_TRUE(!cxxime::update::is_newer_release("0.6.1", "0.6.2"));
    ASSERT_TRUE(!cxxime::update::is_newer_release("0.7.0-rc.1", "0.6.2"));  // pre-release
    ASSERT_TRUE(!cxxime::update::is_newer_release("garbage", "0.6.2"));
}

TEST(Update, verifies_manifest_signatures) {
    // A test key (not the release key) and its signature of `data`.
    const std::string key =
        "U08IeHsXu2Hh/gh99vqz/R58zphFNZDx2Jxis1FNR/9n0VRYwMJGloumxAyxwgMjJEjTDVh10STtUu+A+EGTQw==";
    const std::string signature =
        "8wlbzwuxc334QsU+ynFLnB7zmcL7fwEOMgkc7Q0qRR/RA3kdLjlr5but0wPAjYIbES38r8sIZ4f+LGP0uf91hw==";
    const std::string data = "{\"version\": \"0.6.3\"}\n";
    ASSERT_TRUE(cxxime::update::verify_signature(data, signature, key));
    ASSERT_TRUE(!cxxime::update::verify_signature("{\"version\": \"9.9.9\"}\n", signature, key));
    ASSERT_TRUE(!cxxime::update::verify_signature(data, signature,
                                                  cxxime::update::builtin_public_key()));
    ASSERT_TRUE(!cxxime::update::verify_signature(data, "not base64!", key));
    ASSERT_EQ(cxxime::update::builtin_public_key().size(), 88u);
}

TEST(Update, hashes_files) {
    const auto folder = temp_folder(L"hash");
    write_file(folder / L"abc.txt", "abc");
    std::string hex;
    ASSERT_TRUE(cxxime::update::sha256_file((folder / L"abc.txt").wstring(), &hex));
    ASSERT_EQ(hex, kSha);
    ASSERT_TRUE(!cxxime::update::sha256_file((folder / L"missing").wstring(), &hex));
}

TEST(Update, reuses_a_verified_download_and_cleans_old_ones) {
    const auto folder = temp_folder(L"download");
    write_file(folder / L"zhiyi-v0.6.3-setup.exe", "abc");
    write_file(folder / L"zhiyi-v0.6.1-setup.exe", "old");
    write_file(folder / L"zhiyi-v0.6.2-setup.exe.part", "ol");
    Manifest manifest;
    manifest.version = "0.6.3";
    manifest.file = "zhiyi-v0.6.3-setup.exe";
    manifest.url = "http://127.0.0.1:9/zhiyi-v0.6.3-setup.exe";  // never contacted
    manifest.size = 3;
    manifest.sha256 = kSha;
    std::wstring path;
    std::uint64_t reported = 0;
    ASSERT_EQ(cxxime::update::download_installer(
                  manifest, folder.wstring(),
                  [&](std::uint64_t done, std::uint64_t) { reported = done; }, nullptr, &path),
              cxxime::update::Status::kOk);
    ASSERT_EQ(path, (folder / L"zhiyi-v0.6.3-setup.exe").wstring());
    ASSERT_EQ(reported, 3u);
    ASSERT_TRUE(!std::filesystem::exists(folder / L"zhiyi-v0.6.1-setup.exe"));
    ASSERT_TRUE(!std::filesystem::exists(folder / L"zhiyi-v0.6.2-setup.exe.part"));

    // A changed file is not reused: downloaded again (no server here).
    write_file(folder / L"zhiyi-v0.6.3-setup.exe", "abd");
    ASSERT_EQ(cxxime::update::download_installer(manifest, folder.wstring(), {}, nullptr, &path),
              cxxime::update::Status::kNetwork);
    ASSERT_TRUE(!std::filesystem::exists(folder / L"zhiyi-v0.6.3-setup.exe"));
}

TEST(Update, keeps_the_state) {
    const auto folder = temp_folder(L"state");
    const std::wstring path = (folder / L"update-state.json").wstring();
    ASSERT_EQ(cxxime::update::read_state(path).skipped_version, "");
    cxxime::update::State state;
    state.skipped_version = "0.6.3";
    state.pending_version = "0.6.4";
    ASSERT_TRUE(cxxime::update::write_state(state, path));
    const auto read = cxxime::update::read_state(path);
    ASSERT_EQ(read.skipped_version, "0.6.3");
    ASSERT_EQ(read.pending_version, "0.6.4");
}

RUN_ALL_TESTS()
