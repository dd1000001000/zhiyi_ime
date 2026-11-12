// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The ONNX Runtime environment voice input's models share (voice_recognizer.cc, qwen_asr.cc):
// one per process.
#ifndef CXXIME_ENGINE_VOICE_ORT_H_
#define CXXIME_ENGINE_VOICE_ORT_H_

#include <onnxruntime_cxx_api.h>

namespace cxxime {
namespace voice {
namespace detail {

Ort::Env& voice_ort_env();

}  // namespace detail
}  // namespace voice
}  // namespace cxxime

#endif  // CXXIME_ENGINE_VOICE_ORT_H_
