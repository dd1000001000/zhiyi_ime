# 候选词选词算法

## 概述

本文档描述**随包默认排序**层（[候选排序设计](candidate-ordering.md) 的第 3 层）的选词算法：给定输入，如何从词典中找到候选并排出默认顺序。两种模式各自独立：

- **拼音**：拼写图 + 路径枚举 + ID 索引查找 + 频率排序；短输入另有 Top-N 快速路径（见 [短输入快速路径](short-input-fast-path.md)）。
- **五笔**：完整前缀索引（`wubi86.dict.idx`）二分定位 + 直接读取构建期预排序 postings，不做实时评分。

默认排序之后，翻译结果还会依次应用学习偏好与手动固定（第 2、1 层），最后分页（第 4 层）；本层只负责产出"无个性化数据时的顺序"。

## 拼音选词管线

```
用户输入 → TSF → IPC → Engine::process_key → PinyinProcessor → PinyinTranslator → 候选词列表
```

`PinyinTranslator::translate()` 是核心，分四步：

### 1. 拼写图构建（Syllabifier）

将输入字符串（如 `"sdf"`）构建为拼写图。图中每个节点是输入位置（0, 1, 2, 3），边是输入片段→完整拼音音节的映射。

```
输入: s d f
      │ │ │
      ▼ ▼ ▼
     s→shui  (缩写, 消耗1字符, 信度-0.693)
     s→sa    (缩写, 消耗1字符, 信度-1.2)
     ...
     d→dian  (缩写, 消耗1字符)
     d→da    (缩写, 消耗1字符)
     ...
     f→fei   (缩写, 消耗1字符)
     f→fa    (缩写, 消耗1字符)
```

每个位置的边来自 `SpellingsIndex::prefix_search()`（Patricia trie 前缀搜索）。三种拼写类型：

| 类型 | 示例 | 说明 |
|------|------|------|
| 正常拼写 | `"shu"→"shu"` | 输入=音节，完全匹配 |
| 模糊拼写 | `"zong"→"zhong"` | 模糊音纠正（z/zh 不分） |
| 缩写 | `"s"→"shui"` | 声母缩写，消耗 1 字符 |

### 2. 路径枚举（enumerate_paths）

从图中枚举从位置 0 到最远可达位置的所有音节路径。

```
图:   0──s→1──d→2──f→3
       ├─sha→1
       ├─shai→1
       └─...×35

路径: ["shui","dian","fei"]
      ["sa","da","fa"]
      ["shi","da","feng"]
      ...
```

**关键优化：**

- **信度排序**：DFS 前将每条边的候选音节按信度降序排列。正常拼写（信度 0）优先，缩写中信度越高（越常见）越优先。这确保好路径在截断前被枚举到。
- **枚举上限**：`Syllabifier` 最多枚举 256 条路径（`engine/src/syllabifier.cc` 的 `kMaxPaths = 256`）。超过则 DFS 提前退出。`PinyinTranslator` 只消费前 64 条（`pinyin_translator.cc` 的 `kMaxPaths = 64`）。密集缩写图（如 11 字符全拼产生 154 条边）在无上限时可生成 10,000+ 条路径，降至 256 后同一输入 <1ms 完成。
- **提前退出**：DFS 每层检查 `results.size() >= kMaxPaths`，达成即返回。避免穷举 5^N 条路径后才发现超限。

### 3. 路径过滤（has_prefix）

将每条路径的音节转为整数 ID 序列，查询 ID 索引（`id_index_`）：

```
["shui","dian","fei"] → [id(shui), id(dian), id(fei)]
```

二分查找 + 前缀匹配。只有词典中存在对应 N 元音节记录的路径保留（`QueryScratch::live_path_indices`）。不存在的路径被丢弃（如 `["sa","da","fa"]` ——不存在这个三字词）。

### 4. 候选查找与排序（lookup_by_ids + sort）

对保留的每条路径，在二进制词典中查找所有匹配词条：

```
id_index_ 二分查找: "shui:dian:fei" → entries[117200..117500]
  扫出: 水电费(freq=117200), 税费点(freq=85000), ...
id_index_ 前缀匹配: "shui:dian" → entries[...]
  扫出: 水电(freq=150000), 水点(freq=90000), ...
```

同时查询用户词库索引（`lookup_user_exact` / `lookup_user_prefix`），补充个性化候选。用户词候选按 `score_match()` 分层评分（精确/缩写高基数，前缀按接近程度分档，详见 [用户词库与候选偏好](user-dictionary.md)），插入到结果中。

