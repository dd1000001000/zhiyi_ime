// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The offline translation model (docs/learning-mode.md), downloaded by Settings into
// local_data_dir() (data_path.h):
//
//   translator\translator.json      what is installed: {"version": N, "dir": "v<N>",
//                                     "model": "<file>.gguf"}
//   translator\v<N>\<model>.gguf    Hy-MT2-1.8B, Q4_K_M
//   translator\v<N>\runtime\        llama.cpp (Vulkan build): llama-server.exe and its DLLs
//
// Each version has its own folder, so an update is installed while the model server may still
// run the previous one; Settings deletes old folders once they are not in use.
//
// zhiyi-server runs runtime\llama-server.exe on the chosen graphics card while candidates need
// translating (machine_translator.cc).
#ifndef CXXIME_TRANSLATOR_FILES_H_
#define CXXIME_TRANSLATOR_FILES_H_

namespace cxxime {

constexpr char kTranslatorDir[] = "translator";
constexpr char kTranslatorInstalled[] = "translator.json";
constexpr char kTranslatorRuntimeDir[] = "runtime";
constexpr char kTranslatorServerExe[] = "llama-server.exe";

}  // namespace cxxime

#endif  // CXXIME_TRANSLATOR_FILES_H_
