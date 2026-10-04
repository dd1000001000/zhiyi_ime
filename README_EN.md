# Zhiyi IME (知意输入法)

**English** | [中文](README.md)

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)](https://www.microsoft.com/windows)
[![CMake](https://img.shields.io/badge/CMake-3.15%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)](https://cmake.org)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/dd1000001000/zhiyi_ime/windows-ci.yml?branch=main&label=Windows%20CI&style=flat-square)](https://github.com/dd1000001000/zhiyi_ime/actions/workflows/windows-ci.yml)
[![License](https://img.shields.io/github/license/dd1000001000/zhiyi_ime?style=flat-square)](LICENSE)

> Lightweight · Open source · Context-aware — a Windows Chinese/English input method that picks
> candidates from what you have already typed, on your own CPU

Zhiyi IME is a Windows TSF input method for Simplified Chinese pinyin, English word completion and
plain English letters. A small fine-tuned decision model ([Laya](https://huggingface.co/convaiinnovations/laya),
335 MB after int8 quantization) reads the text you have typed so far, picks the most likely candidate,
puts it first and marks it with a gold sparkle. The model runs entirely offline; nothing you type
leaves your computer.

## Features

- **Context-aware first candidate** for pinyin homophones (权利 / 权力 / 全力) and English completions,
  marked with a sparkle; the other candidates keep their frequency order. About 30 ms per key on 4 CPU threads.
- **Three input modes**: Chinese pinyin / English words (completions while typing, digits select) /
  English letters (typed straight through). Shift switches Chinese/English and `Ctrl+Space` switches
  words/letters by default; both are configurable in Settings.
- **Mixed input**: typing a complete English word in Chinese mode (`hello`, `wechat`) offers that word.
- **Case follows what you type**: `hel` → hello, `Hel` → Hello, `HEL` → HELLO.
- Inherits CxxIME's full pinyin / shuangpin / abbreviations / fuzzy pinyin, sentence composition,
  segmented selection, horizontal and vertical candidate windows and themes.
- User data lives in `%USERPROFILE%\zhiyi\` and is kept on uninstall.

## Accuracy

First-candidate accuracy on the test sets (int8 model on CPU):

| | Frequency order only | Zhiyi IME |
|---|---|---|
| Chinese homophones (800 samples) | 77.4% | 88.5% |
| English completions (840 samples) | 73.2% | 86.3% |

The model weights are published as a [release](https://github.com/dd1000001000/zhiyi_ime/releases/tag/model-zhen-r64); the training code and the training and evaluation data are not published because the
corpora are subject to third-party copyright.

## Building

```cmd
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.30.0 -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya model (~260 MB, GitHub release) -> models\laya\
build_laya.bat                        :: Ninja Release build into build\
build_laya.bat test                   :: unit tests
package_laya.bat                      :: installer (needs NSIS 3.x) into ..\output\
```

Requires Windows 10/11 x64, Visual Studio 2022 or newer (C++ workload), CMake 3.15+ and Python 3.10+.
64-bit only. Without the model the IME works normally, just without context reranking.
The English word list `data\english.words.tsv` is included (regenerate it with
`data\tools\build_english_dictionary.py`).

## Credits and licenses

Zhiyi IME is a modified version of [CxxIME](https://github.com/deanxyuan/cxx-ime) (Apache License 2.0,
Copyright (c) 2026 CxxIME Contributors). It keeps CxxIME's TSF front end, pinyin engine, candidate
window and settings app, and adds Laya context reranking, the English word mode, three-mode
switching and the recommendation mark, with a new name, icon and system registration identifiers.
See [NOTICE](NOTICE) for the list of changes.

- Zhiyi IME as a whole is licensed under **GPL-3.0-only** ([LICENSE](LICENSE))
- Source files taken from CxxIME keep their Apache License 2.0 notices (full text in
  [data/licenses/Apache-2.0.txt](data/licenses/Apache-2.0.txt))
- The Chinese pinyin dictionary and English word list come from [rime-ice](https://github.com/iDvel/rime-ice) (GPL-3.0-only)
- English word frequencies: [wordfreq](https://github.com/rspeer/wordfreq) (data CC BY-SA 4.0)
- The Laya model (Apache-2.0) and ONNX Runtime (MIT)

Full notices: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
