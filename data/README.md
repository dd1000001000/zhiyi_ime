# 词典数据与构建工具

`data/` 保存知意输入法的词典源数据、默认配置和词典构建工具。运行时二进制词典由这里的源数据生成，
不应手工修改。

## 数据分类

| 类型 | 文件 | 是否提交 | 说明 |
|------|------|---------|------|
| 词典源数据 | `pinyin.dict.db.zip` | 是 | 拼音 SQLite 词典的压缩分发副本 |
| 词典源数据 | `wubi86.dict.db.zip` | 是 | 未修改的五笔 86 词典源数据 |
| 英文词表 | `english.words.tsv`、`english/` | 是 | 英文单词与词频（`tools/build_english_dictionary.py` 由 `english/` 下的 rime-ice 词表和 wordfreq 生成） |
| 界面文字 | `ui.zh-CN.json`、`ui.en-US.json` | 是 | 设置程序的中英文界面文字 |
| 授权材料 | `licenses/rime-ice-GPL-3.0.txt` | 是 | 雾凇拼音 GPL-3.0-only 完整许可证 |
| 可审查派生数据 | `symbols.json` | 是 | 从五笔源词典拆出的独立符号分类 |
| 默认配置 | `default.json`、`themes.json` 等 | 是 | 安装包使用的出厂配置 |
| 临时源数据 | `*.dict.db` | 否 | 从压缩源解包或下载得到的 SQLite 文件 |
| 运行时数据 | `*.dict.bin`、`*.dict.idx`、`*.spellings.bin`、`*.topn.bin`、`*.reverse.idx` | 否 | 打包阶段生成的二进制文件 |

拼音运行时词典是精简版：`scripts/prepare_dictionary_bundle.py` 只保留全部单字和权重高于 rime-ice 默认值的词
（约 40 万条，源数据约 190 万条），生僻词可以逐字输入并由自学习记住。

`dictionary_manifest.json` 由打包流水线在所有运行时数据生成完毕后写入，记录文件角色、大小和
SHA-256；它同样不作为源文件维护。

## 数据授权

