// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input's model (docs/voice-input.md), downloaded by Settings into local_data_dir()
// (data_path.h) from the release tagged "voice" (update.h):
//
//   voice\voice.json         what is installed: {"version": N, "dir": "v<N>"}
//   voice\v<N>\model.int8.onnx  SenseVoice-Small, sherpa-onnx's int8 export
//   voice\v<N>\tokens.txt       its tokens
//   voice\v<N>\silero_vad.onnx  Silero VAD v4
//   voice\v<N>\LICENSE-*.txt    the models' licenses
//
// Each version has its own folder, so an update is installed while the server may still have
// the previous one loaded; Settings deletes old folders once they are not in use.
#ifndef CXXIME_VOICE_FILES_H_
#define CXXIME_VOICE_FILES_H_

#include <cstdint>
#include <string>

namespace cxxime {
namespace voice {

constexpr char kVoiceDir[] = "voice";
constexpr char kVoiceInstalled[] = "voice.json";
constexpr char kVoiceModelFile[] = "model.int8.onnx";
constexpr char kVoiceTokensFile[] = "tokens.txt";
constexpr char kVoiceVadFile[] = "silero_vad.onnx";

struct InstalledModel {
    std::uint32_t version = 0;
    std::wstring dir;     // ...\voice\v<N>\ (ends in '\')
    std::wstring model;   // model.int8.onnx
    std::wstring tokens;  // tokens.txt
    std::wstring vad;     // silero_vad.onnx
};

// The model in local_data_dir()\voice, when all its files are there.
bool find_installed(InstalledModel* installed);

}  // namespace voice
}  // namespace cxxime

#endif  // CXXIME_VOICE_FILES_H_
