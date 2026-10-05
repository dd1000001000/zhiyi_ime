# 开发说明

面向维护者与贡献者：构建、测试、打包发布、推荐模型与文档索引。

## 环境要求

- Windows 10/11 x64（只支持 64 位）
- Visual Studio 2022 或更新版本（C++ 桌面开发工作负载；默认路径见 `build_laya.bat` 与 `package_laya.bat`，
  可用环境变量 `VS_PATH`、`VS_GENERATOR` 覆盖）
- CMake 3.15+、Python 3.10+
- 打包需要 [NSIS 3.x](https://nsis.sourceforge.io/)

## 构建与测试

```cmd
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.30.0 -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya 模型（GitHub Release model-zhen-r64）-> models\laya\
build_laya.bat                        :: Ninja Release 构建，产物在 build\
build_laya.bat test                   :: 构建并运行全部测试
```

- 开发构建（`CXXIME_PRODUCTION_BUILD=OFF`）直接读取源码目录下的 `data\`，不需要安装即可运行测试和工具。
- 没有模型时输入法照常工作，只是不做上文推荐。
- `build.bat` 是沿用自 CxxIME 的 Visual Studio 生成器构建（`build.bat debug` 为 Debug），产物在各目录的
  `Debug\` / `Release\` 子目录。
- 调试工具见 [tools/README.md](../tools/README.md)，词典数据与生成工具见 [data/README.md](../data/README.md)。
- 英文词表 `data\english.words.tsv` 已随仓库提供，可用 `data\tools\build_english_dictionary.py` 重新生成。

已安装的输入法模块（TSF DLL）由各程序在启动时加载，测试新版本前要重新打开目标程序；可以用
`Get-Process | ForEach-Object { $_.Modules } | Where-Object ModuleName -like 'zhiyi_tsf*'` 查看各进程加载的版本。

## 打包与发布

```cmd
package_laya.bat                      :: 安装包 -> ..\output\zhiyi-v<VERSION>-setup.exe
```

安装包与安装流程见 [安装、更新与卸载](installation.md)；发布到 GitHub Release（含签名的更新清单
`latest.json`）的步骤见 [scripts/README.md](../scripts/README.md)。版本号在仓库根目录的 `VERSION`，
每个版本的更新说明放在 `docs/release-notes/<版本>.zh-CN.md` 与 `<版本>.en-US.md`。

## 上文推荐模型（Laya）

- 微调后的 [Laya](https://huggingface.co/convaiinnovations/laya)（LoRA r64，中英联合训练），导出为 int8 ONNX，
  在 CPU 上推理（默认 4 线程），每次约 30 ms。
- 模型权重随 [Release model-zhen-r64](https://github.com/dd1000001000/zhiyi_ime/releases/tag/model-zhen-r64) 发布；
  训练代码与训练、评测数据涉及第三方语料版权，不公开。
- 推荐规则：只处理第一页；模型在前 2 × 每页候选数个候选中，从与首选覆盖同一段输入、字数相同的同类候选里
  挑出一个放到第一位并加星标，其余保持词频顺序。英文单词模式下，补全与拼写纠正一起比较，纠正按改动量扣分。
  实现见 `engine/src/laya_rerank.cc`，配置项见 [设置指南](settings-guide.md) 的 `laya.*`。

测试集上的首选准确率（int8 模型，CPU）：

| | 只按词频排序 | 知意输入法 |
|---|---|---|
| 中文同音词（800 条） | 77.4% | 88.5% |
| 英文单词补全（840 条） | 73.2% | 86.3% |

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
