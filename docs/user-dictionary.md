# 用户词库与候选偏好

## 概述

用户个性化数据拆分为多个独立资源，分别由 `UserLexicon`、`CandidatePreference` 与 `ManualCandidateOrder` 维护：

- **用户词库（user lexicon）**：手工添加的词条（文本、编码、音节），用于精确命中与个性化候选排序。持久化为 `user_pinyin.tsv` / `user_wubi.tsv`。
- **候选偏好（candidate preference）**：候选学习记录（选中的候选及其输入码），用于在翻译结果上做本地排序提升。持久化为 `learning_pinyin.tsv` / `learning_wubi.tsv`。
- **手动候选顺序（manual candidate order）**：用户为某个编码显式固定的候选顺序，优先级高于学习偏好。持久化为 `candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv`。

三者数据量都极小（几百到几千条，< 1MB），全部驻留内存，通过 TSV 文件持久化。服务端启动时加载，运行中按需合并保存，关闭时冻结并强制落盘。

这三层与随包默认排序、分页共同构成完整候选顺序，优先级与端到端链路见 [候选排序设计](candidate-ordering.md)。

## 用户词库：内存多路索引

用户词按 `code` 建立 `exact`、`prefix`、`abbr`、`mixed` 四路查询索引，另有 `text` 反查表和按 code 排序的二分数组，消除全表线性扫描。索引只保存 entry id（`uint32_t`），避免 `vector` 扩容导致指针失效。

### 数据结构

```cpp
// engine/include/cxxime/user_lexicon.h
struct Entry {
    std::string text;       // 候选文本，如 "输入法"
    std::string code;       // 原始输入码，如 "shurufa" 或 "srf"
    std::string syllables;  // 可选冒号形式，如 "shu:ru:fa"
    std::string abbr_code;  // 缩写，如 "srf"
    std::vector<std::string> mixed_keys;  // 缓存的 mixed keys，用于 bucket 重排
    int frequency = 1;
    uint64_t sequence = 0;  // 递增序号，用于调频排序
    bool deleted = false;   // 软删除标记
};
```

### 索引结构

| 索引 | 类型 | 用途 |
|------|------|------|
| `exact_index_` | `unordered_map<string, Bucket>` | 精确匹配（完整 code） |
| `prefix_index_` | `unordered_map<string, Bucket>` | 前缀匹配（长度 1..6） |
| `abbr_index_` | `unordered_map<string, Bucket>` | 缩写匹配（首字母组合） |
| `mixed_index_` | `unordered_map<string, Bucket>` | 混合匹配（声母增强简拼 / 首音节展开 / 前两音节展开 / 长词首字母码） |
| `text_index_` | `unordered_map<string, vector<EntryId>>` | 文本反查 |
| `entry_index_` | `unordered_map<string, EntryId>` | code+text 定位条目 |
| `code_sorted_` | `vector<EntryId>` | 按 code 字节序排序，用于长前缀二分查找 |

版本追踪：

```cpp
uint64_t version_ = 0;   // 每次更新递增，用于 cache 失效
uint64_t sequence_ = 0;  // 全局递增序号
```

### 索引 Key 生成

用户词插入或加载时生成以下 key：

| 索引 | 生成规则 | 示例 |
|------|---------|------|
| `exact` | 原始 code | `shurufa`、`srf` |
| `prefix` | exact 的长度 1..6 前缀 | `s`、`sh`、`shu` |
| `abbr` | 每个音节首字母（需 syllables） | `shu:ru:fa` → `srf` |
| `mixed` | 统一生成（需 syllables） | 见下表 |

Mixed code 码型（每词最多 8 个，去重后截断）：

