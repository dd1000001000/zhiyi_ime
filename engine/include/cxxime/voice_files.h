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
//
// Qwen3-ASR-0.6B (voice.model "qwen3-asr", from 1.6.6; update.h: voice-qwen3-asr.json in the same
// release) goes into a folder of its own beside it, so versions before 1.6.6, which clear voice\
// of every folder but their model's, leave it alone:
//
//   voice-qwen3-asr\voice.json            as above
//   voice-qwen3-asr\v<N>\qwen3-asr-encoder.onnx, qwen3-asr-decoder.onnx  (qwen_asr.h)
//   voice-qwen3-asr\v<N>\qwen3-asr-tokens.txt, qwen3-asr-mel.bin, qwen3-asr.json
//   voice-qwen3-asr\v<N>\silero_vad.onnx  the same VAD
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
constexpr char kQwenVoiceDir[] = "voice-qwen3-asr";
constexpr char kQwenEncoderFile[] = "qwen3-asr-encoder.onnx";
constexpr char kQwenDecoderFile[] = "qwen3-asr-decoder.onnx";
constexpr char kQwenTokensFile[] = "qwen3-asr-tokens.txt";
constexpr char kQwenMelFile[] = "qwen3-asr-mel.bin";
constexpr char kQwenConfigFile[] = "qwen3-asr.json";

// voice.model: which recognizer voice input uses.
enum class VoiceModel {
    kSenseVoice,  // "sensevoice" (the default): SenseVoice-Small, the whole recording at once
    kQwen,        // "qwen3-asr": Qwen3-ASR-0.6B, the text shown while speaking (CPU only)
};
// "qwen3-asr" is kQwen; anything else kSenseVoice.
VoiceModel parse_voice_model(const std::string& name);
const char* voice_model_name(VoiceModel model);

struct InstalledModel {
    VoiceModel kind = VoiceModel::kSenseVoice;
    std::uint32_t version = 0;
    std::wstring dir;     // ...\voice\v<N>\ (ends in '\')
    std::wstring model;   // model.int8.onnx; Qwen: qwen3-asr-encoder.onnx
    std::wstring tokens;  // tokens.txt; Qwen: qwen3-asr-tokens.txt
    std::wstring vad;     // silero_vad.onnx
    // Qwen only.
    std::wstring decoder;  // qwen3-asr-decoder.onnx
    std::wstring mel;      // qwen3-asr-mel.bin
    std::wstring config;   // qwen3-asr.json
};

// The folder in local_data_dir() of each model ("voice", "voice-qwen3-asr").
const char* voice_model_dir(VoiceModel model);
// The model in local_data_dir()\<voice_model_dir()>, when all its files are there.
bool find_installed(VoiceModel model, InstalledModel* installed);
// SenseVoice (as before 1.6.6).
bool find_installed(InstalledModel* installed);

}  // namespace voice
}  // namespace cxxime

#endif  // CXXIME_VOICE_FILES_H_
