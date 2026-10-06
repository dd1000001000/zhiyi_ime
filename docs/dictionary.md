# 词典系统设计

## 1. 架构概览

知意输入法采用三层架构处理拼音到汉字的转换：

```
用户输入 "nihao"
    │
    ▼
┌─────────────┐   spellings.bin (Patricia trie)
│ SpellingsIndex │ ─── prefix_search("ni") → [{syllable:"ni", type:normal}, ...]
│   (Prism 层)   │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Syllabifier  │ ─── segment("nihao") → ["ni", "hao"]
│  (分词层)     │     BFS 构建音节图 + DFS 枚举路径
└──────┬──────┘
       │
       ▼
┌─────────────┐   dict.bin (排序数组) + 用户数据 (TSV 多路索引)
│    Dict      │ ─── lookup("ni:hao") → [你好(100), 你号(50), ...]
│  (Table 层)   │
└─────────────┘
```

### 1.1 三层架构详解

```
┌─────────────────────────────────────────────────────────────────┐
│ 第一层: Spelling Algebra (构建时规则引擎)                        │
│                                                                 │
│ 输入: 音节表 {ni, hao, da, di, cha, ca, ...}                    │
│ 规则: abbrev/fuzz/derive/xform/erase/xlit (regex-based)         │
│ 输出: Script = map<输入串, list<Spelling{syllable, type, cred}>> │
│                                                                 │
│ 效果: 音节表被展开为所有可能的输入串→音节映射                     │
├─────────────────────────────────────────────────────────────────┤
│ 第二层: Prism (构建时建索引, 运行时前缀搜索)                     │
│                                                                 │
│ 构建: Patricia trie 存 Script 所有 key                           │
│       Node.spellings[] = [{syllable, type, cred}...]            │
│ 运行: prefix_search(input) → spellings[{syllable, type, cred}]  │
│                                                                 │
│ 效果: O(输入长度) 找到所有匹配的音节                             │
├─────────────────────────────────────────────────────────────────┤
│ 第三层: Table (构建时建索引, 运行时按音节序列查询)               │
│                                                                 │
│ 构建: 按 syllable_ids 序列索引词条                               │
│ 运行: SyllableGraph indices → Code(syllable_ids) → DictEntry    │
│                                                                 │
│ 效果: O(音节数) 精确查找词条                                     │
├─────────────────────────────────────────────────────────────────┤
│ Syllabifier (运行时, 连接第二层和第三层)                         │
│                                                                 │
│ BFS 走 trie 前缀搜索:                                           │
│   每步 prefix_search(input[pos:]) → {syllable, type, cred}       │
│   构建 SyllableGraph (edges: pos→pos → {syllable_id→props})     │
│   DFS 枚举路径 → lookup_by_syllables() → 候选                   │
└─────────────────────────────────────────────────────────────────┘
```

### 核心类关系

```
Engine
  ├── PinyinProcessor       按键处理（字母、退格、空格、数字选候选）
  ├── PinyinTranslator      翻译器，组合分词 + 词典查询
  │     ├── Dict*            词典指针
  │     ├── Syllabifier*     主分词路径（BFS+DFS 音节图）
  │     └── PinyinSegmentor  简版分词器（Syllabifier 不可用时回退）
  ├── Dict                   主词典（二进制加载）+ 用户词库/候选偏好（内存+TSV）
  ├── SpellingsIndex         拼写索引（二进制加载），供 Syllabifier 使用；按拼音方案载入全拼或对应双拼表
  ├── Context                输入状态（拼音缓冲、候选列表、已提交文本）
  └── Config                 配置（字体、布局、主题）
```

## 2. 查询流示例

### 2.1 输入 "dd"（缩写匹配）