| 码型 | 规则 | 示例 |
|------|------|------|
| 声母增强简拼 | zh/ch/sh 取双字母声母，其余取首字母 | `shu:ru:fa` → `shrf` |
| 长词首字母码 | 5+ 音节，每音节取首字母 | `zhong:hua:ren:min:gong:he:guo` → `zhrmghg` |
| 首音节展开 | 第一音节全拼 + 后续首字母 | `shu:ru:fa` → `shurf` |
| 前两音节展开 | 前两音节全拼 + 后续首字母（3+ 音节） | `bei:jing:da:xue` → `beijidx` |
| 首字母+第二音节 | 第一首字母 + 第二音节全拼 + 后续首字母（3+ 音节） | `bei:jing:da:xue` → `bjingdx` |
| 首音节前两字母 | 第一音节前两字母 + 后续首字母 | `shu:ru:fa` → `shrf` |

mixed bucket 裁剪到 `kMaxUserBucketSize = 64`，按 (frequency desc, sequence desc, id asc) 排序。

`prefix` 只保存长度 1..6 的短前缀，服务短输入快速路径。更长的前缀使用 `code_sorted_` 二分范围查询。

### 操作映射

| 操作 | 实现 | 复杂度 |
|------|------|--------|
| 精确匹配（`lookup_by_syllables`） | `exact_index_[code]` → 扫描 bucket | O(bucket_size) |
| 前缀匹配（`lookup`） | `prefix_index_[prefix]` 或 `code_sorted_` 二分 | O(bucket_size) 或 O(log n) |
| 缩写/混合匹配（短输入快速路径） | `abbr_index_` + `mixed_index_` | O(bucket_size) |
| 文本反查（`reverse_lookup`） | `text_index_.find(text)` | O(1) |
| 计数（`count`） | 索引 bucket 大小求和或二分范围计数 | O(1) 或 O(log n) |
| 添加（`add_user_entry`） | 查重 → 插入 entry → 维护索引；重复词条返回 false | O(1) + 索引维护 |

旧的 `update_frequency` 已移除：手工词条改由 `add_user_entry` 管理，学习记录移入候选偏好模块。

### 查询 API

```cpp
std::vector<Candidate> lookup_exact(const std::string& code, int limit,
    const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;
std::vector<Candidate> lookup_prefix(const std::string& prefix, int limit,
    const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;
std::vector<Candidate> lookup_indexed(const std::string& key, int limit,
    const QueryBudget& budget, QueryTrace* trace, UserLookupStats* stats) const;
```

公共集成：`lookup_by_syllables()` 用 `lookup_exact(concat_code)` 补充用户词；`lookup()` 用 `lookup_prefix(code_prefix)` 补充；短输入快速路径走 `lookup_indexed()`。

### 扫描预算

用户词查询遵守 `QueryBudget::max_user_scan`（默认 256）。达到上限时设置 `truncated=true`，不再扫描更多索引项。`user_scan_count` 只反映命中 bucket 或二分范围内的实际检查次数，不随用户词总量增长。

### 评分公式

用户词得分由 `UserLexicon` 内部的 `score_match()`（`engine/src/user_lexicon_query.cc`）计算，评分档位由 `UserLexicon::set_scoring_profile()` 在 `Dict` 初始化时设置：

```
score = base + bounded_frequency + recent_bonus

bounded_frequency = clamp(frequency, 1, 50000)
recent_bonus      = (delta <= 1000) ? 1000 - delta : 0，delta = sequence_ - entry.sequence
```

`base` 按匹配类型与评分档位（`UserScoringProfile::kPinyin` / `kWubi`）分层：

| 匹配情形 | base | 说明 |
|---------|------|------|
| 精确匹配（或 key 长度 == 码长） | `kExactBase = 200000000` | 全码词强力置顶 |
| 缩写 / 混合键匹配 | `kPatternBase = 120000000` | 简拼、首字母混合键命中 |
| 前缀匹配，key ≤ 2 字符 | `kWeakPrefixBase = 4000` | 短前缀不干扰系统词序 |
| 前缀匹配，key + 1 ≥ 码长 | `kNearPrefixBase = 800000` | 接近完整的前缀显著提升 |
| 前缀匹配，key × 2 ≥ 码长 | `kMidPrefixBase = 160000` | 中等长度前缀适度提升 |
| 其余前缀匹配 | `kWeakPrefixBase = 4000` | — |
| 五笔档（kWubi）前缀匹配 | `kWeakPrefixBase = 4000` | 五笔前缀一律弱加分，保护系统简码词序 |

