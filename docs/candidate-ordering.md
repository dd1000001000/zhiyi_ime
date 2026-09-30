# 候选排序设计

## 概述

候选排序是 CxxIME 的核心体验之一。最终呈现给用户的候选顺序由四层按固定优先级叠加而成：

1. **用户手动固定顺序**（`ManualCandidateOrder`）——用户在设置界面为某个编码显式固定的候选顺序；
2. **自动选词偏好**（`CandidatePreference`）——启用候选学习后，由用户选词行为自动累积的偏好；
3. **随包默认排序**——词典自带的排序，由构建期评分/索引决定，用户无个性化数据时的顺序；
4. **分页与展示**——对上述有序列表按页切分、标注高亮项，不改变候选相对顺序。

**固定顺序 > 学习偏好 > 默认排序 > 分页展示**。上层只改变下层结果的相对顺序，不改变候选集合本身（手动固定可能补回被截断的词典候选，见「边界与交互」）。

本文档描述这四层的数据模型、应用时机与端到端链路；各层的详细机制分别在对应文档中展开：

- 默认排序（拼音）见 [候选词选词算法](candidate-selection.md)；
- 默认排序（五笔）见 [五笔候选质量排序](wubi-candidate-ranking.md)；
- 短输入快速路径与 Top-N 索引见 [短输入快速路径](short-input-fast-path.md)；
- 学习偏好与手动固定的数据结构、持久化见 [用户词库与候选偏好](user-dictionary.md)；
- 设置界面操作见 [设置指南 — 词库管理](settings-guide.md)。

## 1. 四层优先级

### 1.1 用户手动固定顺序（第 1 层）

用户可对某个输入编码固定最多 16 个候选（`MANUAL_CANDIDATE_ORDER_MAX_ENTRIES = 16`），顺序完全由用户指定。固定项按用户指定顺序插入候选列表头部；未固定的候选保持其原有相对顺序。

- 固定项以完整候选身份 `{text, code, syllables}` 标识，避免同文不同码、同码不同音的候选互相误伤。
- 固定项在被系统词隐藏过滤、词典重载后仍按身份解析；无法解析的固定项被跳过。
- 同一编码的所有固定项由 `ManualCandidateOrder` 统一管理，提供版本号供并发控制。

### 1.2 自动选词偏好（第 2 层）

启用候选学习（`engine.candidate_learning`）后，用户选中的候选会记录为偏好。翻译时命中项获得 `kPreferenceBaseScore = 210000000` 级别的加分（`+ min(frequency, 50000) + recency`），稳定排在普通系统词与用户词之前。

- 只影响排序，不进入查询索引，不写入用户词库。
- 符号（`kSymbol`）与组合（`kComposed`）候选不记录。
- 学习偏好由服务端统一保存，`CandidatePreference::version()` 用于设置页与查询缓存的新鲜度判断。

### 1.3 随包默认排序（第 3 层）

无个性化数据时，候选顺序来自构建期确定的排序：

- **拼音**：词典查询结果按频率降序合并；短输入走 Top-N 索引（`pinyin.topn.bin`，CXTOPN v3），其 score 由构建期按匹配类型加分（精确 > 缩写 > 混合，完整码 > 前缀）预先算好。
- **五笔**：`wubi86.dict.idx`（CXWIDX v1）在构建期按「精确匹配 → 码长升序 → 词频降序 → 码序 → 文本长度 → 文本字典序」预排序 postings，运行时直接读取，不做实时评分。
- 拼音普通管道与 Top-N 的排序规则详见 [候选词选词算法](candidate-selection.md) 与 [短输入快速路径](short-input-fast-path.md)；五笔默认排序的质量约束与审计见 [五笔候选质量排序](wubi-candidate-ranking.md)。

系统词典在构建期已剥离纯符号与 emoji（见 [词典系统设计](dictionary.md)），默认排序只处理文字与混合文本候选；符号候选统一由 `\` 符号输入提供，不参与这些排序层级。

### 1.4 分页与展示（第 4 层）

有序候选列表按 `page_size` 切分返回（`CandidatePage{page_index, page_offset, page_size, total_count, highlighted}`），高亮项固定为当前页第一个候选。分页不改变排序，只负责切片与展示。

## 2. 应用时机与代码路径

三层个性化排序在各 Translator 的查询结果合并后、分页之前依次应用：

```text
默认排序结果
  → apply_candidate_preferences()   （第 2 层，仅候选学习启用时）
  → sort_candidates_by_score()       （按最终分数降序）
  → apply_manual_candidate_order()   （第 1 层，固定项前置）
  → 分页