```
第一层 Spelling Algebra (构建时):
  音节表: {da, di, ni, hao, ...}
  规则: abbrev/^(.+).$/$1/  (取首字母)
  Script 产出:
    "d"  → [("da", abbrev, -0.693), ("di", abbrev, -0.693)]
    "da" → [("da", normal, 0)]
    "di" → [("di", normal, 0)]
    "n"  → [("ni", abbrev, -0.693)]
    "ni" → [("ni", normal, 0)]
    ...

第二层 Prism (运行时):
  输入 "dd":
    pos=0: prefix_search("dd") → 匹配 "d"
      QuerySpelling("d") → {da:abbrev, di:abbrev}
      edges[0][1] = {da, di}
    pos=1: prefix_search("d") → 匹配 "d"
      edges[1][2] = {da, di}

  SyllableGraph:
    edges: {0→1: {da,di}, 1→2: {da,di}}
    Transpose → indices: {0: {da,di}, 1: {da,di}}

第三层 Table (运行时):
  遍历 indices 路径:
    [da,da] → lookup_by_syllables → 无结果
    [da,di] → lookup_by_syllables → 无结果
    [di,da] → lookup_by_syllables → 无结果
    [di,di] → lookup_by_syllables → "弟弟" ✓, "笛笛" ✓
```

### 2.2 输入 "ca"（模糊拼音）

```
第一层 (构建时):
  规则: derive/^([zcs])h/$1/  (zh↔z, ch↔c, sh↔s)
  Script:
    "ca"  → [("ca", normal, 0), ("cha", fuzzy, -0.693)]
    "cha" → [("cha", normal, 0)]

第二层 (运行时):
  pos=0: prefix_search("ca") → 匹配 "ca"
    QuerySpelling("ca") → {ca:normal, cha:fuzzy}

第三层 (运行时):
  路径 [ca]  → "擦" ✓
  路径 [cha] → "差" ✓  ← 模糊匹配
```

## 3. 二进制格式规范

### 3.1 spellings.bin — 拼写索引（Prism 层）

将输入字符串映射到音节解释。例如 `"d"` → `["da"(缩写), "di"(缩写), "de"(缩写)]`。

> 知意输入法只提供全拼（含简拼与混合输入）：`scripts/dictionary_bundle_layout.py` 的 `SHUANGPIN_SCHEME_NAMES`
> 为空，不生成、也不安装下面提到的双拼拼写表。双拼相关说明沿用自 CxxIME，引擎代码仍支持，供以后恢复时参考。

运行时按拼音方案载入对应的拼写表：全拼用 `pinyin.spellings.bin`（`pinyin.full-pinyin.schema.json`）；微软双拼、小鹤双拼、
自然码双拼、搜狗双拼分别用 `pinyin.<方案名>-shuangpin.spellings.bin`（同名 `pinyin.<方案名>-shuangpin.schema.json`，
方案名见 `scripts/dictionary_bundle_layout.py` 的 `SHUANGPIN_SCHEME_NAMES`）。五份拼写表共用同一份 `pinyin.dict.bin`，
清单角色分别是 `pinyin_spellings` 与 `pinyin_spellings_<方案名>_shuangpin`，文件格式同为下面的 Patricia trie。

采用 **Patricia trie**（压缩前缀树）实现 O(k) 前缀搜索，k 为前缀长度。

#### 文件布局

```
┌────────────────────────────────────────────┐
│ Header (28 bytes)                          │
│   magic[8]      = "CXSPL\x02\x00\x00"     │
│   version       = 2                        │
│   node_count    = N                        │
│   string_data_size                         │
│   nodes_offset  = 28                       │
│   strings_offset                           │
├────────────────────────────────────────────┤
│ Nodes[node_count]  (变长)                   │
│   每个节点:                                 │
│     key_offset    (uint32) → string_data   │
│     key_len       (uint32)                 │
│     num_spellings (uint8)  = ns            │
│     num_children  (uint8)  = nc            │
│     padding       (uint16)                 │
│     SpellingEntry[ns]  (每条 14 bytes)      │
│     ChildEntry[nc]     (每条 8 bytes)       │
├────────────────────────────────────────────┤
│ String Data (连续 UTF-8 字节块)             │
│   所有 key 和 syllable 字符串紧凑存储        │
└────────────────────────────────────────────┘
```

