# 短输入快速路径

## 概述

短输入（拼音为全小写字母，五笔为 1–4 码）是生产输入法中最常见的场景。标准拼音管道需要 Syllabifier 路径枚举 + 多次 dict scan，延迟在毫秒级；两个模式各有专属的快速路径，把常用查询降到微秒级：

- **拼音**：在 Syllabifier 之前插入两层内存查询（用户词多路索引 + Top-N 索引），命中时跳过路径枚举和词典扫描。
- **五笔**：无独立 Top-N 文件，快速路径由构建期预排序的完整前缀索引（`wubi86.dict.idx`）承担——运行时二分定位后直接读取排序好的 postings，不实时评分、不扫描全表；查询结果在 `WubiTranslator` 内以快照缓存复用。

快速路径只改变**获取与合并**方式，不改变 [候选排序设计](candidate-ordering.md) 的四层优先级：默认排序 → 学习偏好 → 手动固定 → 分页。

## 架构

### 拼音

```
Engine::process_key()
  │
  ▼
PinyinTranslator::translate()
  │
  ├─ is_indexable_key(pinyin)?  ──否──→ 标准管道 (Syllabifier + Dict)
  │       │
  │      是
  │       ▼
  │  lookup_indexed_fast(key, limit)
  │       │
  │       ├─ 1. User Dict Short Index (内存多路索引)
  │       │     用户词 exact/prefix/abbr/mixed 索引查询
  │       │
  │       └─ 2. ShortCodeCache (pinyin.topn.bin, CXTOPN v4 共享候选索引)
  │             Darts-clone 双数组 Trie 查找, O(k)
  │       │
  │       ▼
  │  complete_index_hit? ──是──→ 返回缓存页 (cache_hit=true, scan=0)
  │       │
  │      否 (回退)
  │       ▼
  └─ 标准管道 (Syllabifier + Dict), 用缓存候选 seed TopKCollector
```

### 五笔

```
WubiTranslator::translate(code)
  │
  ├─ Dict::lookup(code) ── WubiPrefixIndex::find()   (CXWIDX v1, O(log P + K))
  │     直接读取构建期预排序 postings, 无实时评分
  │
  ├─ apply_candidate_preferences()      (候选学习启用时)
  ├─ apply_manual_candidate_order()     (手动固定前置)
  ├─ 按 text 去重, 标记 source = kWubi
  │
  ├─ WubiTranslator snapshot 查询缓存   (按需加倍 limit 扩大快照)
  │     code / user_dict / preference / manual_order / disabled 版本变化时重建
  │
  └─ 分页返回
```

## 拼音 Short Code Cache

### 二进制格式 (`pinyin.topn.bin`, CXTOPN v4 共享候选索引)

与 `pinyin.dict.bin` 放在同一目录，文件整体读入堆内存。运行时格式为 **CXTOPN v4 共享候选索引**（magic `CXTOPN\x04\x00`，version=4，header 64 字节）：posting 只保存**词典词条索引**与构建期 score（8 字节/条），候选文本、规范音节与词频全部复用 `pinyin.dict.bin` 的词条，不再内联字符串池。索引与词典通过 `dictionary_entry_count` + `dictionary_fingerprint`（FNV-1a）绑定，不匹配即拒绝加载。键查找基于 **Darts-clone** 双数组 Trie（O(k)）。