### 更新流程

`load_user_dict`：在 `transaction_mutex_` 保护下读取 TSV（3/4 列），`parse_entries()` 解析并校验（合法 UTF-8、编码为 a-z、音节格式），构建 `Snapshot` 后重建全部索引并一次性发布，`version_++`。

`add_user_entry` / `delete_user_entries` / `replace_user_entry`：在 `transaction_mutex_` 保护下基于当前快照做变更（查重 / 软删除 / 替换），重建索引后发布并递增 version。另有 `add_user_entry_and_save` / `delete_user_entries_and_save` / `replace_user_entry_and_save` 变体，变更与落盘在同一事务内完成，写盘失败时内存状态保持不变。`UserLexicon` 侧的对应实现为 `add_entry` / `delete_entries` / `replace_entry` 及其 `_and_save` 变体。

`import_file`：从源文件导入用户词库（严格模式，任一行非法即整体失败），文件大小上限 64 MiB（`kMaxUserDictImportBytes`），成功后整体替换当前用户词库并落盘。

用户数据沿用与系统词典相同的纯符号判定：加载、导入以及手工新增/修改（用户词库、候选偏好、手动候选顺序、整句学习）都会过滤纯符号与 emoji，混合文本（如 `U盘`、`SDK`）保留。系统符号已在构建期从系统词典剥离（见 [词典系统设计](dictionary.md)），不会再经由用户数据回到候选中。

### 文件格式（用户词库）

TSV（Tab-Separated Values），支持 3 列（向后兼容）和 4 列格式：

```
text<TAB>code<TAB>frequency                   （3 列，旧格式）
text<TAB>code<TAB>frequency<TAB>syllables     （4 列，新格式）
```

文件：`%USERPROFILE%\zhiyi\user_pinyin.tsv`、`%USERPROFILE%\zhiyi\user_wubi.tsv`。

## 候选偏好

### 数据结构

```cpp
// engine/include/cxxime/candidate_preference.h
struct Entry {
    std::string text;           // 候选文本
    std::string code;           // 输入码（记录时的参数）
    std::string candidate_code; // 候选自身的编码
    std::string syllables;      // 冒号分隔音节
    int frequency = 1;          // 选中次数（commits）
    uint64_t sequence = 0;      // 最后一次选中时的全局计数（tick）
    double dee = 1.0;           // 衰减权重，截至 sequence 那一刻
    int score = 0;              // 最后一次选中时该词的排序分（0 = 未知）
    bool deleted = false;       // 软删除标记
};
```

按 `code + text` 作为条目键（`entry_index_`），另按 code 建立 `code_index_` 便于查询与清理；有音节的条目再按音节首字母
分桶（`initial_index_`）。同一个词、同一组音节只有一条记录：在 `xianzai` 下选过 现在 之后又在 `xz` 下选它，记到同一条上
（次数、权重合并；旧文件里按不同编码记的同一个词在加载时合并），和 Rime 用户词典按音节存一样。

### 记录与查询

- `record_candidate_preference(candidate, code, receipt)`：记录选中的候选。符号（`kSymbol`）与组合（`kComposed`）候选不记录；相同 text+code 次数加一、权重加一。冻结后拒绝记录。`receipt` 记下这次改动前的状态。
- `revoke_candidate_preference(receipt)`：撤回上一次记录（条目没被再次改动时）。上屏后紧接着按 Backspace 时引擎调用它：刚上屏就删，说明选错了，这次学习作废（Rime 的 `DiscardSession`）。
- `apply_candidate_preferences(code, source, candidates, limit)`：翻译结果排序前应用偏好，命中项标记 `learned = true`，不在结果里的条目补进来并标记 `origin = kLearned`，不产生重复项。
  命中的条目有两类：按当前输入精确记录的，以及（拼音）音节能被当前输入拼出来的——每个音节打全或只打声母（z / zh）、
  末尾的 儿 打成 r、最后一个音节可以没打完（`pinyin_spelling_match.h` 的 `match_typed_spelling`，不做更长的词的联想）。
  于是在 `xianzai` 下学过的 现在 对 `xz`、`xianz`、`xianza` 都有效；短输入的 Top-N 快速路径同样适用，因为匹配只扫首字母桶里的几十条，不依赖切分。