#### 结构体定义

**SpellingEntry** (14 bytes, `#pragma pack(push, 1)`)

| 偏移 | 大小 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 4 | uint32 | syllable_offset | 音节字符串在 string_data 中的偏移 |
| 4 | 4 | uint32 | syllable_len | 音节字符串长度 |
| 8 | 1 | uint8 | type | 0=正常, 1=模糊, 2=缩写 |
| 9 | 1 | — | padding | |
| 10 | 4 | float | credibility | 可信度评分 |

Python 格式: `"<IIbxf"`

**ChildEntry** (8 bytes)

| 偏移 | 大小 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 1 | uint8 | first_char | 子节点边的首字符 |
| 1 | 3 | — | padding | |
| 4 | 4 | uint32 | node_index | 子节点在节点数组中的索引 |

Python 格式: `"<B3xI"`

**SpellingType 枚举**

| 值 | 名称 | 含义 |
|----|------|------|
| 0 | kNormalSpelling | 标准音节匹配 |
| 1 | kFuzzySpelling | 模糊拼音匹配 |
| 2 | kAbbreviation | 缩写匹配（单字母→完整音节） |

#### Trie 示例

输入 `"da"→"da"`, `"di"→"di"`, `"de"→"de"`, `"d"→"da"(缩写), `"d"→"di"(缩写), `"d"→"de"(缩写)`:

```
root (node 0)
  └─ key="" (root 节点 key 为空)
     spellings: (无)
     children: 'd' → node 1

node 1
  key="d"
  spellings: [da(abbrev), di(abbrev), de(abbrev)]
  children: 'a' → node 2, 'e' → node 3, 'i' → node 4

node 2  key="a"  spellings: [da(normal)]  children: (无)
node 3  key="e"  spellings: [de(normal)]  children: (无)
node 4  key="i"  spellings: [di(normal)]  children: (无)
```

搜索 `"dd"` 时：走到 node 1 (key="d")，收集缩写拼写，然后找 'd' 子节点 → 未找到 → 返回缩写结果。

#### 搜索算法 (trie_prefix_search)

```cpp
prefix_pos = 0, current_node = 0
while current_node < node_count:
    node = nodes + node_offsets[current_node]
    key = strings + node.key_offset
    if prefix_pos + key_len > prefix_len: break
    if memcmp(key, prefix + prefix_pos, key_len) != 0: break
    prefix_pos += key_len
    collect all SpellingEntry from node
    if prefix_pos >= prefix_len: break
    find child with first_char == prefix[prefix_pos]
    if not found: break
    current_node = child.node_index
```

复杂度: O(k)，k = 前缀长度。

### 3.2 dict.bin — 主词典（Table 层）

存储拼音到汉字的映射，按 `syllable_ids` 排序，支持 O(log n) 二分查找。

#### 文件布局

```
┌────────────────────────────────────────────┐
│ Header (28 bytes)                          │
│   magic[8]      = "CXDIC\x02\x00\x00"     │
│   version       = 2                        │
│   entry_count   = M                        │
│   string_data_size                         │
│   entries_offset = 28                      │
│   strings_offset                           │
├────────────────────────────────────────────┤
│ DictEntry[entry_count]  (每条 20 bytes)     │
│   按 syllable_ids 字节序升序排列             │
├────────────────────────────────────────────┤
│ String Data (连续 UTF-8 字节块)             │
└────────────────────────────────────────────┘
```

#### DictEntry / CandidateStoreEntry 结构体 (20 bytes, `#pragma pack(push, 1)`)

| 偏移 | 大小 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 4 | uint32 | syllable_ids_offset | 拼音键偏移，如 "ni:hao" |
| 4 | 4 | uint32 | text_offset | 汉字文本偏移，如 "你好" |
| 8 | 4 | uint32 | syllable_ids_len | 拼音键长度 |
| 12 | 4 | uint32 | text_len | 汉字文本长度 |
| 16 | 4 | int32 | frequency | 词频 |

Python 格式: `"<IIIIi"`