```
┌────────────────────────────────────────────┐
│ ShortCacheHeader (64 bytes)                │
│   magic[8]      = "CXTOPN\x04\0"           │
│   version       = 4                        │
│   header_size   = 64                       │
│   file_size                                │
│   key_count                                │
│   code_index_count    → Darts units[]      │
│   posting_list_count  → ShortPostingList[] │
│   posting_count       → postings[]         │
│   dictionary_entry_count = dict 词条数     │
│   code_index_offset      → Darts units     │
│   posting_lists_offset   → posting_lists[] │
│   postings_offset        → postings[]      │
│   dictionary_fingerprint ← 词典指纹        │
│   reserved                                 │
├────────────────────────────────────────────┤
│ Darts units[code_index_count] (uint32)     │ ← Darts-clone Double Array Trie
│   键索引，支持 O(k) exactMatchSearch       │
├────────────────────────────────────────────┤
│ ShortPostingList[posting_list_count]       │ ← 4 bytes each
│   posting_offset_and_flags (uint32)        │
│     低 31 位 posting_offset                │
│     最高位 kShortPostingPrefixComplete     │
├────────────────────────────────────────────┤
│ ShortCandidatePosting[posting_count]       │ ← 8 bytes each
│   dictionary_entry_index (uint32)          │ → pinyin.dict.bin 词条索引
│   score                  (int32)           │ ← 构建期排序分（frequency 取自词条）
└────────────────────────────────────────────┘
```

> 格式约束：v4 是当前磁盘基线，header 字段禁止重排，未来只能追加字段；每条 posting list 的候选数由下一条 list 的 offset 推出（首条必须为 0），不单独存 count。v2（`CXTOPN\x02\x00`）仅作为**中间格式**存在，由 `topn_builder` 转换为运行时 v4。

### Key 生成策略

对每条词典记录（syllable_ids 如 `shu:ru:fa`）生成以下 key：

| key 类型 | 生成规则 | 示例 |
|----------|---------|------|
| exact_code | 完整拼音拼接 | `shurufa` |
| abbr_code | 每个音节首字母 | `srf` |
| mixed_code | `generate_mixed_keys()` 统一生成（每条最多 8 个，最长 16 字符） | `shrf`, `shurf`, `zhrmghg` |
| prefix_code | 对以上 key 取长度 1..6 的前缀 | `s`, `sr`, `sh`, `shu`, ... |

mixed code 码型：声母增强简拼（`shrf`）、首音节展开（`shurf`）、前两音节展开（`beijidx`）、长词首字母码（`zhrmghg`）等。exact_code 与 abbr_code 按完整编码写入索引，不受长度限制；prefix_code 只物化长度 1..6 的前缀（`MAX_MATERIALIZED_PREFIX_LENGTH = 6`）；mixed code 受 `MAX_MIXED_KEY_LENGTH = 16` 限制。每个 key 最多保留 `MAX_CANDIDATES_PER_KEY = 64` 个候选（见 `scripts/build_pinyin_topn.py`）。

### 排序规则

与 Rime 给词条排序的方式一致：先按匹配方式分三组，组内按 `ln(词频) + 拼写可信度` 排序
（`build_pinyin_topn.py` 的 `key_quality` / `key_score`，运行时 `rank_fallback_candidate` 用同一公式）。

| 组 | 基数 | 条件 | 例子 |
|----|------|------|------|
| 完整 | 60,000,000 | 词的每个音节都被 key 覆盖，打全或只打声母 | `qianm` → 前面、千米；`jde` → 觉得 |
| 补全 | 45,000,000 | 最后一个音节没打完（`ni` → 年），或 key 本身能切成完整音节却用了声母（`zhou` → 最后 = z+hou） | Rime 只在输入切不成完整音节时补全，并在有全拼切法时去掉简拼 |
| 更长的词 | 30,000,000 | 词比 key 长 | `xianzhan` → 先占领 |

```
score = 组基数 + clamp(500000 × (ln(frequency + 1) + credibility + 10), 0, 14,999,999)
credibility = Σ ln(0.5)  （每个只打声母或没打完的音节，与 spellings 的 abbrev 可信度相同）
```

同一个 key 下按 `score desc → frequency desc → text_length asc → text lexicographic asc` 稳定排序，
同一词多个读音只保留得分最高的一个。每个 key 最多保留 64 个候选。用户词（120,000,000 起）始终在三组之上。

打包时权重不超过 rime-ice 默认值 100 的多字词不进索引（`--min-word-frequency 100`）：它们对任何短 key
都排在最后，全拼输入时运行时查询照样能查到；索引从 153 MB 降到 39 MB，引擎测试集准确率不变。

### 构建流程

