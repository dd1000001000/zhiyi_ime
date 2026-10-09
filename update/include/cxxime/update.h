// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Updates from GitHub Releases. The settings program checks when it opens (Config::update_notify)
// or on request (Settings > Updates); nothing else in the IME uses the network.
//
// Every release has two extra assets next to the installer (scripts/update_signing.py):
//   latest.json      {"version", "file", "url", "size", "sha256", "published", "notes": {lang}}
//   latest.json.sig  base64 ECDSA P-256 signature (r || s) over the exact bytes of latest.json
// The public key is built in (update_public_key.inc); the private key never leaves the release
// machine. An installer is started only when the manifest signature and its SHA-256 match.
#ifndef CXXIME_UPDATE_H_
#define CXXIME_UPDATE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

namespace cxxime {
namespace update {

inline constexpr char kReleaseBase[] =
    "https://github.com/dd1000001000/zhiyi_ime/releases/latest/download/";
inline constexpr char kDownloadPrefix[] =
    "https://github.com/dd1000001000/zhiyi_ime/releases/download/";
inline constexpr wchar_t kReleasesPage[] =
    L"https://github.com/dd1000001000/zhiyi_ime/releases/latest";
// Development only: another base URL for latest.json and the installers (e.g. a local server,
// http allowed on 127.0.0.1 / localhost). The signature is still checked with the built-in key.
inline constexpr wchar_t kTestBaseVariable[] = L"ZHIYI_UPDATE_TEST_BASE";

struct Manifest {
    std::string version;
    std::string file;  // installer file name, e.g. zhiyi-v0.6.3-setup.exe
    std::string url;
    std::uint64_t size = 0;
    std::string sha256;  // lowercase hex
    std::string published;
    std::map<std::string, std::string> notes;  // UI language -> release notes