`static_assert(sizeof(DictEntry) == 20)`

`using DictEntry = CandidateStoreEntry;`（`CandidateStoreEntry` 定义在 `engine/include/cxxime/candidate_store.h`，别名见 `engine/src/binary_format.h`）：运行时 Top-N 索引通过 `CandidateStoreView` 直接引用这些词条，不再复制文本与音节。

#### 搜索算法 (lookup_by_syllables)

1. 将音节列表拼接为冒号分隔键: `["ni","hao"]` → `"ni:hao"`
2. 二分查找 `syllable_ids` 匹配的条目（memcmp 比较）
3. 从匹配位置向前扫描，收集所有相同 `syllable_ids` 的条目
4. 查询内存 user_entries_（同一 code）
5. 合并结果，按 frequency 降序排列（同频时按文本长度升序、文本字典序）

#### 搜索算法 (lookup — 前缀匹配)

与 lookup_by_syllables 类似，但匹配 `syllable_ids` 以给定前缀开头的条目。

#### 反查算法 (reverse_lookup)

线性扫描所有条目，比较 `text` 字段。复杂度 O(n)，因为 dict.bin 按 syllable_ids 排序而非 text。

### 3.3 五笔完整前缀索引（wubi86.dict.idx / CXWIDX v1）

五笔使用独立的完整前缀索引，取代原来的整数 ID 索引（CXIDX）。`wubi86.dict.idx` 在构建时由 `data/tools/dict_builder/wubi_prefix_index.py` 生成，记录每个可达五笔编码前缀的排序词条列表；运行时由 `WubiPrefixIndex` 整文件加载，`Dict::lookup()` 二分查找后直接取预排序 postings，避免全表扫描。

#### 文件布局

```
┌────────────────────────────────────────────┐
│ Header (48 bytes)                          │
│   magic[8]      = "CXWIDX\x01\x00"         │
│   version       = 1                        │
│   header_size / file_size                  │
│   dict_entry_count / key_count             │
│   posting_count                            │
│   keys_offset / postings_offset            │
│   max_code_length = 4 / reserved = 0       │
├────────────────────────────────────────────┤
│ Key[ key_count ]  (每条 12 bytes)           │
│   packed_code / posting_offset             │
│   posting_count                            │
├────────────────────────────────────────────┤
│ Postings[ posting_count ]  (uint32 数组)    │
│   dict.bin 词条索引，按排名预排序           │
└────────────────────────────────────────────┘
```

- `packed_code`：五笔码按 5 bit/字符 压缩（a=1..z=26），最多 4 码
- key 按 `packed_code` 升序，`posting_offset` 连续覆盖 postings 区
- postings 排名：精确匹配 → 码长升序 → 词频降序 → 码序 → 文本长度 → 文本字典序，并按文本去重
- 构建入口：`build_runtime_dictionary.py --wubi-prefix-index`

#### 运行期查询

`Dict::open_wubi_bundle()` 加载 `wubi86.dict.bin` + `wubi86.dict.idx`；`Dict::lookup()` 优先走 `WubiPrefixIndex::find()`（对 `packed_code` 二分查找），直接返回预排序 postings。`wubi86.dict.idx` 是必需文件：索引缺失或加载失败时 `open_wubi_bundle()` 返回失败，服务端启动即报错，不再回退到旧的全表扫描路径。

### 3.4 当前格式版本

运行时加载与构建校验（`scripts/verify_dictionary_bundle.py`）只接受以下当前格式：