```cmd
# 1. Python 生成候选键、规范音节与评分 → CXTOPN v2 中间文件
python scripts/build_pinyin_topn.py --input data/pinyin.dict.db --output data/pinyin.topn.intermediate.bin

# 2. topn_builder 转换中间文件为运行时 CXTOPN v4（构建 Darts-clone Trie，绑定 pinyin.dict.bin）
build\tools\topn_index\Release\topn_builder.exe --input data/pinyin.topn.intermediate.bin --dictionary data/pinyin.dict.bin --output data/pinyin.topn.bin
```

`prepare_dictionary_bundle.py` 自动完成上述两步（`finalize_topn_index` 传入输出目录下的 `pinyin.dict.bin`），产出最终 `pinyin.topn.bin`。

数据流：`pinyin.dict.db` (SQLite) → `build_pinyin_topn.py`（键生成 + 身份 + 评分排序）→ `topn_builder --dictionary pinyin.dict.bin`（Darts-clone Trie 构建 + 写入共享候选 posting + 词典指纹）。键隐式存储于 Trie 结构中，候选只写词典词条索引；每个候选必须能在 `pinyin.dict.bin` 中按 `text + syllables + frequency` 命中，否则构建失败。

### C++ 类

```cpp
// engine/include/cxxime/short_code_cache.h
class ShortCodeCache {
public:
    bool load(const std::string& path, CandidateStoreView store);  // CreateFileA + ReadFile, 堆加载
    void unload();
    bool is_loaded() const;
    std::vector<Candidate> lookup(const std::string& key, int limit,
                                   QueryTrace* trace = nullptr,
                                   bool* prefix_complete = nullptr) const;
    // lookup: Darts-clone Double Array Trie 遍历 key,
    //   命中时设置 trace->cache_hit = true 并返回候选
};
```

`parse_short_cache()` 验证 header 为 `CXTOPN\x04\x00`、`header_size` / `file_size` / 各区段 canonical 布局，并比对 `dictionary_entry_count` 与 `dictionary_fingerprint` 是否等于传入的 `CandidateStoreView`，同时校验每条 posting 的 `dictionary_entry_index` 落在词典范围内。`lookup()` 通过 `darts_offset()` 解码 trie 单元，沿 key 字符遍历 Darts 状态转移，到达叶节点后读取 `ShortPostingList`（offset + prefix-complete 标志），再按 `ShortCandidatePosting` 从词典词条取出 text / syllables / frequency，候选 score 取 posting 内的构建期评分。

`Dict::open_dict()` 在加载 `pinyin.dict.bin` 后计算候选库指纹（`candidate_store()`），再自动尝试加载同目录的 `pinyin.topn.bin`。在 `open_bundle()`（Server 路径）中，Top-N 文件缺失或与词典指纹不匹配都是致命错误；独立模式下缺失时静默回退。

## 五笔快速路径（wubi86.dict.idx）

五笔没有单独的 `.topn.bin`。其"Top-N"能力在**构建期**固化进完整前缀索引 `wubi86.dict.idx`（CXWIDX v1）：

### 格式

```text
Header (48 bytes): magic "CXWIDX\x01\x00", version=1, dict_entry_count,
                   key_count, posting_count, keys_offset, postings_offset,
                   max_code_length=4
Key[key_count]     (12 bytes/条): packed_code(5 bit/字符, a=1..z=26) +
                                  posting_offset + posting_count
Postings[posting_count] (uint32/条): dict.bin 词条索引, 按排名预排序
```

### 预排序规则

postings 排名：**精确匹配 → 码长升序 → 词频降序 → 码序 → 文本长度 → 文本字典序**，并按文本去重。质量约束与审计（前三码候选提升预算、前 10 集合不变、频率不倒退等）见 [五笔候选质量排序](wubi-candidate-ranking.md)。

### 运行时查询

`Dict::open_wubi_bundle()` 加载 `wubi86.dict.bin` + `wubi86.dict.idx`；`Dict::lookup()` 优先走 `WubiPrefixIndex::find()`（对 packed_code 二分查找，O(log P + K)），直接返回预排序 postings，避免全表扫描。`wubi86.dict.idx` 是必需文件：缺失或加载失败时 `open_wubi_bundle()` 失败，服务端启动即报错。

