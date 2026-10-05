# 开发说明

面向维护者与贡献者：构建、测试、打包发布、推荐模型与文档索引。

## 环境要求

- Windows 10/11 x64（只支持 64 位）
- Visual Studio 2022 或更新版本（C++ 桌面开发工作负载；默认路径见 `build_laya.bat`，可用环境变量
  `VS_PATH` 覆盖）
- CMake 3.15+、Python 3.10+

## 公开范围

公开仓库包含输入法本身的全部源码（引擎、TSF 模块、后台服务、候选窗口、设置程序、更新、安装辅助程序、
IMM 兼容模块）、构建脚本，以及生成运行时词典的工具（`data/tools/`、`scripts/` 中的词典脚本、`tools/topn_index`）。
**打包与发布工具、测试、调试与基准工具不公开**；技术文档里提到的测试文件、工具名称和命令指向这些
不公开的部分，仅供参考。CMake 只在这些目录存在时才构建它们。

## 构建与测试

```cmd
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.30.0 -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya 模型（GitHub Release model-zhen-guess-r64）-> models\laya\
build_laya.bat                        :: Ninja Release 构建，产物在 build\
```

- 开发构建（`CXXIME_PRODUCTION_BUILD=OFF`）直接读取源码目录下的 `data\`，不需要安装即可运行测试和工具。
- 没有模型时输入法照常工作，只是不做上文推荐。
- `build.bat` 是沿用自 CxxIME 的 Visual Studio 生成器构建（`build.bat debug` 为 Debug），产物在各目录的
  `Debug\` / `Release\` 子目录。
- 生成运行时词典见 [scripts/README.md](../scripts/README.md)，词典数据与工具见 [data/README.md](../data/README.md)。
- 英文词表 `data\english.words.tsv` 已随仓库提供，可用 `data\tools\build_english_dictionary.py` 重新生成。

已安装的输入法模块（TSF DLL）由各程序在启动时加载，测试新版本前要重新打开目标程序；可以用
`Get-Process | ForEach-Object { $_.Modules } | Where-Object ModuleName -like 'zhiyi_tsf*'` 查看各进程加载的版本。

## 发布

安装包和 GitHub Release（含签名的更新清单 `latest.json`）由维护者用不公开的打包工具生成，安装程序做了什么见
[安装、更新与卸载](installation.md)。版本号在仓库根目录的 `VERSION`，每个版本的更新说明放在
`docs/release-notes/<版本>.zh-CN.md` 与 `<版本>.en-US.md`。

## 上文推荐模型（Laya）

- 微调后的 [Laya](https://huggingface.co/convaiinnovations/laya)（LoRA r64，中英联合训练），导出为 int8 ONNX，
  在 CPU 上推理（默认 4 线程），每次约 20–35 ms。
- 模型只看上文和候选词，不看拼音或已打的字母（“猜词”提示词，模型目录的 `rl_agent_config.json` 中
  `"zhiyi_prompt": "guess"`；没有这一项的旧模型仍用带拼音的提示词）。训练时中文上文是上一句加本句，
  英文是最后 192 个字符。
- 模型权重随 [Release model-zhen-guess-r64](https://github.com/dd1000001000/zhiyi_ime/releases/tag/model-zhen-guess-r64)
  发布（`scripts/fetch_model.py`）；训练代码与训练、评测数据涉及第三方语料版权，不公开。
- 上文：开始输入时，TSF 模块读取输入框里光标前（有选中文字时为选区之前）最多 256 个字（`SET_CONTEXT`），
  模型取其中最后 128 个字 / 192 个英文字符。程序不允许读取时，改用之前上屏的文字，换到另一个输入框时清空；
  密码框和标为私密的输入框不读取。实现见 `tsf/src/text_before_caret.cpp`。
- 推荐规则：只处理第一页；模型在前 2 × 每页候选数个候选中，从与首选覆盖同一段输入、字数相同的同类候选里
  挑出一个放到第一位并加星标，其余保持词频顺序。英文单词模式下，补全与拼写纠正一起比较，纠正按改动量扣分。
  实现见 `engine/src/laya_rerank.cc`，配置项见 [设置指南](settings-guide.md) 的 `laya.*`。

测试集上的首选准确率（int8 模型，CPU）：

| | 只按词频排序 | 知意输入法 |
|---|---|---|
| 中文同音词，上文只有本句（800 条） | 77.4% | 88.5% |
| 中文同音词，上文含上一句（同 800 条） | 77.4% | 91.0% |
| 英文单词补全（840 条） | 73.2% | 87.4% |

## 源码约定

- 知意输入法整体按 GPL-3.0-only 发布；来自 CxxIME 的源文件保留 Apache License 2.0 声明，新文件使用
  `// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.`
- 内部命名空间、宏与 CMake 目标沿用 CxxIME 的 `cxxime` / `CXXIME_`，便于合并上游；对外的文件名、注册标识与
  数据目录使用 `zhiyi` / `ZhiyiIME`。改动列表见 [NOTICE](../NOTICE)。

## 文档索引

| 文档 | 内容 |
|------|------|
| [架构总览](architecture.md) | 模块划分、技术选型、目录结构 |
| [安装、更新与卸载](installation.md) | 打包、安装程序、多版本安装、卸载、诊断包 |
| [路径解析](path-resolution.md) | 程序目录与用户目录的结构、路径函数 |
| [设置指南](settings-guide.md) | 设置程序、任务栏菜单、配置文件 |
| [隐私说明](privacy.md)（[English](privacy.en.md)） | 用户体验改进计划记录的内容 |
| [组合输入](ascii-composer.md) | 内联 ASCII、修饰键切换、符号输入 |
| [候选排序设计](candidate-ordering.md) | 手动固定 / 学习偏好 / 默认排序 / 分页 |
| [候选词选词算法](candidate-selection.md) | 拼音与五笔的查询管道 |
| [五笔候选质量排序](wubi-candidate-ranking.md) | 五笔默认排序与验收基线 |
| [查询预算与候选收集](query-control.md) | QueryBudget、TopKCollector、超时检查点 |
| [短输入快速路径](short-input-fast-path.md) | Top-N 索引与短码缓存 |
| [词典系统设计](dictionary.md) | 拼写索引、二进制词典格式、构建流程 |
| [用户词库与候选偏好](user-dictionary.md) | 用户词、学习记录与固定顺序的存储 |
| [IPC 架构设计](ipc-architecture.md) | 命名管道、IOCP 事件循环、关闭协议 |
| [共享资源预加载](shared-resources.md) | 共享资源、全局状态与热重载 |
| [可观测性与 Benchmark](observability.md) | QueryTrace、TSF 事件追踪、日志、基准 |