| 格式 | magic (hex/ASCII) | 说明 |
|------|-------------------|------|
| spellings v2 | `43 58 53 50 4C 02 00 00` / `CXSPL\x02\x00\x00` | Patricia trie（当前生产格式） |
| dict v2 | `43 58 44 49 43 02 00 00` / `CXDIC\x02\x00\x00` | 平坦排序数组（词条 = `CandidateStoreEntry`，20 字节） |
| dict.idx v3 | `43 58 49 44 58 00 00 00 00` / `CXIDX\0\0\0\0` | 整数 ID 索引（音节→词条，zero-copy） |
| wubi idx v1 | `43 58 57 49 44 58 01 00` / `CXWIDX\x01\x00` | 五笔完整前缀索引（packed code → 排序 postings） |
| topn.bin v4 | `43 58 54 4F 50 4E 04 00` / `CXTOPN\x04\0` | 共享候选格式：Darts-clone 双数组 Trie 键索引 + 8 字节 posting（词典词条索引 + score）；文本 / 音节 / 词频复用 dict.bin，并用 `dictionary_entry_count` + FNV-1a 指纹绑定词典 |

## 4. 数据存储方案

### 4.1 存储层选择

| 数据 | 格式 | 原因 |
|------|------|------|
| 主词典 (dict.bin) | 堆内存二进制加载 | 只读，一次性读入，O(log n) 二分查找 |
| 拼写索引 (spellings.bin) | 堆内存二进制加载 | 只读，一次性读入，O(k) trie 遍历 |
| 用户数据 (user_/learning_*.tsv) | 内存多路索引 | 手工词库 + 候选偏好，TSV 持久化 |

### 4.2 文件大小对比

| 文件 | SQLite (.db) | 二进制 (.bin / .idx) | 压缩 (.zip) |
|------|-------------|----------------------|-------------|
| pinyin 主词典 | 146 MB | 69.5 MB (dict.bin) | 64.3 MB |
| pinyin 整数 ID 索引 | — | 46.2 MB (dict.idx) | — |
| pinyin Top-N 候选索引 | — | 121.1 MB (topn.bin, CXTOPN v4 共享候选；v3 DAT-16 为 291.0 MB) | — |
| pinyin 拼写索引 | — | 0.03 MB (spellings.bin) | — |
| wubi86 主词典 | 3.2 MB | 2.5 MB (dict.bin) | 2.0 MB |
| wubi86 完整前缀索引 | — | 2.3 MB (dict.idx) | — |

### 4.3 为什么不用 SQLite 存主词典

- SQLite 需要编译 120MB 的 sqlite3.c
- SQL `LIKE` 查询 + C++ 过滤比纯内存数据结构慢一个数量级
- 页开销 + 索引冗余导致文件体积膨胀
- 主词典是只读数据，不需要事务/写入能力

### 4.4 为什么不用 DARTS+MARISA

- DARTS (Double-Array Trie) 和 MARISA Trie 需要引入外部依赖
- Patricia trie 在零依赖条件下实现 O(k) 搜索，性能相当
- 字符串去重对短字符串（拼音键平均 4-6 字节）不划算：2.69M × 8 字节引用开销 > 去重节省

## 5. 构建流程

### 5.1 数据源

原始词典存储在 SQLite `.dict.db` 文件中（以 `.zip` 压缩提交到 git）：

```sql
-- spellings 表（拼写规则）
CREATE TABLE spellings (
    input TEXT,        -- 输入键，如 "ni", "n", "ne"
    syllable TEXT,     -- 音节，如 "ni", "ne"
    type INTEGER,      -- 0=正常, 1=模糊, 2=缩写
    credibility REAL   -- 可信度
);

-- dict 表（词典条目）
CREATE TABLE dict (
    text TEXT,          -- 汉字，如 "你好"
    code TEXT,          -- 拼音编码，如 "ni hao"
    frequency INTEGER,  -- 词频
    syllable_ids TEXT   -- 音节键，如 "ni:hao"
);
```

### 5.2 构建工具

`data/tools/build_runtime_dictionary.py`（`dict_builder` 包）— 将 SQLite 转换为二进制格式：

```bash
# 从 .dict.db 构建（直接路径）
python data/tools/build_runtime_dictionary.py -i data/pinyin.dict.db -o data/pinyin

# 从 .zip 构建（自动解压到临时目录）
python data/tools/build_runtime_dictionary.py -i data/pinyin.dict.db.zip -o data/pinyin

# 仅构建 spellings 或 dict
python data/tools/build_runtime_dictionary.py -i data/pinyin.dict.db -o data/pinyin --spellings-only
python data/tools/build_runtime_dictionary.py -i data/pinyin.dict.db -o data/pinyin --dict-only
```