### WubiTranslator 快照缓存

`WubiTranslator::translate()` 对同一输入码维护候选快照：首次查询按 `max(required, doubled_limit)` 扩大查询量，后续翻页直接复用快照，不再触发词典查询。快照在 `code` / `user_dict_version` / `candidate_preference_version` / `manual_candidate_order_version` / `disabled_system_entry_version` 任一变化时重建。该机制承担五笔模式的"会话级快速路径"，避免逐页重复二分。

## User Dict Short Index（拼音）

用户词按 `code` 建立 exact / prefix / abbr / mixed 四路索引，其中 abbr / mixed 与 1..6 前缀专供短输入快速路径查询；`UserLexicon::lookup_indexed()` 按 exact → prefix → abbr → mixed 顺序收集，按 `score_match()` 评分排序。详细结构见 [用户词库与候选偏好](user-dictionary.md)。

## Translator 集成

### 拼音快速路径入口

```cpp
// pinyin_translator.cc — translate() 开头
const int need = offset + fetch_limit + 1;
if (is_indexable_key(pinyin)) {
    fast = lookup_indexed_fast(pinyin, need, trace);
    if (fast.complete_index_hit && fast.candidates.size() > offset) {
        // 缓存足够 → 直接返回分页结果
        // 设置 cache_hit=true, exact_scan=0, prefix_scan=0, deadline_exceeded=false
        return page;
    }
    // 缓存不足 → seed TopKCollector, 继续标准管道
}
```

### Indexable Key Gate 条件

```cpp
bool is_indexable_key(const std::string& pinyin) {
    // 非空且全部为 a-z 小写字母，不限长度
}
```

### 拼音翻页行为

- `page_index == 0`：缓存通常足够，直接返回
- `page_index > 0`：同样尝试缓存；缓存不足时回退标准管道，缓存候选 seed 到 TopKCollector 避免重复查询

### 拼音合并规则

1. User dict short index 候选（按 `score_match()` 分层评分）
2. ShortCodeCache 候选按构建时 score 排序（文本 / 音节 / 词频取自 `pinyin.dict.bin` 词条）
3. 标准管道 (bounded dict lookup) 只用于补足缺失候选
4. 按 `Candidate.text` 去重
5. 缓存候选超过当前页时设置 `truncated=true`

快速路径命中后，拼音仍按统一顺序应用学习偏好与手动固定（见 [候选排序设计](candidate-ordering.md)）。

### 五笔集成

`WubiTranslator::lookup_candidates()`：`Dict::lookup()`（前缀索引预排序）→ `apply_candidate_preferences()`（学习启用时）→ `apply_manual_candidate_order()` → 按 text 去重并标记 `source = kWubi`；`translate()` 通过快照缓存复用结果、按页切分。

## Trace 语义

### cache_hit

| 场景 | cache_hit |
|------|-----------|
| ShortCodeCache 返回 ≥1 候选 | true |
| indexable key gate 未通过 | 不设置 |
| cache miss, 仅 bounded lookup 返回 | 不设置 |

### 纯 cache 命中时的 trace 值

```
cache_hit = true
exact_scan_count = 0
prefix_scan_count = 0
user_scan_count = 0        (用户词索引命中空 bucket 或无用户词)
deadline_exceeded = false
syllable_path_count = 0    (Syllabifier 未调用)
live_path_count = 0
```

五笔查询不经过 Syllabifier，trace 中 `syllable_path_count / live_path_count` 恒为 0。

### truncated

| 场景 | truncated |
|------|-----------|
| 缓存候选 > page_size, 截断 | true |
| 缓存不足, 回退 bounded lookup 时触发 scan budget / deadline | true |

## 文件清单

