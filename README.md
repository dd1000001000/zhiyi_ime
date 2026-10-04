# 知意输入法（Zhiyi IME）

[English](README_EN.md) | **中文**

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)](https://www.microsoft.com/windows)
[![CMake](https://img.shields.io/badge/CMake-3.15%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)](https://cmake.org)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/dd1000001000/zhiyi_ime/windows-ci.yml?branch=main&label=Windows%20CI&style=flat-square)](https://github.com/dd1000001000/zhiyi_ime/actions/workflows/windows-ci.yml)
[![License](https://img.shields.io/github/license/dd1000001000/zhiyi_ime?style=flat-square)](LICENSE)

> 轻量 · 开源 · 懂上文 —— 在本地 CPU 上根据上文推荐候选的 Windows 中英文输入法

知意输入法是一款 Windows TSF 输入法，支持简体中文拼音、英文单词补全与英文字母输入。
它用一个经过微调的小型决策模型（[Laya](https://huggingface.co/convaiinnovations/laya)，int8 量化后 335 MB）
读取你已经输入的上文，从输入法给出的候选里挑出最可能的那个词，放在第一位并用金色星光标出。
模型完全在本地运行，不联网，输入内容不会离开你的电脑。

## 特性

- **懂上文的首选**：拼音同音词（如"权利 / 权力 / 全力"）和英文补全都由模型按上文挑选首选，带星光标记；
  模型在前 2×候选词数 个候选里挑选，其余候选保持原有的词频顺序。每次按键约 30–40 ms（4 线程 CPU）
- **中文输入二选一**：拼音或五笔，在设置里选择。拼音支持全拼（也能识别首字母简拼）和首字母模式
  （每个字母对应一个字，如 `zgr` → 中国人）
- **模糊音**：z=zh、c=ch、s=sh、n=l、an=ang、en=eng、in=ing 可逐组勾选；模糊匹配出的词在后面用括号标出
  正确拼音，如 `zongguo` → 中国(zhong guo)
- **英文输入**：英文单词（打字母时给出补全，数字键选词）/ 英文字母（逐个字母直接上屏）
- **英文拼写纠错**：打错时在候选里给出正确拼写（`teh` → the，`recieve` → receive，`beautf` → beautiful），
  第一个候选始终是原文，不会自动替换。选过两次的纠正会排到推荐位；原样上屏两次的词（如 `kubectl`）
  不再纠错，并会作为补全出现
- **切换键**：中英切换默认 Shift；`Ctrl+Space` 在中文拼音下切换全拼 / 首字母，在英文下切换单词 / 字母，
  都可在设置中修改。状态栏显示当前方式（拼 / 首 / 五、英 / a）
- **中英混输**：中文模式下打出完整的英文单词（如 `hello`、`wechat`），该词会出现在第二位
- **大小写跟随**：英文补全跟随已打字母的大小写（`hel` → hello，`Hel` → Hello，`HEL` → HELLO）
- **简洁的设置**：常规（中文输入、拼音方式、浅色 / 深色主题、字号、候选词数 3–10、界面语言、
  英文拼写纠错）、模糊音、按键、词库（自学习开关、清除学习记录）、关于。设置与安装程序都支持中文和英文：安装时可选择语言
  （默认跟随系统），所选语言也会用作设置界面的语言
- **精简词库**：约 40 万词条（全部单字 + 常用词），生僻词可逐字输入，自学习会记住新词
- 用户数据位于 `%USERPROFILE%\zhiyi\`，卸载时可选择是否保留

## 效果

测试集上的首选准确率（int8 模型，CPU）：

| | 只按词频排序 | 知意输入法 |
|---|---|---|
| 中文同音词（800 条） | 77.4% | 88.5% |
| 英文单词补全（840 条） | 73.2% | 86.3% |

模型权重随 [Release](https://github.com/dd1000001000/zhiyi_ime/releases/tag/model-zhen-r64) 发布；训练代码与训练、评测数据涉及第三方语料版权，不公开。

## 从源码构建

```cmd
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.30.0 -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya 模型 (约 260 MB, GitHub Release) -> models\laya\
build_laya.bat                        :: Ninja Release 构建（产物在 build\）
build_laya.bat test                   :: 运行单元测试
package_laya.bat                      :: 生成安装包（需要 NSIS 3.x），输出到 ..\output\
```

环境要求：Windows 10/11 x64、Visual Studio 2022 或更新版本（C++ 工作负载）、CMake 3.15+、Python 3.10+。
仅支持 64 位。没有模型时输入法照常工作，只是不做上文推荐。
英文词表 `data\english.words.tsv` 已随仓库提供，可用 `data\tools\build_english_dictionary.py` 重新生成。

## 致谢与许可证

知意输入法基于 [CxxIME](https://github.com/deanxyuan/cxx-ime)（Apache License 2.0，Copyright (c) 2026 CxxIME Contributors）
修改而来：保留了它的 TSF 前端、拼音引擎、候选窗与设置程序，新增了 Laya 上文重排、英文单词模式、
三种输入模式切换与推荐标记，并更换了名称、图标与系统注册标识。改动说明见 [NOTICE](NOTICE)。

- 知意输入法整体按 **GPL-3.0-only** 发布（[LICENSE](LICENSE)）
- 来自 CxxIME 的源文件保留其 Apache License 2.0 声明（全文见 [data/licenses/Apache-2.0.txt](data/licenses/Apache-2.0.txt)）
- 中文拼音词库与英文词表来自 [rime-ice](https://github.com/iDvel/rime-ice)（GPL-3.0-only）
- 英文词频来自 [wordfreq](https://github.com/rspeer/wordfreq)（数据 CC BY-SA 4.0）
- Laya 模型（Apache-2.0）与 ONNX Runtime（MIT）

各组件的完整声明见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。