```

### 2.1 拼音（PinyinTranslator）

`pinyin_translator.cc`：

- `lookup_indexed_fast()`（短输入快速路径）与标准管道各自生成默认排序结果后，先 `apply_candidate_preferences()`，再 `sort_candidates_by_score()`，最后 `apply_manual_candidate_order()`。
- 长输入查询页缓存以 `QueryCacheVersions`（user_dict / candidate_preference / manual_candidate_order / disabled_system_entry 四个版本）作为失效条件，任一变化即失效。

### 2.2 五笔（WubiTranslator）

`wubi_translator.cc`：

- `lookup_candidates()` 从 `Dict::lookup()`（前缀索引预排序）取得结果后，按相同顺序应用偏好与手动固定，再按 text 去重并标记 `source = kWubi`。
- `translate()` 使用快照缓存（snapshot）避免重复查询，快照同样跟踪偏好/手动排序/禁用词版本。

### 2.3 混输（MixedTranslator）

`mixed_translator.cc`：

- 先分别取得拼音与五笔两个有序列表，按混输策略（`auto` / `wubi`）合并去重；
- 合并后收集所有手动固定项，按混输顺序整体前置（`manually_ordered()` 按完整身份判定），并从原列表中移除；
- 最后分页。

混输合并策略（`choose_order()`）：四字母纯字母输入且拼音首选不显著强于五笔时五笔优先；长度 ≤ 3 时交错合并；其余拼音优先。`engine.mixed_candidate_preference = wubi` 强制五笔优先交错。

## 3. 数据模型

### 3.1 ManualCandidateOrder（手动固定）

```cpp
// shared/include/cxxime/user_dict.h
struct ManualCandidateOrderEntry {
    std::string text;       // 候选文本
    std::string code;       // 候选编码
    std::string syllables;  // 冒号分隔音节（拼音），五笔可空串
};
```

- 以输入编码为键，每编码最多 16 条固定项，全局最多 100,000 条（`kMaxEntries`）。
- 加载/保存时校验：文本、编码、音节格式合法；编码长度不超过该模式上限（拼音 64、五笔 4）；同一编码内不允许重复身份。
- `version()` 为序列化内容的哈希（`version_token(serialize(...))`），任何修改都会使版本变化，用于设置页与 IPC 的并发控制。

### 3.2 CandidatePreference（学习偏好）

```cpp
// engine/include/cxxime/candidate_preference.h
struct Entry {
    std::string text;           // 候选文本
    std::string code;           // 输入码（记录时的参数）
    std::string candidate_code; // 候选自身的编码
    std::string syllables;      // 冒号分隔音节
    int frequency = 1;
    uint64_t sequence = 0;
    bool deleted = false;
};
```

按 `code + text` 作为条目键；命中后评分：

```text
score = kPreferenceBaseScore(210000000) + min(frequency, 50000) + recency
recency = (当前序号 - 条目序号 <= 1000) ? 1000 - 差值 : 0
```

### 3.3 候选身份

手动固定与学习偏好都使用完整候选身份 `{text, code, syllables}` 而非仅文本，原因：

- 同一文本可能对应多个编码（拼音 `srf` / `shurufa`，五笔重码），固定其一不应影响另一个；
- 拼音下同一文本可能存在不同音节切分，Top-N v3 保存规范音节正是为了提供一致的候选身份；
- 设置界面按身份展示排序原因，避免"同文词"互相覆盖。

## 4. 持久化文件

| 文件 | 内容 |
|------|------|
| `candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv` | 手动固定顺序（第 1 层） |
| `learning_pinyin.tsv` / `learning_wubi.tsv` | 学习偏好（第 2 层） |
| `user_pinyin.tsv` / `user_wubi.tsv` | 用户词库（影响候选集合，不改变系统词排序） |
| `disabled_pinyin.tsv` / `disabled_wubi.tsv` | 系统词隐藏列表（过滤，不改变排序） |
| `pinyin.topn.bin` / `wubi86.dict.idx` | 随包默认排序（第 3 层，构建期生成） |

手动固定文件格式（`# cxxime-candidate-order format=1` 头 + 每行 5 列 TSV）：

