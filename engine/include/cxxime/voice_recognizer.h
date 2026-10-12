// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input (docs/voice-input.md): speech to text on this machine. SenseVoice-Small (the int8
// ONNX export of sherpa-onnx) turns a whole recording into text in one pass, on the CPU or a
// graphics card through DirectML; Silero VAD (v4) only tells whether the recording has speech at
// all. Settings downloads the model files (voice_files.h); the server records and runs them
// (server/src/voice_service.cc). Both use the ONNX Runtime the Laya model uses.
#ifndef CXXIME_VOICE_RECOGNIZER_H_
#define CXXIME_VOICE_RECOGNIZER_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cxxime {
namespace voice {

constexpr int kSampleRate = 16000;
constexpr std::size_t kFbankBins = 80;
// SenseVoice's frames: 7 filter bank frames stacked, every 6th (60 ms).
constexpr std::size_t kStackedFrames = 7;
constexpr std::size_t kStackedShift = 6;
constexpr std::size_t kStackedDim = kFbankBins * kStackedFrames;
// The VAD reads 32 ms windows.
constexpr std::size_t kVadWindow = 512;

// Kaldi's log mel filter bank of 16 kHz samples in [-1, 1], the front end SenseVoice was trained
// with: 25 ms frames every 10 ms (the last partial frame dropped), DC removed, pre-emphasis 0.97,
// Hamming window, 512-point FFT, 80 mel bins from 20 Hz to 8 kHz. Row-major, frames x 80.
std::vector<float> fbank(const float* samples, std::size_t count, std::size_t* frames);
// SenseVoice's input frames: kStackedFrames filter bank frames every kStackedShift, the first
// frame repeated (kStackedFrames - 1) / 2 times before and the last one after as needed (the
// recognizer then normalizes them with the model's mean and deviation). Row-major,
// stacked x kStackedDim.
std::vector<float> stack_frames(const std::vector<float>& fbank, std::size_t frames,
                                std::size_t* stacked);

enum class Language { kAuto, kMandarin, kEnglish, kCantonese };

// "auto", "zh", "en", "yue" (voice.language); anything else is kAuto.
Language parse_language(const std::string& name);

// Whether onnxruntime.dll can be loaded (it is delay-loaded: no ONNX Runtime call may come
// before this is true).
bool runtime_available();

// Threads for the CPU: half the cores, 1 to 4 (the server and Settings' speed test alike).
int cpu_threads();

// Settings' speed test (voice.device): how long the model takes for kTimedSeconds of sound on a
// graphics card (gpu_device, a DirectML adapter index) or the CPU (-1), in ms: the median of
// three runs of lengths it has not seen (about kTimedSeconds; a card prepares each new length, as
// for every recording) after a first one. 0 with *error when it does not run there.
constexpr int kTimedSeconds = 5;
double time_recognition(const std::wstring& model, const std::wstring& tokens, int gpu_device,
                        std::string* error);

class SpeechRecognizer {
public:
    SpeechRecognizer();
    ~SpeechRecognizer();
    SpeechRecognizer(const SpeechRecognizer&) = delete;
    SpeechRecognizer& operator=(const SpeechRecognizer&) = delete;

    // model: the .onnx file, tokens: its tokens.txt. gpu_device: a DirectML adapter index
    // (GpuAdapter::index), or -1 for the CPU with `threads` threads. False with *error when the
    // files or the runtime cannot be loaded.
    bool load(const std::wstring& model, const std::wstring& tokens, int gpu_device, int threads,
              std::string* error);
    bool loaded() const;
    // The text of a recording (UTF-8, 16 kHz samples in [-1, 1]); empty when nothing was said.
    // itn: numbers, dates and the like as digits ("3点" instead of "三点"). False with *error
    // when the model fails.
    bool transcribe(const float* samples, std::size_t count, Language language, bool itn,
                    std::string* text, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Silero VAD, fed 32 ms windows in order: whether someone is speaking now, and whether anyone
// spoke since reset().
class SpeechDetector {
public:
    SpeechDetector();
    ~SpeechDetector();
    SpeechDetector(const SpeechDetector&) = delete;
    SpeechDetector& operator=(const SpeechDetector&) = delete;

    bool load(const std::wstring& model, std::string* error);
    bool loaded() const;
    void reset();
    // One window of kVadWindow samples. True while speaking: a window scores 0.5 or more, and
    // for 300 ms after the last such window. False on a model error.
    bool accept(const float* window);
    // Speech of at least 250 ms (8 windows in a row scoring 0.5 or more) since reset().
    bool heard() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voice
}  // namespace cxxime

#endif  // CXXIME_VOICE_RECOGNIZER_H_