- `query_candidate_preferences` / `delete_candidate_preferences` / `clear_candidate_preferences`：管理接口，供设置页与 IPC 使用。

### 评分

参照 Rime 用户词典的做法：每次选中加一次，权重随后衰减，任何词每上屏一次是一个 tick，权重每 200 个 tick 衰减为 1/e（`algo::formula_d`）：

```
dee_now = dee × exp((sequence − 当前 tick) / 200)
```

- **选过一次（tentative）**：不置顶，只在它所在的排序组内加分，`kLearnedBoost = 1,500,000`，相当于词频 ×20，不越过组的上限；
  不在本次查询结果里的词按它上次的排序分 `score` 加分后补进来。`dee_now` 低于 0.35（约 210 个 tick 没再选）就遗忘，
  再选到时从头算一次。
- **选过两次以上（confirmed）**：置顶，`score = 210,000,000 + 10,000 + dee_now × 1000 + 拼写可信度 × 1000`，最近常选的在前，
  打全的排在只打声母的前面（每个声母 ln 0.5，没打完的音节 ln 0.05，与词典排序同一尺度）；`dee_now` 低于 0.05
  （两次选中后约 740 个 tick 没再用）时退回加分，不删除，再选一次又置顶。
- 通过声母或没打完的音节命中的加分词，加分基准是记住的排序分加上拼写可信度 × 500,000（词典排序的对数尺度）。
- 五笔没有可加分的词频尺度，选一次就置顶，其余规则相同。

### 持久化

- TSV 8 列：`text<TAB>code<TAB>candidate_code<TAB>frequency<TAB>sequence<TAB>syllables<TAB>dee<TAB>score`；
  读取时兼容 6 列（1.0.8 以前，权重按次数折算）和 7 列；保存时已遗忘的条目不再写出
- `save_if_due(delay)` 按最近更新时间合并落盘（服务端默认 1500ms）；`save()` 立即写盘
- `freeze()` 后拒绝记录/删除/清空，但允许保存既有数据
- 文件：`%USERPROFILE%\zhiyi\learning_pinyin.tsv`、`%USERPROFILE%\zhiyi\learning_wubi.tsv`

### 整句学习

`CompositionLearningService`（`engine/src/composition_learning.cc`）记录逐段选出来或由组句上屏的整句，按完整编码查询，
也按音节匹配当前输入（同 `match_typed_spelling`：`zhegsh` 命中学过的 这个事，分数减去声母的可信度），
规则与候选偏好一致：权重同样每 200 个 tick 衰减为 1/e；选过一次只排在组出来的句子之前（`kSentenceBase + kSentenceScoreSpan`，
仍在打全的整词之下），低于 0.35 遗忘；选过两次以上置顶（`220,000,000 + dee_now × 1000`），低于 0.05 退回句首位置。
`revoke(event)` 对应上屏后的 Backspace。
文件 `%USERPROFILE%\zhiyi\learning_composition.tsv`，列为 `text code syllables count sequence dee`，第 6 列不是数字时按旧格式读。

## 手动候选顺序（固定排序）

### 数据结构

```cpp
// shared/include/cxxime/user_dict.h
struct ManualCandidateOrderEntry {
    std::string text;       // 候选文本
    std::string code;       // 候选编码
    std::string syllables;  // 冒号分隔音节（拼音），五笔可空串
};
```

`ManualCandidateOrder`（`engine/src/manual_candidate_order.cc`）以输入编码为键保存固定项列表：

- 每编码最多 16 条固定项（`MANUAL_CANDIDATE_ORDER_MAX_ENTRIES = 16`），全局最多 100,000 条（`kMaxEntries`）；
- 加载/写入前校验：文本、编码、音节格式合法；编码长度不超过模式上限（拼音 64、五笔 4）；同一编码内不允许重复身份；
- `version()` 为序列化内容的哈希（`version_token(serialize(...))`），任何修改都会使版本变化，供设置页与 IPC 乐观并发控制。

