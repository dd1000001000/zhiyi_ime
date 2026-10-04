# 知意输入法（Zhiyi IME）

[English](README_EN.md) | **中文**

> 轻量 · 开源 · 懂上文 —— 在本地 CPU 上根据上文推荐候选的 Windows 中英文输入法

知意输入法是一款 Windows TSF 输入法，支持简体中文拼音、英文单词补全与英文字母输入。
它用一个经过微调的小型决策模型（[Laya](https://huggingface.co/convaiinnovations/laya)，int8 量化后 335 MB）
读取你已经输入的上文，从输入法给出的候选里挑出最可能的那个词，放在第一位并用金色星光标出。
模型完全在本地运行，不联网，输入内容不会离开你的电脑。

## 特性

- **懂上文的首选**：拼音同音词（如"权利 / 权力 / 全力"）和英文补全都由模型按上文挑选首选，带星光标记；
  其余候选保持原有的词频顺序。每次按键约 30 ms（4 线程 CPU）
- **三种输入模式**：中文拼音 / 英文单词（打字母时给出补全，数字键选词）/ 英文字母（逐个字母直接上屏）。
  中英切换默认 Shift，英文单词与字母切换默认 `Ctrl+Space`，都可在设置中修改
- **中英混输**：中文模式下打出完整的英文单词（如 `hello`、`wechat`），该词会出现在候选里
- **大小写跟随**：英文补全跟随已打字母的大小写（`hel` → hello，`Hel` → Hello，`HEL` → HELLO）
- 继承 CxxIME 的全拼 / 双拼 / 简拼 / 模糊音、动态组句、多段选词、横竖排候选窗与多套主题
- 用户数据位于 `%USERPROFILE%\zhiyi\`，卸载时默认保留

## 效果

测试集上的首选准确率（int8 模型，CPU）：

| | 只按词频排序 | 知意输入法 |
|---|---|---|
| 中文同音词（800 条） | 77.4% | 88.5% |
| 英文单词补全（840 条） | 73.2% | 86.3% |

评测、训练与量化流程见 [bench/README.md](bench/README.md)。

## 从源码构建

```cmd
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.30.0 -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya 模型 (约 260 MB, GitHub Release) -> models\laya\
build_laya.bat                        :: Ninja Release 构建（产物在 build\）
build_laya.bat test                   :: 运行单元测试
```

环境要求：Windows 10/11 x64、Visual Studio 2022 或更新版本（C++ 工作负载）、CMake 3.15+、Python 3.10+。
仅支持 64 位。没有模型时输入法照常工作，只是不做上文推荐。
英文词表 `data\english.words.tsv` 已随仓库提供，可用 `data\tools\build_english_dictionary.py` 重新生成；
模型的训练、评测与量化在 [bench/](bench/README.md)。

## 致谢与许可证

知意输入法基于 [CxxIME](https://github.com/deanxyuan/cxx-ime)（Apache License 2.0，Copyright (c) 2026 CxxIME Contributors）
修改而来：保留了它的 TSF 前端、拼音引擎、候选窗与设置程序，新增了 Laya 上文重排、英文单词模式、
三种输入模式切换与推荐标记，并更换了名称、图标与系统注册标识。改动说明见 [NOTICE](NOTICE)。

- 程序代码：Apache License 2.0（[LICENSE](LICENSE)）
- 中文拼音词库与英文词表来自 [rime-ice](https://github.com/iDvel/rime-ice)（GPL-3.0-only），
  因此包含词库的发行版整体按 GPL-3.0 分发
- 英文词频来自 [wordfreq](https://github.com/rspeer/wordfreq)（数据 CC BY-SA 4.0）
- Laya 模型（Apache-2.0）与 ONNX Runtime（MIT）

各组件的完整声明见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。
