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
    std::string name;     // the version as shown after "v" ("4-1026"); "" = `version`
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

// Voice input's model (docs/voice-input.md, voice_files.h), in the release tagged "voice" (not
// marked latest):
//   voice.json      {"version": N, "min_app": "1.5.0",
//                    "files": [{"file", "size", "sha256"}, ...]}
//   voice.json.sig  signed like latest.json
//   the files: model.int8.onnx, tokens.txt, silero_vad.onnx (all three required) and the
//   models' licenses (LICENSE-*.txt)
// Qwen3-ASR (from 1.6.6, voice.model "qwen3-asr") is in the same release, voice-qwen3-asr.json
// (+ .sig, the same format): qwen3-asr-encoder.onnx, qwen3-asr-decoder.onnx, qwen3-asr-tokens.txt,
// qwen3-asr-mel.bin, qwen3-asr.json and silero_vad.onnx (all six required; the VAD is the same
// file as SenseVoice's) and its license. Older versions read only voice.json.
inline constexpr char kVoiceReleasePath[] = "voice/";  // under kDownloadPrefix
inline constexpr char kVoiceManifestFile[] = "voice.json";
inline constexpr char kQwenVoiceManifestFile[] = "voice-qwen3-asr.json";

struct VoiceManifest {
    std::uint32_t version = 0;
    std::string min_app;  // the oldest program version that runs it ("" = any)
    std::vector<TranslatorFile> files;

    std::uint64_t total_size() const;
};

// qwen: voice-qwen3-asr.json's files instead of voice.json's.
bool parse_voice_manifest(const std::string& text, const std::string& download_prefix,
                          VoiceManifest* manifest, bool qwen = false);

struct VoiceCheckResult {
    Status status = Status::kNetwork;
    VoiceManifest manifest;
};

// Downloads and verifies voice.json, or voice-qwen3-asr.json with qwen (blocking; run on a worker
// thread).
VoiceCheckResult check_voice(bool qwen = false);

// Offline packages (docs/offline-packages.md, issue #52): for a computer that cannot download from
// GitHub, each downloadable part also comes as one zip, zhiyi-offline-<part>-v<N>.zip (on ModelScope,
// kOfflinePackagesUrl): the release's signed manifest (translator.json,
// voice.json, voice-qwen3-asr.json or glossary.json, with its .sig) and the files it lists, stored
// uncompressed. Settings' "Import from file" reads one: the manifest's signature as for a download,
// then each file's size and SHA-256 as it is copied out. The files of a release, downloaded one by
// one into a folder, are read the same way from that folder (the manifest chosen).
inline constexpr wchar_t kOfflinePackagesUrl[] = L"https://www.modelscope.cn/models/dd1000001000/zhiyi_packages/files";

enum class PackageKind { kNone, kTranslator, kVoice, kQwenVoice, kGlossary };

struct OfflinePackage {
    PackageKind kind = PackageKind::kNone;
    std::wstring path;  // the zip, or the folder of the chosen manifest
    bool zip = false;
    TranslatorManifest translator;          // kTranslator
    VoiceManifest voice;                    // kVoice, kQwenVoice
    std::vector<GlossaryPack> glossaries;   // kGlossary: those the package holds
};

// Opens a package (a .zip, or a manifest .json with its .sig and files beside it): finds its manifest,
// checks the signature with the built-in key and parses it. kInvalid: not a package, a bad signature
// or a file the manifest lists missing from the package (glossary packages may hold some of their packs);
// kDisk: the file cannot be read.
Status open_offline_package(const std::wstring& path, OfflinePackage* package);

// Copies one of the package's files to `path` (via path.part), checking that it has `size` bytes and
// the SHA-256 `sha256`. `progress` and `cancel` as for download_installer.
Status extract_offline_file(const OfflinePackage& package, const std::string& file, std::uint64_t size,
                            const std::string& sha256, const std::wstring& path, const Progress& progress,
                            const std::atomic<bool>* cancel);

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