### 语义

固定项按完整身份 `{text, code, syllables}` 匹配，在翻译结果中按用户指定顺序整体前置；未固定的候选保持原相对顺序。应用顺序为：默认排序 → 学习偏好 → 手动固定 → 分页，详见 [候选排序设计](candidate-ordering.md)。

### 持久化

- 文件头 `# cxxime-candidate-order format=1` + 每行 5 列 TSV：`input_code<TAB>text<TAB>candidate_code<TAB>syllables<TAB>position`（position 从 1 开始）；
- 写盘采用原子替换；写盘失败时内存状态保持不变；
- 文件：`%USERPROFILE%\zhiyi\candidate_order_pinyin.tsv`、`%USERPROFILE%\zhiyi\candidate_order_wubi.tsv`。

### 查询与修改接口

`Dict` 暴露以下 API（`engine/src/dict_user_data.cc`）：

```cpp
std::vector<ManualCandidateOrderEntry> manual_candidate_order(const std::string& code) const;
bool replace_manual_candidate_order_and_save(const std::string& code,
                                             const std::vector<ManualCandidateOrderEntry>& entries);
bool replace_manual_candidate_order_if_version(const std::string& code,
                                               const std::vector<ManualCandidateOrderEntry>& entries,
                                               std::uint64_t expected_version,
                                               bool* version_conflict);
bool has_manual_candidate_order(const std::string& input_code, const std::string& text,
                                const std::string& candidate_code,
                                const std::string& syllables) const;
std::uint64_t manual_candidate_order_version() const;
```

设置页通过 IPC 的 `kQueryCandidateOrder` / `kSetCandidateOrder` / `kClearCandidateOrder` 操作访问（`lexicon_control.h`），写入携带 `expected_version`；版本不一致返回 `ERROR_REVISION_MISMATCH`，界面提示刷新重试。设置界面操作方式见 [设置指南 — 词库管理](settings-guide.md)。

## Cache 失效

- **用户词库**：以 `UserLexicon::version()` 判定新鲜度。长拼音查询页缓存键包含该版本，版本不一致的条目不会命中，由 LRU 自然淘汰；Session recent cache 在版本变化后扫描时过滤已删除或已不存在的词条。
- **候选偏好**：偏好只影响候选排序，不进入查询索引；`apply_candidate_preferences` 在每次翻译时应用。其版本（`CandidatePreference::version()`）用于设置页等服务端查询的数据新鲜度判断。
- **手动候选顺序**：`ManualCandidateOrder::version()` 同样参与查询页缓存与 Translator 快照的失效判断；任何固定项变更都会使旧缓存失效。
- 系统短码缓存 `pinyin.topn.bin` 只含系统词，不受用户数据影响。

## 并发策略

- 用户词库：`shared_mutex`（查询持有 shared 锁）+ `transaction_mutex_`（加载/添加/删除/导入串行化）+ `save_mutex` 串行化落盘；删除使用 `deleted` 软标记，不从容器物理移除。
- 候选偏好：`shared_mutex` + `save_mutex`；`save_if_due` 按最后更新时间合并；`freeze` 后拒绝变更。
- 手动候选顺序：`shared_mutex`（读）+ `mutation_mutex_`（替换/落盘串行化）；原子替换写盘。
- 服务端以 `reload_mutex_` 串行化词典重载与会话生命周期（创建/销毁/清理），关闭流程先冻结偏好再强制保存，避免运行中操作与落盘相互冲突。

## 系统词隐藏

`DisabledSystemLexicon` 维护用户隐藏的系统词列表，持久化为 `disabled_pinyin.tsv` / `disabled_wubi.tsv`（每行一个词）。`disable_system_entry_and_save` / `restore_system_entry_and_save` 以事务方式写入，写盘失败时内存状态保持不变；`Dict::lookup` 在查询热路径统一过滤被隐藏的系统词。