输出文件：
- `data/pinyin.spellings.bin` — Patricia trie 拼写索引（全拼）
- `data/pinyin.<方案名>-shuangpin.spellings.bin` — Patricia trie 拼写索引（微软、小鹤、自然码、搜狗四种双拼各一份，由 `prepare_dictionary_bundle.py` 用对应双拼 schema 单独导出）
- `data/pinyin.dict.bin` — 排序数组主词典
- `data/pinyin.reverse.idx` — 词语反查索引（由 `prepare_dictionary_bundle.py` 生成）

五笔：先在临时副本上剥离系统符号（emoji 与纯 P/S 符号，含旧 `co*` 扩展项），再用过滤后的词典生成 `dict.bin` + 完整前缀索引。
剥离掉的符号不进入候选，而是并入独立清单 `data/symbol_catalog.json`，由 `generate_symbols.py` 生成引擎使用的 `data/symbols.json`：

```bash
python data/tools/filter_dictionary_symbols.py --wubi-input data/wubi86.dict.db \
    --pinyin-input data/pinyin.dict.db \
    --wubi-output <filtered-wubi.dict.db> --pinyin-output <filtered-pinyin.dict.db>
python data/tools/build_runtime_dictionary.py -i <filtered-wubi.dict.db> -o data/wubi86 \
    --dict-only --wubi-prefix-index
python data/tools/generate_symbols.py --catalog data/symbol_catalog.json \
    --output data/symbols.json
```

### 5.3 构建管线

```
fetch_pinyin_dictionary.py / fetch_wubi_dictionary.py    从网络获取词典数据
        │
        ▼
   pinyin.dict.db                 SQLite 源文件（git 中以 .zip 存储）
        │
        ▼
  repair_pinyin_dictionary.py     打包时修正音节（见 5.6），再裁剪到权重 ≥100 的词
        │
        ▼
  build_runtime_dictionary.py     Python 转换工具（dict_builder 包）
   ├── pinyin_spellings.py        SQLite → Patricia trie → spellings.bin
   ├── runtime_dictionary.py      SQLite → 排序数组 → dict.bin
   └── pinyin_syllable_index.py   SQLite → 整数 ID 索引 → dict.idx
        │
        ▼
   pinyin.spellings.bin           运行时内存加载
   pinyin.*-shuangpin.spellings.bin  四种双拼拼写表（prepare_dictionary_bundle.py 生成）
   pinyin.dict.bin                 运行时内存加载
   pinyin.dict.idx                 整数 ID 索引

  build_pinyin_topn.py            SQLite → Top-N 候选键、规范音节与评分（CXTOPN v2 中间文件）
        │
        ▼
  topn_builder --dictionary       中间文件 + pinyin.dict.bin → 运行时 CXTOPN v4 共享候选索引
        │
        ▼
   pinyin.topn.bin                运行时内存加载（Darts trie + 8 字节 posting：词典词条索引 + score）

  fetch_wubi_dictionary.py / filter_dictionary_symbols.py   五笔与拼音源数据 → 剥离系统符号的词典
        │
        ▼
  build_runtime_dictionary.py --dict-only --wubi-prefix-index
        │
        ▼
   wubi86.dict.bin + wubi86.dict.idx（完整前缀索引）

  symbol_catalog.json / generate_symbols.py         独立符号清单 → symbols.json（不进入词典候选）

  prepare_dictionary_bundle.py   bundle 打包 + 反查索引
        │
        ▼
   pinyin.reverse.idx / wubi86.reverse.idx（Settings 词语反查）
```

### 5.4 Patricia Trie 构建过程

1. 从 SQLite 读取所有 `(input, syllable, type, credibility)` 条目
2. 逐条插入 Patricia trie：
   - 遍历 key 字符，查找匹配的子节点
   - 完全匹配：递归进入子节点
   - 部分匹配：分裂节点（创建公共前缀中间节点）
   - 无匹配：创建新叶节点
