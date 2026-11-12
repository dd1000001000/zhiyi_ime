// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Voice input with Qwen3-ASR-0.6B (voice.model "qwen3-asr"; docs/voice-input.md): the official
// weights (Qwen Team, Alibaba; Apache-2.0) exported to ONNX and quantized without retraining
// (release voice-qwen3-asr-0.6b-preview of the main repository), run on the CPU:
//
//   qwen3-asr-encoder.onnx  mel (1, 128, F; F a multiple of 100), mel_len (1,) int64
//                           -> audio (A, 1024): the audio encoder (8-bit)
//   qwen3-asr-decoder.onnx  input_ids (1, S) int64, audio (A, 1024), past_key_i / past_value_i
//                           (1, 8, P, 128), i = 0..27 -> logits (1, 151936) of the last position,
//                           present_key_i / present_value_i: the 28-layer text decoder (attention
//                           and down_proj 8-bit, the rest 4-bit)
//   qwen3-asr-tokens.txt    per token id: its bytes in hex, a tab, 0 text / 1 special / 2 <asr_text>
//   qwen3-asr-mel.bin       128 x 201 float32: the processor's mel filters
//   qwen3-asr.json          the prompt's ids around the audio placeholders, the forced-language
//                           suffixes ("language Chinese<asr_text>"), the special ids
//
// The front end (qwen_log_mel) reproduces the official processor's features (Whisper-style log
// mel: 400-point frames every 10 ms, a periodic Hann window, 128 slaney mel bins, log10 floored
// 8 below the maximum, the clip padded to 30 s and to at least 0.5 s). Decoding is greedy.
//
// Text while speaking (start() / partial()) follows the official streaming method
// (qwen_asr.inference Qwen3ASRModel.streaming_transcribe): each time it is called, the whole
// recording so far goes through the model again, and once the recording is 4 s long (the
// official 2 chunks of 2 s) the previous output, without its last 5 tokens, is given as the start
// of the answer, so the text does not jump around. The final text (transcribe(), finish()) is the
// whole recording decoded afresh.
//
// While speaking, the encoder's 8 s windows that are whole (they do not see each other) and the
// decoder's keys and values for the prompt up to them are kept between calls, so a call costs
// about the same however long the recording is (0.2 to 0.45 s on a Ryzen 5 9600X, 4 threads).
#ifndef CXXIME_QWEN_ASR_H_
#define CXXIME_QWEN_ASR_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cxxime/voice_recognizer.h>

namespace cxxime {
namespace voice {

struct QwenFiles {
    std::wstring encoder, decoder, tokens, mel, config;
};

// The processor's log mel features of 16 kHz samples in [-1, 1] for the encoder: 128 x F
// (row-major, F = 100 * ceil(mel_len / 100)) with *mel_len the frames that are the recording's.
// filters: 128 x 201 (qwen3-asr-mel.bin).
std::vector<float> qwen_log_mel(const float* samples, std::size_t count, const std::vector<float>& filters,
                                std::size_t* mel_len, std::size_t* frames);

class QwenRecognizer {
public:
    QwenRecognizer();
    ~QwenRecognizer();
    QwenRecognizer(const QwenRecognizer&) = delete;
    QwenRecognizer& operator=(const QwenRecognizer&) = delete;

    // On the CPU with `threads` threads. False with *error when the files or the runtime cannot be
    // loaded.
    bool load(const QwenFiles& files, int threads, std::string* error);
    bool loaded() const;

    // The text of a whole recording (UTF-8, 16 kHz samples in [-1, 1]); empty when nothing was
    // said. False with *error when the model fails.
    bool transcribe(const float* samples, std::size_t count, Language language, std::string* text,
                    std::string* error);

    // Text while speaking: start() when recording starts, then partial() with the whole recording
    // so far each time it has grown (voice input: as soon as the last call is done and 0.5 s more
    // was recorded).
    void start(Language language);
    bool partial(const float* samples, std::size_t count, std::string* text, std::string* error);
    // The final text after start() / partial(): as transcribe() (the whole recording decoded afresh),
    // reusing what the calls before computed for the encoder windows that were already whole.
    bool finish(const float* samples, std::size_t count, std::string* text, std::string* error);

private:
    bool step(const float* samples, std::size_t count, bool final, std::string* text, std::string* error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voice
}  // namespace cxxime

#endif  // CXXIME_QWEN_ASR_H_