每个候选按 Rime 的方式打分（`rank_fallback_candidate`，与 Top-N 索引同一公式，见
[短输入快速路径](short-input-fast-path.md#排序规则)）：先分组——音节全部对上（完整）> 最后一个音节靠补全、
或输入能切成完整音节却走了简拼（补全）> 词比输入长（更长的词）；组内按 `ln(词频) + 路径可信度`
（每个简拼或补全音节 ln 0.5，模糊音再加 ln 0.2）。所有路径的结果合并去重，按分数降序排列
（`sort_candidates_by_score`：score desc → 文本长度 asc → 文本字典序），再进入偏好/手动固定层，最后分页返回。

### 组句（词典里没有整词时）

整词候选不足一页时，`PinyinComposer` 在完整音节路径上把多个词拼成整句，做法同 Rime 的 Poet（不带语法模型）：

- 句子分数 = Σ（ln(词频 / 1e8) + ln(1e-6)），每多一个词扣 ln(1e-6)，所以词越少、越常用越好；
- 每段音节取最多 3 个同音词，只取词频不低于该段最高者 10% 的（改/该 保留，瓦/哇 不要）；
- 歧义切点（两边合起来也是一个音节，如 `xia` 的 xi|a、`xian` 的 xi|an）不能做词的边界，跨过它的词
  （西安）扣 ln 0.1：`nikanxia` 不会出现"你看洗啊"；
- 只有一个词的"句子"不输出（词典候选已有）；
- 按分数输出至多 5 句：先取与上一句相差不超过 10% 的（相差阈值随句数收紧，同 Rime 的 MakeSentences），
  再给其他音节切法各留最好的一句（与最好的一句相差不超过 45%，如 `nikanxiane` 的 你看限额 / 你看下呢）。
  模型在这些整句里挑首选。
- 每段的同音词按词频从高到低存放，取够后即停止扫描，13 个音节以上的长句不会因扫描预算
  （`max_entry_scans = 2048`）用完而缺最后几段；ü 打成 v 的拼写（jv、lve）也参与组句；
- 最后一个音节还没打完（`…yizhengj`）时没有整句候选：第一个候选是已打完部分的整句（`…yizheng` 的组句），
  选中后剩下的字母接着打（`translate()` 的 leading sentence）。

### 完整示例：输入 `"sdf"`

```
1. 拼写图: s(35候选)×d(~20候选)×f(~10候选)
2. 枚举: 8050 条路径 (按信度排序)
   5634: shui:dian:fei  (信度之和最高的一批排在前面)
3. 过滤: has_prefix 筛掉不存在的三字词
4. 查找+排序:
   [0] 士大夫  124450  (来自 shi:da:fu)
   [1] 斯蒂芬  117510  (来自 si:di:fen)
   [2] 史蒂夫  117465  (来自 shi:di:fu)
   [3] 水电费  117200  (来自 shui:dian:fei)
   ...
```

## 五笔选词管线

五笔编码固定 1–4 码，无歧义切分，因此不走拼写图/路径枚举，直接以码查询：

### 1. 前缀索引定位

`Dict::lookup(code)` 对 `wubi86.dict.idx`（CXWIDX v1）的 `packed_code` 做二分查找（O(log P)），定位到该编码前缀的 posting 区间。

### 2. 预排序 postings 读取

postings 是 `dict.bin` 词条索引数组，构建期已按 **精确匹配 → 码长升序 → 词频降序 → 码序 → 文本长度 → 文本字典序** 排序并按文本去重。运行时线性读取（O(K)）即可得到默认顺序，不做实时评分。

### 3. 排序规则摘要

五笔默认排序由 `wubi_ranking.py` 构建期审计约束（如三码补全提升预算、前 10 候选集合不变、频率不倒退等），完整规则见 [五笔候选质量排序](wubi-candidate-ranking.md)。

### 示例：`qdr`

```
Dict::lookup("qdr") → WubiPrefixIndex::find(packed_code(qdr))
  预排序 postings: [然后(freq高), 厃(freq低, 罕见精确字), ...]
```

构建期审计把"然后"提升为首选（覆盖 `qdr → 然后` 回归用例）。

查询结果随后依次应用学习偏好与手动固定（见 [候选排序设计](candidate-ordering.md)），按 text 去重并标记 `source = kWubi`。

## 用户词典查询

用户词典在查询管道中通过多路索引（exact / prefix / abbr / mixed）查询，与系统词典结果合并（拼音与五笔共用同一套用户词机制）：

| 查询方法 | 触发条件 | 使用的索引 |
|----------|---------|------|
| `UserLexicon::lookup_exact()` | `lookup_by_syllables()` 精确匹配 | `exact_index_` |
| `UserLexicon::lookup_prefix()` | `lookup()` 前缀匹配 | `prefix_index_`（前缀长度 ≤ 6）或 `code_sorted` 二分（前缀更长） |
| `UserLexicon::lookup_indexed()` | 短输入快速路径 | exact → prefix → abbr → mixed 四路桶 |

### lookup_prefix 两路查询

`UserLexicon::lookup_prefix()` 根据 key 长度走不同的索引路径，两路都在收集后按 `score_match()` 评分降序排列，再根据 `max_user_scan` 截断：

- **key 长度 ≤ 6**：通过 `prefix_index_` 哈希桶定位，桶内遍历全部命中条目，评分排序后截断
- **key 长度 > 6**：对 `code_sorted`（按 code 字典序排序的 `EntryId` 数组）做 `lower_bound` 二分定位起始位置，线性扫描前缀匹配条目，评分排序后截断

### 用户词评分

用户词评分通过 `UserLexicon` 内部的 `score_match()` 函数（`engine/src/user_lexicon_query.cc`）计算，按匹配类型和打分档位分档：

```cpp
// 基础分（按匹配类型）
kExactBase     = 200000000   // 完全匹配（key == code）
kPatternBase   = 120000000   // 缩写/混合匹配
kNearPrefixBase = 800000     // 前缀：key_len + 1 >= code_len
kMidPrefixBase  = 160000     // 前缀：key_len * 2 >= code_len
kWeakPrefixBase = 4000       // 前缀：其余情况，含 key_len <= 2

// 频率加成（bounded）
bounded_frequency = clamp(frequency, 1, 50000)

// 近期使用加成
recent_bonus = (current_sequence - entry_sequence <= 1000)
             ? 1000 - delta
             : 0

// 最终分
score = base + bounded_frequency + recent_bonus
```

打分分两档：

| 档位 | 条件 | 行为 |
|------|------|------|
| `kPinyin` | 拼音模式（默认） | 完整分档：exact → pattern → nearPrefix → midPrefix → weakPrefix |
| `kWubi` | 五笔模式 | 前缀匹配一律 `kWeakPrefixBase`（4000），避免用户词干扰五笔码长精确匹配 |

档位由 `Dict` 在初始化时通过 `UserLexicon::set_scoring_profile()` 设置：拼音词典用 `kPinyin`，五笔词典用 `kWubi`（`engine/src/dict.cc`）。

用户词候选通过 `candidate.origin = kUser` 标识，评分远高于系统词典前缀匹配（系统前缀无加分），保证用户选过的词优先于系统未选过的同码词。

### 扫描预算

用户词查询遵守 `QueryBudget::max_user_scan`（默认 256）。`user_scan_count` 只反映命中 bucket 或二分范围内的实际检查次数，不随用户词总量增长。

## 默认排序规则汇总

### 拼音

| 来源 | 排序依据 |
|------|---------|
| 普通管道 | 完整 > 补全 > 更长的词，组内 ln(词频) + 拼写可信度（同 Top-N） |
| Top-N 快速路径 | 构建期 score，同上（见 [短输入快速路径](short-input-fast-path.md)） |
| 组句 | 词典里没有整词时补在后面，句子分数同 Rime 的 Poet（见上） |
| 用户词 | `score_match()` 分档（见上），远高于系统前缀匹配 |

### 五笔

| 来源 | 排序依据 |
|------|---------|
| 前缀索引 | 构建期预排序 postings（精确 → 码长 → 词频 → 码序 → 文本长度 → 文本字典序） |
| 用户词 | 五笔档弱加分（`kWeakPrefixBase = 4000`），保护系统简码词序 |

## 排序后的个性化层

默认排序结果合并后，按 [候选排序设计](candidate-ordering.md) 的优先级依次处理：

1. `apply_candidate_preferences()`（第 2 层，学习偏好；仅候选学习启用时）——命中项按 `kPreferenceBaseScore = 210000000` 加分并标记 `origin = kLearned`；
2. 重新按最终分数排序；
3. `apply_manual_candidate_order()`（第 1 层，手动固定）——固定项按用户指定顺序前置，未固定候选保持相对顺序；
4. 分页返回（第 4 层）。

学习偏好独立于用户词库，清空偏好即恢复默认排序；手动固定可逐编码恢复默认。

## Candidate 结构

```cpp
// shared/include/cxxime/candidate.h
struct Candidate {
    std::string text;                          // 候选词文本
    std::string comment;                       // 注释（五笔编码、拼音等）
    int frequency = 0;                         // 评分/频率（降序排列依据）
    CandidateSource source = kPinyin;           // 词典来源：kPinyin / kWubi
    std::string code;                          // 码表编码（如 "shurufa"）
    std::string syllables;                     // 音节序列（如 "shu:ru:fa"）
    CandidateOrigin origin = kSystem;           // 候选来源：kSystem / kUser / kLearned / kCache / kComposed
};
```

| 字段 | 说明 |
|------|------|
| `code` | 紧凑码表编码，系统词来自 `syllable_ids → compact_syllable_code()`，用户词存 `user_entry.code` |
| `syllables` | 冒号分隔的音节字符串（如 `"shu:ru:fa"`），系统词从二进制词典字符串表读取，用户词存 `user_entry.syllables` |
| `origin` | `kSystem`（系统词典）、`kUser`（用户词库）、`kLearned`（候选偏好命中）、`kCache`（查询页缓存命中）、`kComposed`（组合候选） |
| `source` | `kPinyin`（拼音模式查询）、`kWubi`（五笔模式查询） |

`text + code + syllables` 构成**完整候选身份**，手动固定与学习偏好均按身份匹配（见 [候选排序设计](candidate-ordering.md)）。`origin` 和 `source` 还用于日志追踪、设置界面的排序原因展示。

## 查询页缓存

与短输入快速路径并行的缓存层，按「输入 + 页码 + 页大小」保存已计算出的候选页，`translate_page()` 开头先查此缓存，未命中才进入快速路径或标准管道。

标准管道返回页面前将结果存入 LRU 缓存，`candidate_offset` 也参与缓存键与命中判断：

```cpp
// pinyin_translator.cc — 返回前
if (!deadline_hit && !(trace && trace->deadline_exceeded))
    store_query_cache(pinyin, page_index, candidate_offset, page_size, versions, page);
```

| 属性 | 值 |
|------|-----|
| 触发条件 | 任意输入长度，只要输入与页参数命中已缓存条目 |
| 容量 | LRU 64 条（`kMaxQueryCacheEntries = 64`），`sequence` 递增序号实现淘汰 |
| 命中条件 | input + page_index + candidate_offset + page_size + 用户词/偏好/手动排序/禁用词/整句学习版本全匹配 |
| 失效 | 任一相关版本变化后全部缓存自动失效 |
| 非缓存场景 | deadline 命中或 deadline_exceeded 时不写入缓存，避免缓存过期/不完整结果 |

`lookup_query_cache()` 命中时设置 `trace->cache_hit = true` 并清零 trace 计数字段（exact/prefix/user_scan = 0），语义与短输入快速路径的 cache hit 一致。

## 性能特征

### 拼音

| 输入长度 | 路径数 | 延迟 | 说明 |
|---------|--------|------|------|
| 1 字符 | ~10 | < 1ms | 单个音节候选少 |
| 2 字符 | ~200 | < 1ms | 正常输入 |
| 3 字符（缩写） | 封顶 256 | < 1ms | 如 sdf，每位置 10~35 候选，DFS 上限截断 |
| 4 字符（全拼） | 封顶 256 | < 1ms | 如 shui，多数位置只有精确匹配 |
| 11 字符（全拼） | 封顶 256 | < 1ms | 如 nihaoshijie，154 条边密集图，DFS 上限截断 |

### 五笔

| 输入 | 路径 | 延迟 | 说明 |
|------|------|------|------|
| 1–4 码 | 前缀索引二分 + 读 postings | < 0.1ms | O(log P + K)，无歧义切分，无实时评分 |

## Pipeline 架构

### 拼音

```
Syllabifier(拼写图) → Segmentor(切分) → Translator(查词+排序)
```

- **Syllabifier**：BFS 构建音节图，基于 Patricia trie 前缀搜索，DFS 枚举全路径（信度排序+上限截断）
- **Segmentor**：内置完整拼音音节表，贪婪最长匹配
- **Translator**：基于音节 ID 的词典二分查找 + 用户词索引查询 + 频率排序 + 偏好/手动固定
- Filter/Formatter 层未实现（目前不需要繁简转换、注释等）

### 五笔

```
WubiTranslator → Dict::lookup → WubiPrefixIndex::find(packed_code 二分)
               → 预排序 postings → 偏好/手动固定 → 去重 → 快照缓存 → 分页
```

## 查询预算

查询管道支持 deadline 超时和扫描预算截断，通过 `QueryBudget` 结构传递。详见 [查询预算与候选收集](query-control.md)（包括 deadline 检查点分布、`make_budget()` 分档、TopKCollector 等）。