    // The notes in `language`, else English, else any.
    std::string notes_for(const std::string& language) const;
};

// Parses latest.json. False when a field is missing or unsafe: the installer must be a file
// named like zhiyi-v<version>-setup.exe under `download_prefix`.
bool parse_manifest(const std::string& text, const std::string& download_prefix,
                    Manifest* manifest);

// True when `candidate` is a release (not a pre-release) newer than `current`.
bool is_newer_release(const std::string& candidate, const std::string& current);

// ECDSA P-256 / SHA-256 signature check; `public_key_base64` is X || Y (64 bytes).
bool verify_signature(const std::string& data, const std::string& signature_base64,
                      const std::string& public_key_base64);
const std::string& builtin_public_key();

// SHA-256 of a file (lowercase hex); false when it cannot be read.
bool sha256_file(HANDLE file, std::string* hex);
bool sha256_file(const std::wstring& path, std::string* hex);

enum class Status {
    kOk,
    kNotFound,      // no release information (HTTP 404)
    kNetwork,       // no connection, timeout, HTTP error
    kInvalid,       // bad manifest or signature, or installer hash mismatch
    kDisk,          // the installer could not be written
    kCancelled,
};

struct CheckResult {
    Status status = Status::kNetwork;
    Manifest manifest;
    bool newer = false;
};

// Downloads and verifies latest.json (blocking; run on a worker thread).
CheckResult check_latest(const std::string& current_version);

// Downloads the installer into `directory` (resumed from a .part file), then checks size and
// SHA-256. `progress(done, total)` is called as data arrives; `cancel` stops the download.
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
Status download_installer(const Manifest& manifest, const std::wstring& directory,
                          const Progress& progress, const std::atomic<bool>* cancel,
                          std::wstring* path);

// Checks the installer again with the file locked against writes, then starts it elevated in
// update mode (/UPDATE). ERROR_CANCELLED in *error when the user declined the UAC prompt.
bool launch_installer(const std::wstring& path, const std::string& sha256, HWND owner,
                      DWORD* error);

// %USERPROFILE%\zhiyi\updates (created).
std::wstring download_directory();
// Deletes downloaded installers except `keep` (a file name; empty: all).
void clean_downloads(const std::wstring& directory, const std::wstring& keep = {});
// Deletes downloaded installers (and partial downloads) of `current_version` or older. Run at
// every settings start: right after an update the installer that opened settings may still be
// running, so its file cannot be deleted yet.
void clean_installed_downloads(const std::wstring& directory, const std::string& current_version);

// Learning mode language packs (docs/learning-mode.md), in the release tagged "glossary" (not
// marked latest):
//   glossary.json      {"packs": [{"id", "source", "target", "version", "format", "min_app",
//                                  "file", "size", "sha256", "entries"}, ...]}
//   glossary.json.sig  signed like latest.json
//   <id>.v<version>.gloss
inline constexpr char kGlossaryReleasePath[] = "glossary/";  // under kDownloadPrefix

struct GlossaryPack {
    std::string id;  // <source>-<target>
    std::string source;
    std::string target;
    std::uint32_t version = 0;
    std::uint32_t format = 0;
    std::string min_app;  // the oldest program version that reads it ("" = any)
    std::string file;
    std::string url;
    std::uint64_t size = 0;
    std::string sha256;
    std::uint32_t entries = 0;
};

bool parse_glossary_manifest(const std::string& text, const std::string& download_prefix,
                             std::vector<GlossaryPack>* packs);

struct GlossaryCheckResult {
    Status status = Status::kNetwork;
    std::vector<GlossaryPack> packs;
};

// Downloads and verifies glossary.json (blocking; run on a worker thread).
GlossaryCheckResult check_glossaries();

// Downloads a pack to `path` (via path.part, resumed), checking its size and SHA-256. The
// installed pack may be mapped by the server, so the caller moves it into place.
// `progress` and `cancel` as for download_installer.
Status download_glossary(const GlossaryPack& pack, const std::wstring& path,
                         const Progress& progress, const std::atomic<bool>* cancel);

// The offline translation model (docs/learning-mode.md, translator_files.h), in the release
// tagged "translator" (not marked latest):
//   translator.json      {"version": N, "min_app": "1.2.0", "prompt": "zhiyi-mt-1" (optional),
//                         "model": {"file", "size", "sha256"},
//                         "runtime": {"file", "size", "sha256"}}
//   translator.json.sig  signed like latest.json
//   <model>.gguf, <runtime>.zip (llama.cpp's llama-server and its DLLs, with their licenses)
inline constexpr char kTranslatorReleasePath[] = "translator/";  // under kDownloadPrefix

struct TranslatorFile {
    std::string file;
    std::string url;
    std::uint64_t size = 0;
    std::string sha256;
};

struct TranslatorManifest {
    std::uint32_t version = 0;
    std::string min_app;  // the oldest program version that runs it ("" = any)
    std::string prompt;   // how the model is asked (machine_translation.h; "" = Hy-MT2's way)
    TranslatorFile model;    // .gguf
    TranslatorFile runtime;  // .zip
};

bool parse_translator_manifest(const std::string& text, const std::string& download_prefix,
                               TranslatorManifest* manifest);

struct TranslatorCheckResult {
    Status status = Status::kNetwork;
    TranslatorManifest manifest;
};

// Downloads and verifies translator.json (blocking; run on a worker thread).
TranslatorCheckResult check_translator();

// Downloads one of its files to `path` (via path.part, resumed), checking size and SHA-256.
Status download_translator_file(const TranslatorFile& file, const std::wstring& path,
                                const Progress& progress, const std::atomic<bool>* cancel);

// %USERPROFILE%\zhiyi\update-state.json: a version the user skipped, and the version being
// installed (so the next settings start can say it was updated).
struct State {
    std::string skipped_version;
    std::string pending_version;
};
State read_state(const std::wstring& path = {});
bool write_state(const State& state, const std::wstring& path = {});

}  // namespace update
}  // namespace cxxime

#endif  // CXXIME_UPDATE_H_
