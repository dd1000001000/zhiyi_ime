# Zhiyi IME (知意输入法)

**English** | [中文](README.md)

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)](https://www.microsoft.com/windows)
[![CMake](https://img.shields.io/badge/CMake-3.15%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)](https://cmake.org)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/dd1000001000/zhiyi_ime/windows-ci.yml?branch=main&label=Windows%20CI&style=flat-square)](https://github.com/dd1000001000/zhiyi_ime/actions/workflows/windows-ci.yml)
[![Release](https://img.shields.io/github/v/release/dd1000001000/zhiyi_ime?style=flat-square)](https://github.com/dd1000001000/zhiyi_ime/releases/latest)
[![License](https://img.shields.io/github/license/dd1000001000/zhiyi_ime?style=flat-square)](LICENSE)

> Lightweight · Open source · Context-aware — a Windows Chinese/English input method that picks
> candidates by the text before the caret

<a href="docs/media/zhiyi-intro.mp4"><img src="docs/media/zhiyi-intro-preview.webp" alt="Zhiyi IME intro video" width="100%"></a>

<p align="center"><a href="docs/media/zhiyi-intro.mp4">▶ Watch the intro video: four people, four stories (1:58 · Chinese voices · Chinese and English subtitles)</a></p>

## Features

- **Context-aware recommendation**: a small model running on your computer reads the text before
  the caret, puts the most likely candidate first and marks it with a blue-purple star (权利 / 权力 /
  全力). The model works offline; nothing you type leaves your computer. With a dedicated graphics
  card the model can run on it instead (DirectML: NVIDIA, AMD and Intel cards; Settings tests the
  speed first and uses the card only when it is faster than the CPU).
- **Learning mode (candidate translations)**: each candidate shows what it means in another language,
  with English part-of-speech labels (n. v. adj. …) and up to three common senses, so you pick up a
  language while typing. Chinese candidates can be shown in English, Japanese, Korean, French,
  German, Spanish or Russian, English candidates in Chinese, Japanese, Korean, French, German, Spanish
  or Russian; words with several readings are translated by reading. Translations come from offline
  language packs (about 40,000 Chinese and 20,000 English words) looked up on your computer. The
  installer includes Chinese → English and English → Chinese; the others are downloaded on the
  Learning page in Settings. The word lists are open at
  [zhiyi-glossary](https://github.com/dd1000001000/zhiyi-glossary): corrections welcome. Phrases,
  rare words and short sentences the packs lack can be translated by a downloadable on-device model
  (about 0.4 GB, distilled from Tencent Hy-MT2-7B) on a graphics card or the CPU (about 0.3 s a page
  on the CPU): a page at once, filled in as it arrives, offline. While typing, hold Ctrl and press
  1-9 to type that candidate's translation (its first sense) instead of the candidate: `xianzai`
  then Ctrl+1 types `now`; a candidate without a translation types nothing.
- **Choose where data lives**: the user dictionary, learning records, language packs, model caches
  and downloaded models can move to another drive with one click on the General page.
- **Full pinyin, initials, Wubi, shuangpin**: full pinyin also accepts initials and a mix of both
  (`wsyige` → 我是一个); in initials mode each letter is one character (`zgr` → 中国人); shuangpin
  comes in the Microsoft, Xiaohe, Ziranma, Sogou and Smart ABC schemes and shares the dictionary
  and learning records with full pinyin. While typing, the style switch key (Ctrl+Space by default) cycles the
  four; the input box always shows the keys as typed.
- **Fuzzy pinyin**: z=zh, c=ch, s=sh, n=l, an=ang, en=eng, in=ing, each pair on or off; words found
  through a fuzzy pair show their correct pinyin.
- **English input**: word completion or letter by letter; misspelled words get their correct spelling
  as a candidate, never replaced automatically.
- **Mixed input**: a complete English word typed in Chinese mode is offered among the candidates.
- **Your own switch keys** for Chinese/English, input style, punctuation and full/half width, with
  warnings about conflicts with Windows or other programs.
- **Taskbar menu**: right-click the 中/英 indicator to switch Chinese/English, full pinyin / initials /
  Wubi / shuangpin, punctuation, full width and more, or to exit the background service (it starts again when you
  switch to Zhiyi IME).
- **Candidate window**: vertical (default) or horizontal, light and dark themes, three font sizes;
  Chinese and English interface.
- **Self-learning**: words you use move up; Backspace right after a commit undoes what it learned.
- **Backup and transfer**: export all settings, the lexicon and the learning records to one file and
  import it on another computer or after reinstalling; nothing is imported from an incomplete or
  damaged backup.
- **Optional start at sign-in**: the background service can start the first time you switch to
  Zhiyi IME instead.
- **Automatic updates**: Settings checks for a new version when it opens and installs it in one click;
  language packs have their own update check.
- **Privacy**: an optional user experience program, off by default, with records kept on your computer
  only; see the [privacy notes](docs/privacy.en.md).

<img src="docs/media/learning-mode.png" alt="Learning mode: Chinese candidates in English and Japanese, English candidates in Chinese" width="100%">

## Recommendation accuracy

How often the right word comes first, and how fast and how large each model is, on the same test sets and the same candidates: word frequency only (no context), [metasequoiaime/pinyin-ime-reranker](https://huggingface.co/metasequoiaime/pinyin-ime-reranker-25M) (4M / 25M, another open model that reranks pinyin candidates by the preceding text; Chinese only), the untuned multilingual [Laya](https://huggingface.co/convaiinnovations/laya) model, and the models of Zhiyi 1.1, 1.2, 1.4 and 1.5.4. The 1.2 model is the 1.1 model distilled into the smaller mmBERT-small, with more training text from LCCC and Chinese C4: it is more accurate, about twice as fast on the CPU, and takes about half the memory, video memory and disk space. 1.4 ships a unified model: 1.2 plus a composition head that reads the typed pinyin and scores every character at every syllable, so long pinyin typed in one go is composed by the model and comes out right far more often ((i) (j); 4-16 characters in full pinyin: 46.6% to 60.7%). In exchange it takes about 5 ms more per page on the CPU, and picks homophones and English completions 0.9 and 1.6 points less often than 1.2, while on the real IME candidates it is 0.5 points ahead. 1.5.4 replaces the composition head with a decoder that writes the sentence a character at a time, each character given the context, all the typed pinyin and the characters already written; the engine keeps 16 paths over the dictionary's words and takes the best sentence, and the composition part was retrained on long-form web text with real preceding context. Against 1.4: long input in full pinyin 60.7% to 61.4% (11-16 characters 53.2% to 55.4%), last syllable an initial 30.6% to 30.8%, first syllables abbreviated 16.2% to 18.1%; real IME candidates 78.1% to 78.5%, homophones and English completion unchanged; word choice no longer computes the composition head on the way, so a page is back to 1.2's 13 ms on the CPU. Test details are in the [development notes](docs/development.md#上文推荐模型laya) (Chinese).

<img src="docs/media/compare.en-US.svg" alt="Candidate-ranking models: (a) real Chinese candidates (b) Chinese homophones (c) English completion (d) CPU time (e) GPU time (f) memory (g) video memory (h) model file (i) long input in full pinyin (j) long input, last syllable an initial" width="100%">

## Offline translation model

Candidates the learning-mode language packs lack are translated by a model on your computer. From 1.3.0 it is Zhiyi v2: [LMT-60-0.6B](https://huggingface.co/NiuTrans/LMT-60-0.6B) distilled from Tencent [Hy-MT2-7B](https://huggingface.co/tencent/Hy-MT2-7B)'s translations of about 1.15 million IME candidates (short words, sentences typed in one go and English words, with the text before the caret). Compared with Hy-MT2 1.8B used before, it is about as accurate (better from Chinese into English, a little worse from Chinese into the other languages), a third of the download, faster on the graphics card with less video memory, and it runs on the CPU too (about 0.3 s a page), so computers without a dedicated graphics card can use it.

<img src="docs/media/translator.en-US.svg" alt="Offline translation models: (a) all (b) Chinese to English (c) Chinese to other languages (d) English to other languages (e) GPU time (f) CPU time (g) video memory (h) memory (i) download size" width="100%">

## Installation

**Requirements**: Windows 10 / 11 (64-bit).

1. Download the latest `zhiyi-v<version>-setup.exe` from
   [Releases](https://github.com/dd1000001000/zhiyi_ime/releases/latest) and run it (administrator
   approval is needed).
2. Press `Win + Space` to switch to Zhiyi IME, or make it the default input method in Windows Settings
   > Time & language > Language & region.
3. To open Settings, right-click 中/英 on the taskbar and choose Settings….

- **Updates**: when a new version is out, Settings tells you; click Update now on its Updates page.
  Programs already open use the new version after they are reopened.
- **Moving to another computer or reinstalling**: Export… on the Dictionary page of Settings, then
  Import… on the new computer.
- **Uninstalling**: uninstall Zhiyi IME in Windows Settings > Apps > Installed apps; you can keep your
  personal data (settings, learned words and downloaded language packs in `%USERPROFILE%\zhiyi\`).

## Credits

Zhiyi IME is a modified version of [CxxIME](https://github.com/deanxyuan/cxx-ime). Thanks to:

- [CxxIME](https://github.com/deanxyuan/cxx-ime): the input method framework, pinyin engine,
  candidate window and settings app (Apache License 2.0)
- [rime-ice](https://github.com/iDvel/rime-ice): the Chinese pinyin dictionary and English word list (GPL-3.0)
- [Laya](https://huggingface.co/convaiinnovations/laya): the recommendation model's architecture and the teacher it was distilled from (Apache-2.0)
- [mmBERT](https://huggingface.co/jhu-clsp/mmBERT-small): the recommendation model's encoder, mmBERT-small (MIT)
- [LMT-60](https://huggingface.co/NiuTrans/LMT-60-0.6B) (NiuTrans): the base of the offline translation model (Apache-2.0)
- [Hy-MT2](https://huggingface.co/tencent/Hy-MT2-7B) (Tencent Hunyuan): the first offline translation model (1.8B) and the teacher it was distilled from (7B) (Apache-2.0)
- [llama.cpp](https://github.com/ggml-org/llama.cpp): the offline translation runtime (MIT, downloaded with the model)
- [LCCC](https://github.com/thu-coai/CDial-GPT) (MIT) and the Chinese part of [C4](https://huggingface.co/datasets/allenai/c4) (ODC-BY): part of the distillation text
- [SkyPile-150B](https://huggingface.co/datasets/Skywork/SkyPile-150B) (Skywork, Skywork Community License): about 70% of the training text for long-pinyin composition since 1.5.4
- [wordfreq](https://github.com/rspeer/wordfreq): English word frequencies (CC BY-SA 4.0)
- [ONNX Runtime](https://github.com/microsoft/onnxruntime): model inference (MIT)
- [DirectML](https://github.com/microsoft/DirectML): inference on graphics cards (Microsoft DirectML
  license, redistributable)

The learning-mode language packs were generated offline by large language models (GLM, Gemma), then
checked automatically and corrected by hand; the word lists are open under GPL-3.0 at
[zhiyi-glossary](https://github.com/dd1000001000/zhiyi-glossary).

Zhiyi IME is licensed under [GPL-3.0-only](LICENSE). The models Zhiyi trained itself (the recommendation model and offline translation model v2) are licensed under GPL-3.0-only too; the voice models (SenseVoice-Small, Silero VAD) and the llama.cpp runtime are not Zhiyi's and keep their own licenses, which come with the download; full notices are in [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). Development and build notes:
[docs/development.md](docs/development.md).