| 文件 | 职责 |
|------|------|
| `engine/src/short_code_cache_format.h` | CXTOPN v4 二进制格式结构体定义（ShortCacheHeader 64B, ShortPostingList 4B, ShortCandidatePosting 8B） |
| `engine/include/cxxime/short_code_cache.h` | ShortCodeCache 类声明 |
| `engine/include/cxxime/candidate_store.h` | CandidateStoreView: dict.bin 词条/字符串池视图、词条校验与 FNV-1a 指纹 |
| `engine/src/short_code_cache.cc` | 加载、校验 v4 header 与词典绑定（entry_count/fingerprint）, Darts trie 查找（darts_offset 解码）, 按词典词条还原候选 |
| `tools/topn_index/topn_index_format.h` | 运行时格式类型别名（TopnIndexHeader / TopnPostingList / TopnCandidatePosting） |
| `tools/topn_index/index_writer.cc` | write_index(): Darts::DoubleArray::build() 构建 trie, 候选命中词典后写入共享候选 posting 与词典指纹 |
| `tools/topn_index/index_reader.cc` | IndexReader: trie 遍历查找, 按词典词条还原候选 |
| `tools/topn_index/candidate_store_file.cc` | 构建期加载 `pinyin.dict.bin` 并暴露 CandidateStoreView（含 FNV-1a 指纹） |
| `tools/topn_index/intermediate_reader.cc` | IntermediateReader: 读取 Top-N 中间文件格式 |
| `tools/topn_index/main.cc` | topn_builder 入口: 中间文件 + pinyin.dict.bin → 运行时共享候选索引 |
| `tools/topn_index/benchmark.cc` | topn_benchmark 工具: 中间格式基线 vs 共享候选索引 的延迟/QPS 对比 |
| `third_party/darts-clone/include/darts.h` | Darts-clone Double Array Trie 库（构建 + 查找） |
| `scripts/build_pinyin_topn.py` | 离线生成 Top-N 候选键、规范音节与评分（v2 中间文件） |
| `scripts/benchmark_topn.ps1` | 共享候选索引 benchmark 脚本（`--dictionary` / `--index`） |
| `data/tools/dict_builder/wubi_prefix_index.py` | 五笔完整前缀索引构建（CXWIDX v1，预排序 postings） |
| `engine/src/wubi_prefix_index.cc` | WubiPrefixIndex 加载与二分查询 |
| `engine/include/cxxime/dict.h` | ShortCodeCache 成员 + getter, 用户词索引结构 |
| `engine/src/dict.cc` | open_dict() / open_bundle() 加载 topn.bin 与 wubi idx, 用户词索引构建与查询 |
| `engine/include/cxxime/translator.h` | is_indexable_key(), lookup_indexed_fast(), 查询页缓存 |
| `engine/src/pinyin_translator.cc` | 拼音快速路径入口, 合并逻辑, 用户词版本过滤 |
| `engine/src/wubi_translator.cc` | 五笔查询（前缀索引 + 偏好/手动排序 + 快照缓存） |
| `engine/src/engine.cc` | 会话与提交链路, 查询缓存清理 |
| `test/engine/short_cache_test.cc` | ShortCodeCache 单元测试（v4 加载/查找、词典指纹不匹配与损坏拒绝） |
| `test/engine/wubi_prefix_query_test.cc` | 五笔前缀索引查询/排序验证 |
| `test/support/topn_test_data.h` / `.cc` | Top-N 测试数据辅助函数 |

## 查询页缓存

`PinyinTranslator` 还持有一份按「输入 + 页码 + 页大小」索引的查询页缓存，位于快速路径之前（`translate_page()` 开头即查询）：

- **触发条件**：任意输入长度，只要页参数与输入命中缓存条目
- **容量**：LRU 64 条（`kMaxQueryCacheEntries = 64`），`sequence` 递增序号实现淘汰
- **失效**：`user_dict_version` / `candidate_preference_version` / `manual_candidate_order_version` / `disabled_system_entry_version` / `composition_learning_version` 任一变化后条目不再命中
- **非缓存场景**：deadline 命中或 deadline_exceeded 时不写入

详细机制见 [候选词选词算法 — 查询页缓存](candidate-selection.md#查询页缓存)。