3. BFS 遍历分配节点索引
4. 序列化：header + 节点数据 + 字符串数据

### 5.5 SQLite → 二进制的数据流

```
SQLite spellings 表          Patricia Trie              spellings.bin
┌─────────────────┐         ┌─────────┐               ┌──────────────┐
│ input│syll│type │  ──→    │  root   │  ──serialize→  │ Header       │
│ "d"  │"da"│  2  │         │   │     │               │ Nodes[]      │
│ "da" │"da"│  0  │         │   d     │               │ String Data  │
│ ...  │    │     │         │  /│\    │               └──────────────┘
└─────────────────┘         a  e  i
```

### 5.6 拼音音节修正（repair_pinyin_dictionary.py）

`pinyin.dict.db` 由 rime-ice 转换而来，转换时去掉了音节间的空格（`xi an` → `xian`）再按"最长匹配"重新切分，
约 3.4 万个词的音节因此切错（西安 `xian`、办公室 `bang:o:n:g:shi`、单干 `dang:an`），简拼和组句都找不到它们；
rime-ice 的 tencent 词表没有注音列，约 98 万条的权重被当成了编码（`西安站` 编码 `100`）。
`prepare_dictionary_bundle.py` 在临时库上运行 `repair_pinyin_dictionary.py`，源文件不改：

- 有编码的词按每个字自己的读音重新切分编码（带 `·` `-` 的译名只算汉字）；单字的编码就是一个音节；
  切不开且含非音节片段的少数词条删除；
- 没有注音的 tencent 词只保留 3～4 字的，按 Rime 的造词规则注音：优先用词里已有的词的读音
  （银行卡 → 银行 yin:hang），其余单字取占该字权重 ≥5% 的读音，至多 4 种组合，权重记 100。

之后裁剪只保留单字和权重 ≥100 的词（约 146 万条）。

## 6. 用户数据（用户词库与候选偏好）

用户个性化数据分为三个独立资源：

- **用户词库（user lexicon）**：手工添加的词条，采用多路内存索引（exact / prefix / abbr / mixed）+ TSV 持久化（`user_pinyin.tsv` / `user_wubi.tsv`）。词条存储 `text`、`code`、`syllables`（冒号分隔音节键）、`abbr_code`、`mixed_keys`，支持 `frequency`、`sequence`（版本计数）、`deleted`（软删除）。
- **候选偏好（candidate preference）**：候选学习记录，按 code 索引 + 6 列 TSV 持久化（`learning_pinyin.tsv` / `learning_wubi.tsv`），在翻译结果上应用以提升命中候选（`origin = kLearned`）。
- **手动候选顺序（manual candidate order）**：用户为编码显式固定的候选顺序，按输入码索引 + 5 列 TSV 持久化（`candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv`），优先级高于候选偏好。

用户词评分分双档：`UserScoringProfile::kPinyin` 和 `kWubi`，由 `Dict` 初始化时通过 `UserLexicon::set_scoring_profile()` 设置（拼音词典用 `kPinyin`，五笔词典用 `kWubi`）；候选偏好得分高于普通用户词，用于把学过的候选稳定置前；手动候选顺序在偏好之上直接前置固定项。完整优先级见 [候选排序设计](candidate-ordering.md)。

详见 [用户词库与候选偏好](user-dictionary.md)。

## 7. 二进制加载

### 7.1 加载流程

四个运行时文件（`dict.bin`、`dict.idx`、`topn.bin`、`spellings.bin`）都通过 `MappedFile`
（`engine/include/cxxime/mapped_file.h`）只读映射到内存，不再复制到堆上：

```cpp
MappedFile file;
file.open(path);          // CreateFileMapping + MapViewOfFile，随后 PrefetchVirtualMemory 整个文件
data_ = file.data();      // 验证 magic → 设置指针 → 构建索引（布局与文件一致，指针直接偏移）
```

