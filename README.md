# 知意输入法（Zhiyi IME）

[English](README_EN.md) | **中文**

[![Release](https://img.shields.io/github/v/release/dd1000001000/zhiyi_ime?style=flat-square)](https://github.com/dd1000001000/zhiyi_ime/releases/latest)
[![License](https://img.shields.io/github/license/dd1000001000/zhiyi_ime?style=flat-square)](LICENSE)

> 轻量 · 开源 · 懂上文 —— 根据你已经打过的字推荐候选的 Windows 中英文输入法

## 功能简介

- **懂上文的推荐**：一个在本机运行的小模型读取你前面打过的字，从候选里挑出最可能的那个放在第一位，
  并用蓝紫色星标标出（如“权利 / 权力 / 全力”）。模型不联网，输入内容不会离开你的电脑
- **拼音或五笔**：拼音支持全拼、首字母简拼和二者混合（`wsyige` → 我是一个），也可以切换成首字母模式
  （每个字母对应一个字，`zgr` → 中国人）
- **模糊音**：z=zh、c=ch、s=sh、n=l、an=ang、en=eng、in=ing 可以逐组开关，模糊匹配的词会标出正确拼音
- **英文输入**：单词联想（打几个字母就给出补全）或逐字母输入；打错时给出正确拼写，但不会自动替换
- **中英混输**：中文模式下打出完整的英文单词，该词会出现在候选里
- **自由设置切换键**：中英切换、输入方式、中英文标点、全角/半角都可以设置成自己习惯的按键，
  并提示与系统或其他程序快捷键的冲突
- **任务栏快速切换**：右键任务栏上的“中/英”可以切换中英文、全拼/首字母/五笔、标点、全角等
- **浅色 / 深色主题**，中英文界面，自学习（常用的词自动往前排）
- **自动更新**：打开设置时检查新版本，一键下载、校验并安装
- **隐私**：可选的用户体验改进计划，默认关闭，记录只保存在本机，详见[隐私说明](docs/privacy.md)

## 安装说明

**系统要求**：Windows 10 / 11（64 位）。

1. 从 [Releases](https://github.com/dd1000001000/zhiyi_ime/releases/latest) 下载最新的
   `zhiyi-v版本号-setup.exe`，双击运行并按提示安装（需要管理员权限）。
2. 安装完成后按 `Win + 空格` 切换到“知意输入法”。也可以在“设置 > 时间和语言 > 语言和区域”中
   把它设为默认输入法。
3. 打开设置：右键任务栏上的“中/英”，选择“设置…”。

- **更新**：有新版本时，打开设置会提醒你，在“更新”页点“立即更新”即可。已经打开的程序要重新打开
  才会用上新版本。
- **卸载**：在“设置 > 应用 > 已安装的应用”中卸载“知意输入法”，可以选择是否保留个人数据
  （位于 `%USERPROFILE%\zhiyi\`，包括设置和学习记录）。

## 致谢

知意输入法基于 [CxxIME](https://github.com/deanxyuan/cxx-ime) 修改而来，感谢以下项目：

- [CxxIME](https://github.com/deanxyuan/cxx-ime)：输入法框架、拼音引擎、候选窗与设置程序（Apache License 2.0）
- [rime-ice（雾凇拼音）](https://github.com/iDvel/rime-ice)：中文拼音词库与英文词表（GPL-3.0）
- [Laya](https://huggingface.co/convaiinnovations/laya)：上文推荐所用的模型（Apache-2.0）
- [wordfreq](https://github.com/rspeer/wordfreq)：英文词频（CC BY-SA 4.0）
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)：模型推理（MIT）

知意输入法按 [GPL-3.0-only](LICENSE) 发布，各组件的完整声明见 [NOTICE](NOTICE) 和
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。开发与构建说明见 [docs/development.md](docs/development.md)。
