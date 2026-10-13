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
python scripts\fetch_onnxruntime.py   :: ONNX Runtime 1.24.4（DirectML 版）+ DirectML.dll -> third_party\onnxruntime\
python scripts\fetch_model.py         :: Laya 模型（GitHub Release model-zhen-unified-dec）-> models\laya\
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

- [Laya](https://huggingface.co/convaiinnovations/laya) 结构的决策模型，编码器是 mmBERT-small（22 层、宽 384）。1.2.0 起由 1.1 的模型（mmBERT-base 上中英联合训练的 LoRA r64）蒸馏而来，训练样本 111 万条（原有的输入法实际候选样本，加上从 LCCC 和 C4 中文抽取、由输入法生成候选的样本）。导出为 int8 ONNX，在 CPU 上推理（默认 4 线程），每次约 10–20 ms；显卡上约 6 ms。
- 1.4.0 起是“统一模型”：在 1.2 的模型上加第二个头（2 层），两个头一起训练、从更大的统一教师模型蒸馏。第一个头照旧在候选里选词；第二个头读已打的拼音（每个音节一个槽位，`unit_ids` / `slot_pos`，单位和字表在模型目录的 `unified_vocab.json`），给每个音节位置上的每个字打分，用于长拼音组句（见下文“长拼音组句”）。没有 `unified_vocab.json` 或模型没有第二个头时只选词。
- 1.5.4 起第二个头换成解码器（2 层，带交叉注意力）：编码器读一遍上文和已打的拼音（`laya.int8g.onnx` 输出 `memory` / `slot_vec`），解码器（`laya_decoder.int8.onnx`，`unified_vocab.json` 的 `decoder` 字段指明）按顺序每次写一个字，每个字都看上文、全部音节和前面已经写下的字。组句部分用 315 万条长拼音样本重新训练（七成来自带真实上文的长篇网页文本 SkyPile-150B，三成对话），两个头都从更大的同结构教师模型蒸馏。引擎认出 `decoder` 字段就用解码器组句（见下文“长拼音组句”），没有时照旧用组句头，两种模型可以互换。
- 模型只看上文和候选词，不看拼音或已打的字母（“猜词”提示词，模型目录的 `rl_agent_config.json` 中
  `"zhiyi_prompt": "guess"`）。训练时中文上文是上一句加本句，
  英文是最后 192 个字符；中文候选是输入法实际给出的第一页（全拼、末音节不完整、简拼和混打）。
- 模型权重随 [Release model-zhen-unified-dec](https://github.com/dd1000001000/zhiyi_ime/releases/tag/model-zhen-unified-dec)
  发布（`scripts/fetch_model.py`）；训练代码与训练、评测数据涉及第三方语料版权，不公开。
- 上文：开始输入时，TSF 模块读取输入框里光标前（有选中文字时为选区之前）最多 256 个字（`SET_CONTEXT`），
  模型取其中最后 128 个字 / 192 个英文字符。程序不允许读取时，改用之前上屏的文字，换到另一个输入框时清空；
  密码框和标为私密的输入框不读取。实现见 `tsf/src/text_before_caret.cpp`。
- 推荐规则：只处理第一页；模型在前 2 × 每页候选数（`laya.max_candidates`）个候选中，从覆盖全部已输入拼音的候选里（字数不限，如 显示 /
  西安市）挑出一个，移到这些候选的第一个位置并加星标，其余候选（包括只对应部分拼音的，如 piaol 的 飘）保持原来的
  顺序。英文单词模式下，已打的原文固定在第一位，后面的补全和拼写纠正与拼音一样取前 2 × 每页候选数个（词库里的词
  够多时正好两页，可以翻页），模型在其中挑一个放到原文之后，纠正按改动量扣分。
  模型训练时候选顺序是打乱的，自己没有"第一个多半对"的位置偏好（原顺序 / 倒序 / 打乱准确率几乎相同）；
  引擎的排序可以通过排位先验加回来：
  得分 = log P(模型) + `laya.rank_prior_weight`（默认 0，英文 0.2）× log P(正确词排在第 r 位)，
  先验按训练数据统计。中文默认不加先验：加 0.4 时总体首选准确率高约 1.4 个点，但正确词排在后面时
  更容易被第一个候选压住；旧版本配置里保存的 0.4 读取时按 0 处理（只在配置格式 1.1 之前的文件上做一次）。模型词向量表只保留中英文用到的 8.2 万个 token（1.2 的模型文件 85 MB，加上组句头的统一模型 92 MB，1.5.4 的解码器模型 84 MB 加解码器 17 MB；1.1 的 mmBERT-base 模型为 194 MB）。
  长拼音组句（统一模型，`laya.continuation`，默认开）：从 `laya.continuation_min_syllables`（4）个音节起，模型读已打的音节、给每个音节上的每个字打分，
  引擎组句时每个词的分数 = ln P(模型给这几个字) + `laya.continuation_weight`（0.5）× 引擎的词分数；每种切分（如 xian / xi'an，最多 `laya.continuation_paths` 种）
  一次模型调用，简拼的各种读法共用一次。模型在后台算，按键从不等它（`server/src/continuation_worker.cc`）：
  - 按键时只按词频组句，照常由选词头（Laya）重排第一页；同时把这次缺的切分交给后台线程，每个会话只算最新的一份，
    停顿 `laya.continuation_delay_ms`（100 ms，CPU 和显卡都一样）后才开始算，连续打字时不做下一个键就作废的计算。
    算好的分数缓存在内存里，每代 8 MB、两代，删回原来的输入时不用再算。
  - 算完后，若这个会话的组合没变（上文、已选的部分和剩下的输入都相同，`Engine::continuation_key`），服务端发 `kRefreshCandidates`
    通知 TSF；TSF 用 `REFRESH_CANDIDATES` 来取时，若组合仍没变、还停在第一页且没有移动高亮，才重新出第一页并作为这次的回复。
    页面只在 TSF 来取时变：在这之前按下的空格等键先到服务端，上屏的就是屏幕上那一页，看到的和上屏的总是一致。重新出的第一页：覆盖全部输入的第一个候选是组出的句子时，模型组的第一句放在第一个并加星标，这一页不再由选词头重排（`laya.continuation_rerank` 默认关），
    领头的是词典词（如 眼科医院）时仍由选词头选（总放句子会低 4 个点）；重新组句超时或组不出时留着按键时那一页。模型组出的句子少于 5 个时，
    词频组的句子排在它后面，一句也组不出来时（如只有 3 个音节的那种切分才对）就用词频组的。继续打字、删字或先选了一部分（剩下的输入变了，已选的字成为上文）都会作废之前的结果，按新的组合重算。
  - 只对声明了 `kClientCapabilityCandidateRefresh` 的客户端这样做：更新后仍在运行的旧 TSF 收不到刷新，否则会出现看到的和上屏的不一致。
    没有后台线程时（laya_smoke、测试）模型在按键时同步打分；`LAYA_SMOKE_BACKGROUND=1` 模拟停顿后的结果，`=nopause` 是按键时的结果。
  端到端（首选，bench `eval_long_e2e.py`，822 条长句、1898 条词语 eng_test；旧选词模型 → 按键时 → 停顿后；小鹤双拼打同样的全拼长句也是 47.0% → 60.7%）：
  全拼 46.6% → 47.0% → 60.7%，末字只打声母 16.3% → 17.3% → 30.6%，全简拼 3.0% → 3.2% → 5.2%，前简后全 8.4% → 9.9% → 16.2%，
  词语 77.6% → 78.2% → 78.1%；两种都没有空页（按键时同步组句的版本有 13 页空的）。
  解码器模型（1.5.4）：同样从 `laya.continuation_min_syllables` 个音节起、在后台算。每种切分读一遍（`Reranker::encode`，缓存方式同上），然后按字做 beam 搜索（`pinyin_composer.h` 的 `sequential_compose`）：每条路径要么接着写当前词典词的下一个字，要么在词边界开始一个新词，每写一个字所有路径一起调用一次解码器（`laya.decoder_beam` 条，默认 16），一个词的分数 = 它每个字的 ln P 之和 + `laya.decoder_weight`（0.3）× 引擎的词分数，最后取总分最高的完整路径；简拼和末字未打完的输入（`compose_typed_spans`）也一样。CPU 上编码一次约 8 ms，每写一个字约 2 ms。端到端（停顿后，同上）：全拼 60.7% → 61.4%，末字只打声母 30.6% → 30.8%，全简拼 5.2% → 7.1%，前简后全 16.2% → 18.1%，词语 78.1% → 78.5%。
  长句（关掉 `laya.continuation` 或模型没有组句头时；4 个音节以上、没有词典词覆盖全部输入）：引擎组句时每段音节多留同音词（最多 8 个，不再限于最常见词的十分之一以内），
  给模型最多 8 个整句；模型是按“猜下一个词”训练的，单独挑整句不如引擎自己的顺序，所以得分 = log P(模型) +
  `laya.sentence_score_weight`（默认 1）× 引擎整句分数（各词 ln 频率之和，`Candidate::composed_score`）。
  第一页只显示排序后的前 5 个整句，其余排到后面，只对应部分拼音的词（如 这个输入法）仍留在第一页。
  实现见 `engine/src/laya_rerank.cc`，配置项见 [设置指南](settings-guide.md) 的 `laya.*`。

测试集上的首选准确率（int8 模型，CPU）：

| | 只按词频排序 | 知意输入法 |
|---|---|---|
| 中文，输入法实际候选（1898 条：全拼、末音节不完整、简拼、混打） | 63.0% | 77.6% |
| 其中正确词在候选里的（1646 条） | 72.3% | 89.2% |
| 中文同音词，上文只有本句（800 条） | 77.4% | 89.0% |
| 中文同音词，上文含上一句（同 800 条） | 77.4% | 90.9% |
| 英文单词补全（840 条） | 73.2% | 86.5% |

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