- 页面是文件支持的共享页：不计入进程的私有提交，内存紧张时系统可以直接丢弃再从文件读回；
  任务管理器里服务端的"内存"少了约 140 MB（146 万条词库：堆加载 160 MB → 映射后约 24 MB 私有内存）。
- 启动时一次 `PrefetchVirtualMemory` 把整个文件读入，正常使用中不会因首次访问缺页；
  CxxIME 原本改用堆加载是为了避开 mmap 换页（防病毒软件 / 过滤驱动介入时的延迟），知意用预取换取内存。
- 文件在映射期间不能被截断或删除，只能重命名（打开时带 `FILE_SHARE_DELETE`）。替换词典必须"写新文件、
  把旧文件改名挪开、把新文件移到原名"，不能原地覆盖；写测试词典的 `Dict::create_test_dict`、
  `SpellingsIndex::create_test_trie` 和测试工具先调用 `MappedFile::rename_away(path)`。
  安装程序写入新的版本目录，热重载（`DictionaryMonitor`）重新映射新文件后旧映射才释放。

### 7.2 资源管理

`unload()` 关闭映射（`MappedFile::close()`：UnmapViewOfFile + CloseHandle）。

## 8. 测试

> 测试套件不在公开仓库中，以下内容仅供参考。

### 8.1 测试框架

自定义测试框架 (`test/support/testutil.h`)，无外部依赖：

```cpp
TEST(SuiteName, TestName) { ... }          // 自注册测试用例
ASSERT_TRUE(cond) / ASSERT_EQ(a, b) / ...  // 断言宏（fatal）
RUN_ALL_TESTS()                            // main 入口，自动发现并运行
```

### 8.2 测试覆盖

与本模块相关的测试覆盖如下（完整测试套件由 CMake 自动注册，运行方式见 [开发说明](development.md)）：

| 测试文件 | 测试内容 |
|----------|----------|
| `dict_test` | 词典打开/前缀查找/音节查找/空查询/反查/用户词频更新 |
| `dictionary_format_test` | 当前格式可加载；dict v1 / spellings v1 / dict.idx v2 被拒绝 |
| `wubi_test` | Wubi86 基本查找/前缀匹配/去重 |
| `wubi_prefix_query_test` | 五笔完整前缀索引查询排序 |
| `short_cache_test` | 短码缓存查询（Top-N v4 索引与词典词条绑定） |
| `dictionary_monitor_test` | 词典 manifest 监控 |
| `symbol_table_test` | symbols.json 符号表加载/分类/分页 |
| `wubi_engine_test` | 五笔引擎集成（词典查询路径） |
| `engine_source_test` | 引擎源码级测试（词典加载/查询） |
| `engine_test` | 引擎集成翻译路径（词典为数据源） |
| `shuangpin_test` | 双拼方案描述表、候选规范输入键、分段选词与学习、方案热切换 |
| `shuangpin_schema_runtime_test` | 合成双拼 schema 生成的 trie 由运行时索引加载与切分 |
| `pinyin_spelling_schema_test` | 双拼 schema 契约、黄金映射与冲突声明校验（Python） |
| `ordinary_candidate_test` | 语义策略：混合文本保留、纯符号/emoji 剔除，个人词/偏好/排序/学习的加载、导入与修改路径 |
| `dictionary_symbol_policy_test` | 词典符号过滤、分类与构建期共享策略验证（Python） |
| `symbol_catalog_test` | 独立符号清单 → symbols.json 的可复现性、顺序与非法输入边界（Python） |
| `unicode_classification_generator_test` | 冻结 Unicode 输入复现符号/emoji 分类数据（Python） |
| `wubi_prefix_index_test` | 五笔完整前缀索引构建验证（Python） |
| `pinyin_topn_pipeline_test` | Top-N 键生成与共享候选转换验证（Python） |
| `reverse_index_pipeline_test` | 反查索引流水线验证（Python） |
| `dictionary_format_verifier_test` | 构建校验器对当前格式 magic/version 的检查（Python） |

