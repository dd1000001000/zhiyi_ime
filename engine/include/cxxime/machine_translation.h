// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Offline translation of candidates the language pack has no translation for
// (docs/learning-mode.md): the downloaded model run by llama.cpp's llama-server.exe
// (translator_files.h) on a graphics card or the CPU, asked over HTTP on 127.0.0.1 with a random
// key. One request per candidate; the server's parallel slots translate a page at once.
#ifndef CXXIME_MACHINE_TRANSLATION_H_
#define CXXIME_MACHINE_TRANSLATION_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cxxime {
namespace mt {

// The parallel slots: a page has at most this many candidates to translate.
constexpr int kParallel = 10;

// learning.translator_device for the processor instead of a graphics card.
inline constexpr char kCpuDevice[] = "cpu";

// How a model is asked (translator.json "prompt"; the model was trained on it):
//   ""            Hy-MT2-1.8B (translator v1): its own Chinese template, through the chat API
//   "zhiyi-mt-1"  the distilled model (v2): student_prompt(), raw through /completion
inline constexpr char kStudentPrompt[] = "zhiyi-mt-1";

struct InstalledModel {
    std::wstring dir;     // ...\translator\ (ends in '\')
    std::wstring model;   // the .gguf file
    std::wstring server;  // runtime\llama-server.exe
    std::uint32_t version = 0;
    std::string prompt;   // "" or kStudentPrompt
};

// The model and runtime in local_data_dir()\translator, when both are there.
bool find_installed(InstalledModel* installed);

struct Item {
    std::string target;  // en ja ko fr de es ru zh
    std::string text;
    std::string source = "zh";  // the candidate's language: zh, or en for English candidates
};

// Whether a candidate is worth translating: some letters, at most 32 characters, not only
// digits and symbols.
bool translatable(const std::string& text);

// The model's answer as shown after a candidate: the first line, trimmed, without quotes;
// empty when it only repeats the candidate.
std::string clean_translation(const std::string& source, const std::string& answer);

// The prompt for one candidate (Hy-MT2's templates), with the text before it when given.
std::string translation_prompt(const std::string& target, const std::string& text,
                               const std::string& context);

// The distilled model's prompt (kStudentPrompt), byte for byte what it was trained on
// (bench/mt_student.py; a test compares the two):
//   <|im_start|>system\nTranslate the input method candidate into English. ...<|im_end|>\n
//   <|im_start|>user\nContext: <text before>\nChinese: <candidate><|im_end|>\n
//   <|im_start|>assistant\n
// without the Context line when there is no text before.
std::string student_prompt(const Item& item, const std::string& context);

// One running llama-server.exe. Not thread-safe except translate(), which may run from several
// threads at once.
class LlamaServer {
public:
    LlamaServer();
    ~LlamaServer();
    LlamaServer(const LlamaServer&) = delete;
    LlamaServer& operator=(const LlamaServer&) = delete;

    // Starts it on the graphics card `device_key` (gpu_adapters.h), or the processor for
    // kCpuDevice, and waits until it is ready and warmed up (a few seconds). False with a
    // log-style reason in *error.
    bool start(const InstalledModel& installed, const std::string& device_key, std::string* error);
    void stop();
    bool running() const;
    const std::string& device() const { return device_; }

    // The translations, in the order of `items` (empty where it failed), asked in parallel.
    std::vector<std::string> translate(const std::vector<Item>& items, const std::string& context,
                                       int timeout_ms = 10000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string device_;
    std::string prompt_;  // InstalledModel::prompt of the running model
};

}  // namespace mt
}  // namespace cxxime

#endif  // CXXIME_MACHINE_TRANSLATION_H_