```text
# cxxime-candidate-order format=1
input_code<TAB>text<TAB>candidate_code<TAB>syllables<TAB>position
```

`position` 从 1 开始，表示该编码下固定顺序中的位置。文件位于 `%USERPROFILE%\cxxime\`。

## 5. 设置界面（词库管理 → 候选排序）

设置窗口「词库管理」面板提供三个视图：**词条 / 候选排序 / 选词偏好**。「候选排序」视图的工作流：

1. 选择拼音或五笔，输入候选文本，从候选编码列表中选择目标编码；
2. 列表展示该编码下全部候选及其**排序原因**（`CandidateOrderReason`）：
   - `kDefault` — 随包默认排序；
   - `kUserLexicon` — 用户词库命中；
   - `kLearned` — 学习偏好命中；
   - `kManual` — 手动固定项。
3. 对单个候选可执行：
   - **固定置顶**：插入固定列表首位；
   - **追加固定**：追加到固定列表末尾；
   - **上移 / 下移**：调整固定项相对顺序；
   - **移除固定**：从固定列表删除；
   - **恢复默认顺序**：清空该编码的全部固定项（二次确认）。

固定项上限 16；保存时携带读取到的版本号，若服务端版本已变化则返回 `ERROR_REVISION_MISMATCH`，界面提示「候选顺序已在其他位置更新，请刷新后重试」并自动刷新。

## 6. IPC 协议

`lexicon_control.h` 为候选排序提供三个操作（`LexiconOperation`）：

| 操作 | 语义 |
|------|------|
| `kQueryCandidateOrder` | 查询某编码的候选列表 + 当前固定项 + 版本号 |
| `kSetCandidateOrder` | 写入固定项（携带 `expected_version`，乐观并发控制） |
| `kClearCandidateOrder` | 清空某编码的固定项（携带 `expected_version`） |

查询结果 `CandidateOrderQueryResult`：

```cpp
struct CandidateOrderQueryResult {
    std::string input_code;
    std::uint64_t version = 0;
    bool has_more = false;
    std::vector<CandidateOrderEntryInfo> entries;      // 含 reason / available
    std::vector<ManualCandidateOrderEntry> manual_entries; // 当前固定项
};
```

`available` 表示该候选是否仍可从词典解析（词典更新后旧候选可能失效）。

## 7. 边界与交互

- **系统词隐藏**：手动固定解析出的候选若在隐藏列表且非用户词，会被过滤，即使被固定。
- **数量截断**：固定项前置后超出 `limit` 的部分被截断，不改变未固定候选的相对顺序。
- **混输去重**：合并按文本去重，固定项优先保留混输策略偏好的来源（五笔优先时保留五笔身份）。
- **词典更新**：固定项按身份解析，词典重载后同身份候选仍可被固定；无法解析的固定项不产生候选。
- **格式演进**：Top-N v3（CXTOPN v3）为 0.4 磁盘基线，候选身份字段（text + syllables）禁止重排，未来只追加字段。

## 8. 相关文档与测试

- [候选词选词算法](candidate-selection.md) — 第 3 层（拼音默认排序）
- [五笔候选质量排序](wubi-candidate-ranking.md) — 第 3 层（五笔默认排序）
- [短输入快速路径](short-input-fast-path.md) — Top-N 索引与快速路径
- [用户词库与候选偏好](user-dictionary.md) — 第 1、2 层数据结构与持久化
- [设置指南](settings-guide.md) — 设置界面操作

覆盖本功能的测试：

- `disabled_system_lexicon_test`：固定/偏好与系统词隐藏的交互；
- `dictionary_format_test`、`short_cache_test`：Top-N v3 身份与候选写入；
- `user_data_separation_test`：手动固定与偏好的事务、版本与失效语义；
- `session_manager_status_test` / `session_manager_integration_test`：服务端加载与 IPC；
- `wubi_engine_test`：五笔混合排序与固定/偏好；
- `dictionary_symbol_policy_test` / `symbol_catalog_test` / `ordinary_candidate_test`：系统符号剥离与用户数据的纯符号过滤；
- `pinyin_topn_pipeline_test` / `wubi_prefix_index_test`：默认排序层构建验证。