- 拼音词典派生自 [rime-ice](https://github.com/iDvel/rime-ice)，按 GPL-3.0-only
  发布。完整许可证保存在 `licenses/rime-ice-GPL-3.0.txt`，并随安装包分发。
- 五笔词典和 `symbols.json` 派生自
  [rime-wubi86-jidian](https://github.com/KyleBing/rime-wubi86-jidian)，按
  Apache-2.0 发布。
- 英文词表来自 rime-ice（GPL-3.0-only），词频来自 [wordfreq](https://github.com/rspeer/wordfreq)
  （数据 CC BY-SA 4.0，只在生成时使用）。
- 知意输入法整体按 GPL-3.0-only 发布，不替代上述第三方词典数据各自的许可证；
  Apache-2.0 全文见 `licenses/Apache-2.0.txt`。

## 本地工具入口

需要单独调试 SQLite 到运行时格式的转换时，可使用：

```bat
python data\tools\build_runtime_dictionary.py ^
    --input data\pinyin.dict.db --output data\pinyin
```

五笔完整前缀索引必须显式指定，且输入必须是已经由 `split_wubi_symbols.py` 过滤符号扩展项的
临时 SQLite 词典：

```bat
python data\tools\build_runtime_dictionary.py ^
    --input <filtered-wubi-db> --output data\wubi86 ^
    --dict-only --wubi-prefix-index ^
    --wubi-ranking-source data\pinyin.dict.db.zip ^
    --wubi-ranking-baseline data\tools\dict_builder\wubi_ranking_baseline.json
```

## 工具职责

`data/tools/` 中的文件名使用“词典领域 + 动作或产物”命名，避免无法判断用途的通用名称。

| 工具 | 职责 |
|------|------|
| `build_runtime_dictionary.py` | 将 SQLite 源词典转换为运行时词典和对应索引 |
| `fetch_pinyin_dictionary.py` | 下载并生成拼音 SQLite 源词典 |
| `fetch_wubi_dictionary.py` | 下载并生成五笔 SQLite 源词典 |
| `convert_rime_dictionary.py` | 将其他 RIME YAML 词典转换为 SQLite 格式 |
| `generate_pinyin_spellings.py` | 根据拼音 schema 生成 spellings 表 |
| `generate_pinyin_syllable_ids.py` | 为拼音 SQLite 词典生成音节 ID 分段字段 |
| `split_wubi_symbols.py` | 从五笔源词典生成符号表和过滤后的临时词典 |

`data/tools/dict_builder/` 是构建实现包，不是面向用户的命令集合：

| 模块 | 生成内容 |
|------|---------|
| `runtime_dictionary.py` | 通用 `.dict.bin` |
| `pinyin_spellings.py` | 拼音 `.spellings.bin` |
| `pinyin_syllable_index.py` | 拼音 `.dict.idx` |
| `wubi_prefix_index.py` | 五笔完整前缀候选 `.dict.idx` |
| `wubi_ranking.py` | 五笔可见候选的离线语料排序与全量验收 |
| `reverse_index.py` | Settings 词语反查使用的 `.reverse.idx` |
| `source_archive.py` | 安全解析 `.dict.db` 与 `.dict.db.zip` 输入 |

## 数据流水线

拼音：

```text
pinyin.dict.db.zip
  -> 临时 SQLite
  -> 拼写规则与四种内置双拼映射分别展开
  -> 共享 dict.bin + 五份 spellings.bin + syllable idx + reverse idx
```

五笔：

```text
wubi86.dict.db.zip
  -> 临时 SQLite
  -> 过滤后的临时 SQLite
  -> 读取 pinyin.dict.db.zip 的通用词频
  -> dict.bin + 完整前缀候选 idx + reverse idx
```

符号：
```text
symbol_catalog.json(独立收录清单)
  -> generate_symbols.py
  -> symbols.json(引擎使用)
```

二进制文件必须从源数据重建，不应手工编辑或提交。
`symbols.json` 作为可审查派生数据提交，
构建时重新生成并比对；修改清单后必须重新生成，不可只改输出文件。

五笔离线排序在各输入前缀的既有前 10 个候选内，对同一完整编码、同源词频的相邻纯符号与中文词进行稳定调整：
有正通用语料词频、且词频不低于符号的中文词可以越过符号，但不越过其他普通词。
一至三码保留原语料排序后的首选及精确短码候选顺序，仅调整补全项；四码可调整首选。
系统纯符号在排序前过滤，保留既有排序算法和版本；混合文本（如 `U盘`、`SD卡`）不按纯符号处理。
排序使用冻结的符号范围，避免 Python 的 Unicode 版本影响产物；全库变化由
`wubi_ranking_baseline.json` 的规则版本、排序指纹和独立符号调整计数验收。

## 系统词典过滤规则

- 普通系统词候选保留文字和混合文本；完整 emoji、全部由 Unicode 标点或符号
  （P/S 类）组成的文本统一过滤，不设置"常用符号保留普通编码"白名单。
  单独的数字、字母以及含标点的人名不属于纯符号；旧 co* 扩展项按明确编码过滤，
  保留同前缀普通词保护及新增扩展码审查。
- 分类依据固定 Unicode 17.0 数据，不依赖构建机 Python 的 Unicode 版本或字体。
- 完整序列覆盖肤色、ZWJ 和限定/未限定形式；仅兼容默认 emoji 单字符加 FE0F
  这一已审计别名。未知图形组合或畸形序列使构建失败，须先审核；孤立 P/S 组件按
  纯符号过滤。标准 Objects、Flags 等序列不要求已被符号清单收录，过滤不会自动扩库。
  未知纯图形串附带标点、单位、变体选择符、ZWJ 或 tag 仍需审核，不能因此视为混合文字。
  数字仅在组成 keycap 时按图形处理；含普通数字或文字的混合内容继续保留。
- 过滤在主词典、前缀/音节索引、Top-N 和反查索引生成前完成，不能只跳过一种索引。
  离线过滤只处理系统源的临时副本，不改源 ZIP。个人词、历史偏好、手动排序及组句学习
  在加载、导入时过滤纯符号与 emoji；手工新增或修改个人词、排序项时也执行检查。
  正常文字和混合文本保留；内部候选提交学习沿用已有来源与结构检查，不重复字符分类。
  不改 IPC 或文件格式，不在查询候选时扫描分类。符号模式提交不写入普通候选偏好。

### 分类数据的来源与再生成

三份 Unicode 17.0 官方原始输入随仓库保存在 `data/unicode/17.0/`，不依赖外部工作区。
来源、下载链接、体量及更新步骤见 [`unicode/README.md`](unicode/README.md)。
从项目根目录执行以下命令即可完全离线重生成分类表：

```bat
python data/tools/generate_emoji_classification.py
```

默认生成 `data/tools/dict_builder/emoji_classification.json`。输入、输出默认路径均以
脚本位置为基准，不依赖终端工作目录；`--source-dir` 可指定另一组待审核的原始输入，
`--output` 可指定临时产物。所有输入在生成前必须通过固定 SHA-256 校验，
缺少文件或内容不符会报错，且不会覆盖已有分类表；不自动下载、不猜测版本。
普通构建使用冻结 JSON；再生成测试直接使用仓库内的三份 TXT，均无需联网。
维护验证命令：

```bat
python test/data/unicode_classification_generator_test.py
python test/data/symbol_catalog_test.py
```

`emoji_classification.json` 是识别数据，不是供用户选择的 emoji 库。即使默认入口不收录
emoji，构建仍需识别完整序列，并让未知或畸形组合进入人工审核，而不是猜测删留。
它保留来源、版本、哈希和生成后的序列，维护动作是更新上游输入并重新生成，不能手改
其中某条数据。输入运行时不加载这份完整表，只使用生成的紧凑属性范围。

## 独立符号清单维护

`symbol_catalog.json` 是收录与导航的唯一来源。人工只维护分类和有中文用途说明的分组，
不再逐项保存 Unicode 英文名称。每组只允许两种形式之一：

```json
{"name":"带圈数字1-20", "range":["2460", "2473"]}
```

```json
{"name":"温度与角度", "codepoints":["2103", "2109", "00B0"]}
```

`range` 是升序闭区间，只用于已审核的连续系列；中间有空洞就拆成多组，不自动补齐
整个 Unicode 区块。`codepoints` 按显式顺序展开，每项也可用空格分隔完整序列的码点。
分类、组和组内列表顺序即最终候选顺序，分组名称只帮助维护，不改变运行时导航。
生成器拒绝同时指定两种规则、未知规则字段、非法范围、代理码点及展开后的类内重复。
不依赖字体识别字符，也不引入区块自动收录、继承或通用表达式。

默认 10 类、304 个类别项，围绕中文写作、办公、技术表达和汉语教学收录。
不提供 emoji、日文假名、西里尔字母、注音分类，不全量导入 Unicode 区块。
电脑符号不再独立分类，三个标准键盘符号并入"常用符号"：移除 F8FF、E76C 私用项。
"补充标点"避开已有中文标点按键映射；序号按系列连续排列；单位保留温度、角度、面积
和体积；货币优先标准形式，保留全角人民币在末尾。带调拼音去掉普通拉丁字母。
类内重复报错，跨类重复须有明确用途（当前仅数学与希腊字母中的 π）。

| 编码 | 分类 | 项数 |
|------|------|------|
| bd | 补充标点 | 14 |
| sz | 数字序号 | 74 |
| sx | 数学符号 | 49 |
| jt | 箭头 | 13 |
| xl | 希腊字母 | 48 |
| dw | 单位 | 5 |
| hb | 货币 | 6 |
| ts | 常用符号 | 28 |
| py | 拼音字母 | 26 |
| pp | 偏旁 | 41 |

单位由 `\dw`、数学由`\sx`、音符等由`\ts` 输入。
中文、中文标点、半角状态空闲时输入 `\` 可浏览分类。

维护时只在对应分组中审核范围或显式码点及顺序，再运行：

```bat
python data/tools/generate_symbols.py --catalog data/symbol_catalog.json ^
    --output data/symbols.json
```

词典过滤与符号收录是两项独立决策：后续词典新增纯符号会被过滤，但不会自动加入清单。
新增符号应按用途、可发现性和体量单独审核，不能以词典是否包含它作为收录依据。

运行时分类范围由同一冻结数据派生：

```bat
python data/tools/generate_symbol_ranges.py ^
    --source data/tools/dict_builder/emoji_classification.json ^
    --output shared/src/symbol_ranges.inc
```

范围为 Unicode P/S 加非数字 Emoji 属性；数字只有组成 keycap 时才按 emoji 处理。
ZWJ、变体选择符及 tag 等组件不使纯图形变成普通词；带文字的混合内容保留。
运行时只编译紧凑码点范围，不加载完整 Unicode 或 emoji 序列表。
历史文件加载时逐条忽略符号，不强制重写磁盘；正常保存、合并导入时写出过滤结果。
