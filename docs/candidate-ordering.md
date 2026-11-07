# 候选排序

本文档讲一页候选是怎么排出来的：从输入查出候选、默认排序、学习偏好与手动固定、全拼长词提前、上文推荐，
到快捷短语、英文单词等固定位置的候选和分页。（原来的“候选排序设计”“候选词选词算法”“候选的位置”三篇合并于此。）

## 概述

第一页按下面的顺序排出来，每一步只调整前一步的结果：

| 步骤 | 做什么 | 代码 |
|---|---|---|
| 1. 查词与默认排序 | 从词典、用户词库查出候选，按词典打分排序（无个性化数据时的顺序） | `pinyin_translator.cc` / `wubi_translator.cc` / `mixed_translator.cc` |
| 2. 学习偏好 | 选过的词加分或置顶（`apply_candidate_preferences`），再按分数重排 | 同上 |
| 3. 手动固定 | 为某个编码固定的词按指定顺序放在最前（`apply_manual_candidate_order`） | 同上 |
| 4. 全拼长词提前 | `xian` 的“西安”这类多音节词放到第 4 位（手动固定的词多于 3 个时在它们之后） | `pinyin_translator.cc` 的 `promote_long_words` |
| 5. 上文推荐 | 第一页取 2 页的候选，模型按光标前的文字挑一个放到首位并标星，再截回一页 | `engine.cc` 的 `translate_composition`、`laya_rerank.cc` |
| 6. 固定位置的候选 | emoji 跟在它的词后面；快捷短语、日期时间、英文单词放到各自的位置 | `engine.cc`、`candidate_placement.h` |
| 7. 分页 | 按每页候选数切开；翻页时从上一页最后一个候选之后接着排 | `translation_result` / `CandidatePage` |

要点：

- **手动固定 > 学习偏好 > 默认排序**。上层只改变下层结果的先后，不改变候选集合（手动固定可能补回被截断的词典候选，
  见[边界与交互](#9-边界与交互)）。
- **上文推荐不动手动固定的词**：模型只在它们之后挑，挑中的放在它们后面。
- **固定位置的候选在所有排序之后放进去**，模型看不到也挪不动它们；翻页时重新查询第一页也经过同样的步骤，所以每次结果一样。

相关文档：

- 短输入快速路径与 Top-N 索引见 [短输入快速路径](short-input-fast-path.md)；
- 五笔默认排序的质量约束与审计见 [五笔候选质量排序](wubi-candidate-ranking.md)；
- 学习偏好与手动固定的数据结构、持久化见 [用户词库与候选偏好](user-dictionary.md)；
- 快捷短语的写法见 [快捷短语](custom-phrases.md)；
- 查询预算与超时见 [查询预算与候选收集](query-control.md)。

## 1. 默认排序：拼音

```
用户输入 → TSF → IPC → Engine::process_key → PinyinProcessor → PinyinTranslator → 候选词列表
```

`PinyinTranslator::translate()` 分四步；短输入另有 Top-N 快速路径（见 [短输入快速路径](short-input-fast-path.md)），
其分数由构建期按同一公式预先算好，顺序与这里一致。

### 1.1 拼写图（Syllabifier）

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

### 1.2 路径枚举（enumerate_paths）

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

- **最优先枚举（A\*）**：路径按"可信度 + 音节权重"从高到低产出。可信度就是最终排序用的那一个（每个简拼、
  模糊音各扣一次）；音节权重是该音节在词典里的占比取对数（`engine/src/syllable_frequency.h`，由
  `scripts/gen_syllable_frequency.py` 从词典生成）。每个声母会展开成它的全部音节（`womsh` 的 m、sh），
  以前的深度优先按字母序展开，256 条的上限花在 wo:ma:sa:ha… 上，根本到不了 wo:men:shi，于是 `womsh` 没有
  我们是、`zhegsh` 没有 这个是、`tabsh` 给 他把手；现在 men、shi 这些常见音节先出来。搜索对每个位置先算好
  "到终点还能拿到的最高分"做启发值，所以只展开即将被报出的那些路径的前缀，死胡同从不进入。
- **枚举上限**：`Syllabifier` 最多产出 256 条路径（`engine/src/syllabifier.cc` 的 `kMaxPaths = 256`），
  `PinyinTranslator` 只消费前 128 条（`pinyin_translator.cc` 的 `kMaxPaths = 128`）：全拼后面跟两个声母时
  读法有几百种，想要的那种（`womenjd` 的 jue:de）未必在前 64 条里。
- **儿化**：音节之后的 r 读成 儿（`huar` 花儿、`nar` 哪儿、`yihuir` 一会儿），当作打全的音节 er（可信度 0），
  查到的词在这个位置必须真是 儿（十二、华尔 不算）；r 做声母的读法（华人、纳入）照旧是简拼，于是
  `nar` 先给 那儿 / 哪儿，再给 纳入 / 那人。Top-N 索引为以 儿 结尾的词另外生成 r 结尾的 key。
- **末音节补全**：切分一次之后总是再切一次开着末音节补全（`enable_terminal_completion`，"ji" → jie、
  "buzhida" → bu:zhi:dao），只收尾音节是补全的那些路径，和 Top-N 索引对它收录的每个 key 都做补全保持一致：
  `buzhida` 给 不知道（补全组，在打全的 不知大 之下、更长的 不知大体 之上），`xianggan` 给 香港，`huanyin`
  给 欢迎。以前只在全拼路径查不到词时才补全，7 个字母以上的输入就少了这些候选。

### 1.3 路径过滤（has_prefix）

将每条路径的音节转为整数 ID 序列，查询 ID 索引（`id_index_`）：

```
["shui","dian","fei"] → [id(shui), id(dian), id(fei)]
```

二分查找 + 前缀匹配。只有词典中存在对应 N 元音节记录的路径保留（`QueryScratch::live_path_indices`）。不存在的路径被丢弃（如 `["sa","da","fa"]` ——不存在这个三字词）。

### 1.4 查词与打分（lookup_by_ids + sort）

对保留的每条路径，在二进制词典中查找所有匹配词条：

```
id_index_ 二分查找: "shui:dian:fei" → entries[117200..117500]
  扫出: 水电费(freq=117200), 税费点(freq=85000), ...
id_index_ 前缀匹配: "shui:dian" → entries[...]
  扫出: 水电(freq=150000), 水点(freq=90000), ...
```

同时查询用户词库（见[第 3 节](#3-用户词)），补充个性化候选。

每个候选按 Rime 的方式打分（`rank_fallback_candidate`，与 Top-N 索引同一公式，见
[短输入快速路径](short-input-fast-path.md#排序规则)）：先分组——音节全部对上（完整）> 最后一个音节靠补全、
或输入能切成完整音节却走了简拼（补全）> 覆盖全部输入的组句（见下）> 词比输入长（更长的词）；组内按
`ln(词频) + 路径可信度`（每个简拼音节 ln 0.5，补全的音节再加 ln 0.1 即共 ln 0.05，模糊音再加 ln 0.2，
结尾的 儿 打成 r 不扣分）。

- 词典里处在权重下限（100，导入的长尾词）的词，在路径用了声母时不做补全也不做更长的词：`nikanx`（x 只是声母）
  不会给 你看行不行，而 `biecuogu` 仍补全出 别错过。
- 既靠模糊音又靠声母才能覆盖输入的词（开了 n/l 模糊时 `nikanx` 的 李康勋）归入"更长的词"一组，
  排在按所打字母组出的句子（你看+下）之后。
- 词典几乎没见过用的单字（词频不超过 100：㝵、給、瘧、祋）排在所有词和句子之后，分数只取对数分的百分之一
  （`kRareCharacterScale`）：`de` 是 的 地 得 德 嘚 锝 然后才是 等、定额，而不是一页生僻字。
- 比输入长的词是联想，只留分数最高的 3 个（`kMaxExtensionCandidates`）：`bandaoti` 给 半导体、半导体产业、
  半导体激光器、半导体业，后面接前缀候选 搬到 / 半，而不是一整页 半导体×××。Rime 不设这个上限，但它的词典长词
  少得多；我们导入的长尾词会把页面填满。

所有路径的结果合并去重，按分数降序排列（`sort_candidates_by_score`：score desc → 文本长度 asc → 文本字典序）。

### 组句

整词候选不足一页时，`PinyinComposer` 在完整音节路径上把多个词拼成整句，做法同 Rime 的 Poet（不带语法模型）。
词典里没有覆盖全部输入的词（完整或补全）时，句子排在只是更长的词之前；和补全词（`wansha` 的 晚上）比，
Rime 按分数：常用词的 ln(词频) + ln 0.05 远高于两个以上词各扣 ln(1e-6) 的句子，所以句子排在补全组的底部
（`kSentenceBase`，45,000,000 起、补全词之下，组内按句子分数）。已有整词覆盖输入时（`mohei` 有 抹黑），
句子只是备选，排在所有词之后（`covered_sentence_frequency`），和 Rime 在有整词时不给句子一样：

- 句子分数 = Σ（ln(词频 / 1e8) + ln(1e-6)），每多一个词扣 ln(1e-6)，所以词越少、越常用越好；
- 每段音节取最多 3 个同音词，只取词频不低于该段最高者 10% 的（改/该 保留，瓦/哇 不要）；
- 歧义切点（两边合起来也是一个音节，如 `xia` 的 xi|a、`xian` 的 xi|an）不能做词的边界，跨过它的词
  （西安）扣 ln 0.1：`nikanxia` 不会出现"你看洗啊"；
- 只有一个词的"句子"不输出（词典候选已有）；
- 按分数输出至多 5 句：先取与上一句相差不超过 10% 的（相差阈值随句数收紧，同 Rime 的 MakeSentences），
  再给其他音节切法各留最好的一句（与最好的一句相差不超过 45%，如 `nikanxiane` 的 你看限额 / 你看下呢）。
  上文推荐在这些整句里挑首选（见[第 6 节](#6-上文推荐)）；
- 每段的同音词按词频从高到低存放，取够后即停止扫描，13 个音节以上的长句不会因扫描预算
  （`max_entry_scans = 2048`）用完而缺最后几段；ü 打成 v 的拼写（jv、lve）也参与组句；
- 最后一个音节还没打完（`…yizhengj`）时没有整句候选：第一个候选是已打完部分的整句（`…yizheng` 的组句），
  选中后剩下的字母接着打（`translate()` 的 leading sentence）。

长拼音（`laya.continuation`，默认开，至少 4 个音节）时，组句的词不只按词频打分：上文推荐模型的续写头给词图里
每个词的每个字打分，和词频分按 `laya.continuation_weight` 混合，整句由模型和词频一起决定。模型没加载或没有这个头时
退回上面只按词频的做法。

**全拼混着声母的输入**（`rangwolaibsmoxing`、`zhegemx`、`nikanx`）没有一条全拼音节路径，上面的组句不会
运行；只要没有一个打全的词覆盖全部输入——Rime 同样在有整词时不给句子——`compose_typed_spans`
直接在输入字母上组句。词是靠声母覆盖输入的（`womenjd` 的 我们决定）时句子照样组，但排在所有词之后
（`covered_sentence_frequency`），给上文推荐挑（我们+觉得）：

- 输入的每个子串 `[start, end)` 查一次 Top-N 索引（它的 key 本来就含简拼和混拼：`bs`、`mx`、`x`、`rangwolai`），
  子串恰好拼出的词（每个音节打全或只打声母）成为一条边，分数同 Poet：`ln(词频 / 1e8) + ln(1e-6)`，
  每个声母 ln 0.5；
- 声母后面明明接着打了这个音节的全拼时（`nikanx` 里把 看 读成 k），再扣 ln 0.05——Rime 的切分器会直接丢掉
  这种简拼边；单个声母只配一个单字（`wssb` 的 维生素 + 把）再扣 ln 0.1；
- 每段最多留 12 个词（声母段能配的词很多，上文推荐在整句之间挑），字母位置上做宽 16 的 beam，
  最多 6 个词；至少两个词（一个词覆盖全部输入就是词典候选本身）；只差最后一个单字的状态是单个声母的
  同音字（我们将+的、我们将+到、我们将+大…），同一个前缀只留两个，给其他读法（我们+觉得）留位置；
- 按分数输出至多 8 句（与最好的一句相差不超过 ln 100），和词典候选一起按分数排序。
  `rangwolaibsmoxing` → 让我来表示模型 / 让我来部署模型 …，`zhegemx` → 这个明显 / 这个梦想 / 这个模型 …，
  `nikanx` → 你看下 / 你看小 …；两个声母对应哪个词（`bs` 是 部署 还是 表示）只能靠词频和上文推荐，
  没有上文推荐时 `wssb` 仍是 卫生设备 在前。

**重复的短码**（`hhhh`、`nihh`）按每个字母一个字组出（`CompositionPathKind::kRepeatedShortCode`）：
字母读成哪个音节，看词典里它的叠词（三叠优先于两叠）谁最常见——h 是 哈（哈哈哈 9 万次，呵呵呵 100 次），
所以 `hhhhhhhhh` 是 哈哈哈哈哈哈哈哈哈，而不是单字更常见的 会。

### 1.5 示例：输入 `"sdf"`

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

系统词典在构建期已剥离纯符号与 emoji（见 [词典系统设计](dictionary.md)），默认排序只处理文字与混合文本候选；
符号候选统一由 `/` 符号输入提供，不参与这些排序。

## 2. 默认排序：五笔

五笔编码固定 1–4 码，无歧义切分，因此不走拼写图/路径枚举，直接以码查询：

```
WubiTranslator → Dict::lookup → WubiPrefixIndex::find(packed_code 二分)
               → 预排序 postings → 偏好/手动固定 → 去重 → 快照缓存 → 分页
```

1. **前缀索引定位**：`Dict::lookup(code)` 对 `wubi86.dict.idx`（CXWIDX v1）的 `packed_code` 做二分查找（O(log P)），
   定位到该编码前缀的 posting 区间。
2. **预排序 postings**：postings 是 `dict.bin` 词条索引数组，构建期已按 **精确匹配 → 码长升序 → 词频降序 → 码序 →
   文本长度 → 文本字典序** 排序并按文本去重。运行时线性读取（O(K)）即可得到默认顺序，不做实时评分。
3. **构建期审计**：五笔默认排序由 `wubi_ranking.py` 约束（如三码补全提升预算、前 10 候选集合不变、频率不倒退等），
   完整规则见 [五笔候选质量排序](wubi-candidate-ranking.md)。例如 `qdr` 的预排序 postings 里“然后”（词频高）
   被审计提升为首选，排在罕见的精确字“厃”之前。

`WubiTranslator::translate()` 使用快照缓存避免重复查询，快照同样跟踪偏好、手动固定和禁用词的版本。

## 3. 用户词

用户词典通过多路索引（exact / prefix / abbr / mixed）查询，与系统词典结果合并（拼音与五笔共用同一套用户词机制）：

| 查询方法 | 触发条件 | 使用的索引 |
|----------|---------|------|
| `UserLexicon::lookup_exact()` | `lookup_by_syllables()` 精确匹配 | `exact_index_` |
| `UserLexicon::lookup_prefix()` | `lookup()` 前缀匹配 | `prefix_index_`（前缀长度 ≤ 6）或 `code_sorted` 二分（前缀更长） |
| `UserLexicon::lookup_indexed()` | 短输入快速路径 | exact → prefix → abbr → mixed 四路桶 |

`lookup_prefix()` 根据 key 长度走不同的索引路径，两路都在收集后按 `score_match()` 评分降序排列，再根据
`max_user_scan` 截断：key 长度 ≤ 6 时通过 `prefix_index_` 哈希桶定位；更长时对 `code_sorted`（按 code 字典序排序的
`EntryId` 数组）做 `lower_bound` 二分定位，线性扫描前缀匹配条目。

用户词评分（`score_match()`，`engine/src/user_lexicon_query.cc`）按匹配类型分档：

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

| 档位 | 条件 | 行为 |
|------|------|------|
| `kPinyin` | 拼音模式（默认） | 完整分档：exact → pattern → nearPrefix → midPrefix → weakPrefix |
| `kWubi` | 五笔模式 | 前缀匹配一律 `kWeakPrefixBase`（4000），避免用户词干扰五笔码长精确匹配 |

档位由 `Dict` 在初始化时通过 `UserLexicon::set_scoring_profile()` 设置（`engine/src/dict.cc`）。用户词候选以
`candidate.origin = kUser` 标识，评分远高于系统词典前缀匹配，保证用户选过的词优先于系统未选过的同码词。
用户词查询遵守 `QueryBudget::max_user_scan`（默认 256）；`user_scan_count` 只反映命中 bucket 或二分范围内的实际
检查次数，不随用户词总量增长。

## 4. 学习偏好与手动固定

默认排序结果合并后，各翻译器按同一顺序处理：

```text
默认排序结果
  → apply_candidate_preferences()   （学习偏好，仅候选学习启用时）
  → sort_candidates_by_score()       （按最终分数降序）
  → apply_manual_candidate_order()   （手动固定，固定项前置）
```

- **拼音**（`pinyin_translator.cc`）：`lookup_indexed_fast()`（短输入快速路径）与标准管道各自生成默认排序结果后都走这三步。
- **五笔**（`wubi_translator.cc`）：`lookup_candidates()` 从 `Dict::lookup()` 取得结果后按相同顺序处理，再按 text 去重并标记 `source = kWubi`。
- **混输**（`mixed_translator.cc`）：先分别取得拼音与五笔两个有序列表，按混输策略（`auto` / `wubi`）合并去重；
  合并后收集所有手动固定项，按混输顺序整体前置（`manually_ordered()` 按完整身份判定）。合并策略（`choose_order()`）：
  四字母纯字母输入且拼音首选不显著强于五笔时五笔优先；长度 ≤ 3 时交错合并；其余拼音优先。
  `engine.mixed_candidate_preference = wubi` 强制五笔优先交错。

### 4.1 学习偏好

启用候选学习（`engine.candidate_learning`）后，用户选中的候选会记录为偏好，做法参照 Rime 的用户词典
（`engine/src/candidate_preference.cc`）：

- 每次上屏给该词记一次，并让它的权重（dee）加 1；所有词的权重每 200 次上屏衰减为 1/e（librime `algo::formula_d`）。
- **只选过一次**：只在本组内加分（`kLearnedBoost`，约词频 ×20）；权重衰减到 `kTentativeExpiry`（0.35）以下时遗忘，
  约 210 次其他上屏之后。
- **选过两次以上**：置顶在词典词之前，分数 `kPreferenceBaseScore (210000000) + 10000 + min(权重, 20000) × 1000`，
  权重高的在前；长期不用、权重衰减到 `kPinFloor` 以下时退回组内加分。
- 五笔没有词频可以在组内加，选过一次就置顶。
- 学习按音节而不是按打出来的字母生效：在 `xianzai` 下学过的 现在，打 `xz`、`xianz` 时同样命中（打全的排在只打
  声母的前面，简拼的可信度计入分数）。
- 上屏后紧接着按 Backspace 撤销这次学习；单条学习记录可在设置的词库管理里删除。
- 只影响排序，不进入查询索引，不写入用户词库；符号（`kSymbol`）候选不记录；逐段选出来或由组句上屏的拼音整句作为
  一条整句记在同一份偏好里，规则相同，置顶后和词按权重排。
- 学习偏好由服务端统一保存，`CandidatePreference::version()` 用于设置页与查询缓存的新鲜度判断。

详见 [用户词库与候选偏好](user-dictionary.md#候选偏好)。

### 4.2 手动固定

用户可对某个输入编码固定最多 16 个候选（`MANUAL_CANDIDATE_ORDER_MAX_ENTRIES = 16`），顺序完全由用户指定。
固定项按用户指定顺序插入候选列表头部，候选带 `pinned` 标记；未固定的候选保持原有相对顺序。

```cpp
// shared/include/cxxime/user_dict.h
struct ManualCandidateOrderEntry {
    std::string text;       // 候选文本
    std::string code;       // 候选编码
    std::string syllables;  // 冒号分隔音节（拼音），五笔可空串
};
```

- 以输入编码为键，每编码最多 16 条，全局最多 100,000 条（`kMaxEntries`）。
- 加载/保存时校验：文本、编码、音节格式合法；编码长度不超过该模式上限（拼音 64、五笔 4）；同一编码内不允许重复身份。
- 固定项在被系统词隐藏过滤、词典重载后仍按身份解析；无法解析的固定项被跳过。
- `version()` 为序列化内容的哈希（`version_token(serialize(...))`），任何修改都会使版本变化，用于设置页与 IPC 的并发控制。

**设置界面**：CxxIME 的设置程序在「词库管理」里提供候选排序编辑（固定置顶、上移下移、恢复默认等）。知意输入法的
设置程序只保留自学习开关与“清除学习记录”，没有这个页面；手动固定的数据格式、引擎逻辑与下面的 IPC 接口仍然保留，
`%USERPROFILE%\zhiyi\candidate_order_*.tsv` 中的固定项照常生效。

**IPC**：`lexicon_control.h` 为手动固定提供三个操作（`LexiconOperation`）：

| 操作 | 语义 |
|------|------|
| `kQueryCandidateOrder` | 查询某编码的候选列表 + 当前固定项 + 版本号 |
| `kSetCandidateOrder` | 写入固定项（携带 `expected_version`，乐观并发控制） |
| `kClearCandidateOrder` | 清空某编码的固定项（携带 `expected_version`） |

查询结果 `CandidateOrderQueryResult` 含 `input_code`、`version`、`has_more`、候选列表 `entries`（带 `reason` /
`available`，`available` 表示该候选是否仍可从词典解析）与当前固定项 `manual_entries`。

### 4.3 候选身份

手动固定与学习偏好都使用完整候选身份 `{text, code, syllables}` 而非仅文本：

- 同一文本可能对应多个编码（拼音 `srf` / `shurufa`，五笔重码），固定其一不应影响另一个；
- 拼音下同一文本可能存在不同音节切分，Top-N v3 保存规范音节正是为了提供一致的候选身份；
- 设置界面按身份展示排序原因，避免"同文词"互相覆盖。

### 4.4 持久化文件

文件位于 `%USERPROFILE%\zhiyi\`：

| 文件 | 内容 |
|------|------|
| `candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv` | 手动固定 |
| `learning_pinyin.tsv` / `learning_wubi.tsv` | 学习偏好 |
| `user_pinyin.tsv` / `user_wubi.tsv` | 用户词库（影响候选集合，不改变系统词排序） |
| `disabled_pinyin.tsv` / `disabled_wubi.tsv` | 系统词隐藏列表（过滤，不改变排序） |
| `pinyin.topn.bin` / `wubi86.dict.idx` | 随包默认排序（构建期生成，在安装目录） |

手动固定文件格式（`# cxxime-candidate-order format=1` 头 + 每行 5 列 TSV，`position` 从 1 开始）：

```text
# cxxime-candidate-order format=1
input_code<TAB>text<TAB>candidate_code<TAB>syllables<TAB>position
```

## 5. 全拼长词提前

拼出整个输入、但音节比第一个候选的字数多的词（`xian` 的 西安、`tian` 的 提案、`jie` 的 饥饿、`qie` 的 企鹅），
按词频会排在单音节读法的所有常用字之后（西安 曾在第 25 位）。和雾凇的 `long_word_filter` 一样，其中最常用的两个
放到第 4 位，留在第一页，上文推荐看得到它们、也可以再挪动（`promote_long_words`）：

- 只限全拼、打全的词（没有声母、没有模糊音），词频高于词典下限，输入不超过 12 个字母；
- 在排序之后、手动固定之后做：快速路径和标准管道一样，先手动固定的词，再长词；手动固定的词多于 3 个时放在它们之后。

## 6. 上文推荐

只作用于拼音和五笔的第一页（`engine.cc` 的 `translate_composition`，`laya_rerank.cc` 的 `LayaRerank::apply`）：

1. **取 2 页**：第一页先取 `laya_candidate_count()` 个候选（`laya.max_candidates`，0 即每页候选数的 2 倍）。
2. **只比覆盖整个输入的候选**：其中拼出全部输入的候选参与比较（显示 / 西安市、今天 / 今天是），只覆盖一部分输入的
   （`piaol` 的 飘）留在原位；**手动固定的词不参与，也不动**。可比较的候选少于 `laya.min_candidates`（2）时不调整。
3. **打分**：模型按光标前的文字（最多 `laya.context_chars` = 128 字）和所打拼音给这些候选打分，再加上：
   - 原来顺序的先验（`laya.rank_prior_weight`，中文默认 0）；
   - 词语 n-gram 的 ln P(候选 | 前文)（`laya.ngram_weight`）；有 n-gram 时光标前没有文字也照常推荐（`laya.ngram_without_context`）；
   - 候选是组出的长句（至少 `laya.sentence_min_chars` 个字）时，组句器的分数（`laya.sentence_score_weight`）。
   五笔候选不带拼音，也不用 n-gram。
4. **放到首位**：分数最高的候选挪到可比较候选的第一个位置（即手动固定的词之后），标上推荐（星标）；其余候选保持原顺序。
   勾选 `laya.exact_pinyin_first`（默认不勾，issue #34）时，候选里有每个音节都打全的词，就只在这些词里挑。
5. **长拼音的整句**：续写头自己写出的句子（`laya.model_sentences`）在明显好过首选时放到首位（`promote_model_sentence`），
   同样在手动固定的词之后。
6. **整句不超过 5 个**：多出来的组句候选挪到后面（`move_extra_sentences_back`），然后截回一页。

后面几页不再推荐：翻页时重新查询第一页（同样经过推荐），从上一页最后一个候选之后接着排，已经显示过的候选不会再出现。

## 7. 固定位置的候选

固定位置的候选在上面所有排序之后放进去（`engine/src/candidate_placement.h`），模型看不到也挪不动它们；翻页时重新
查询第一页也经过同样的步骤，所以每次结果一样。

| 来源 | 位置 | 优先级 |
|---|---|---|
| 快捷短语（[快捷短语](custom-phrases.md)） | 文件里写的位置（1–9） | 最高 |
| 内置日期时间（拼音读作日期 / 时间 / 星期） | 第 2 位起 | 次之 |
| 中文输入里的英文单词（`english.position_in_pinyin`，默认第 2 位；输入不是完整拼音时第 1 位） | 第 2 位 | 再次 |

规则：

1. 普通候选里和置位候选文字相同的去掉（只出现一次）；两个置位候选文字相同时留优先级高的。
2. 按优先级依次放：放到它的位置，被占了就往后找第一个空位；位置超过末尾就放在最后。
3. emoji 在置位之前插到它的词后面（`Engine::add_emoji_candidates`，`engine.emoji_candidates`），不会挤占置位候选的
   位置：拼音第一页里有 emoji 的词（`symbols.json` 的 `emoji_words`）后面紧跟它的 emoji，每个词最多 2 个、每页最多
   3 个，第一个候选总是文字。emoji 与它跟着的词消耗同样的拼音，来源是符号，选了不记学习。
4. 最后截到一页；被挤出去的普通候选出现在下一页。

拼音第一页什么都没查到（既不是拼音也不是英文单词）时，给出所打的字母本身。

## 8. 分页与缓存

有序候选列表按 `page_size` 切分返回（`CandidatePage{page_index, page_offset, page_size, total_count, highlighted}`），
高亮项固定为当前页第一个候选。分页不改变排序，只负责切片与展示。

### 查询页缓存

与短输入快速路径并行的缓存层，按「输入 + 页码 + 页大小」保存已计算出的候选页，`translate_page()` 开头先查此缓存，
未命中才进入快速路径或标准管道。标准管道返回页面前将结果存入 LRU 缓存，`candidate_offset` 也参与缓存键与命中判断：

```cpp
// pinyin_translator.cc — 返回前
if (!deadline_hit && !(trace && trace->deadline_exceeded))
    store_query_cache(pinyin, page_index, candidate_offset, page_size, versions, page);
```

| 属性 | 值 |
|------|-----|
| 触发条件 | 任意输入长度，只要输入与页参数命中已缓存条目 |
| 容量 | LRU 64 条（`kMaxQueryCacheEntries = 64`），`sequence` 递增序号实现淘汰 |
| 命中条件 | input + page_index + candidate_offset + page_size + 用户词/偏好（含整句）/手动排序/禁用词版本全匹配（`QueryCacheVersions`） |
| 失效 | 任一相关版本变化后全部缓存自动失效 |
| 非缓存场景 | deadline 命中或 deadline_exceeded 时不写入缓存，避免缓存过期/不完整结果 |

`lookup_query_cache()` 命中时设置 `trace->cache_hit = true` 并清零 trace 计数字段（exact/prefix/user_scan = 0），
语义与短输入快速路径的 cache hit 一致。

### Candidate 结构

```cpp
// shared/include/cxxime/candidate.h（节选）
struct Candidate {
    std::string text;                          // 候选词文本
    std::string comment;                       // 注释（五笔编码、拼音等）
    int frequency = 0;                         // 评分/频率（降序排列依据）
    CandidateSource source = kPinyin;           // 词典来源：kPinyin / kWubi
    std::string code;                          // 码表编码（如 "shurufa"）
    std::string syllables;                     // 音节序列（如 "shu:ru:fa"）
    CandidateOrigin origin = kSystem;           // 候选来源：kSystem / kUser / kLearned / kCache / kComposed
    bool pinned = false;                       // 手动固定
    bool recommended = false;                  // 上文推荐挑中（星标）
};
```

| 字段 | 说明 |
|------|------|
| `code` | 紧凑码表编码，系统词来自 `syllable_ids → compact_syllable_code()`，用户词存 `user_entry.code` |
| `syllables` | 冒号分隔的音节字符串（如 `"shu:ru:fa"`），系统词从二进制词典字符串表读取，用户词存 `user_entry.syllables` |
| `origin` | `kSystem`（系统词典）、`kUser`（用户词库）、`kLearned`（候选偏好命中）、`kCache`（查询页缓存命中）、`kComposed`（组句） |
| `source` | `kPinyin`（拼音模式查询）、`kWubi`（五笔模式查询） |

### 性能

拼音用 `query_bench` 实测（2026-11-06，一台台式机上 11-04 构建的引擎和词典，查询页缓存为冷、开组句，每个输入 200 次，
`query_bench --data data --cache cold --input ...`）：

| 输入 | 走哪条路 | 中位数 | p95 |
|---------|--------|------|------|
| `n`（1 字符） | Top-N 快速路径 | 0.03 ms | 0.05 ms |
| `ni`（2 字符） | Top-N 快速路径 | 0.04 ms | 0.06 ms |
| `sdf`（3 字符缩写） | Top-N 快速路径 | 0.24 ms | 0.34 ms |
| `shui`（4 字符全拼） | Top-N 快速路径 | 0.24 ms | 0.37 ms |
| `nihaoshijie`（11 字符全拼） | 标准管道（路径封顶 256） | 1.2 ms | 2.0 ms |
| `woxiangshuruyiduanhenchangdepinyin`（34 字符） | 标准管道 + 组句 | 2.9 ms | 3.6 ms |
| 五笔 1–4 码 | 前缀索引二分 + 读 postings，O(log P + K) | < 0.1 ms（未随此次重测） | |

以上不含上文推荐的模型推理，也不含 IPC 与候选窗绘制。查询管道支持 deadline 超时和扫描预算截断，通过 `QueryBudget`
结构传递，详见 [查询预算与候选收集](query-control.md)。

## 9. 边界与交互

- **系统词隐藏**：手动固定解析出的候选若在隐藏列表且非用户词，会被过滤，即使被固定。
- **数量截断**：固定项前置后超出 `limit` 的部分被截断，不改变未固定候选的相对顺序。
- **混输去重**：合并按文本去重，固定项优先保留混输策略偏好的来源（五笔优先时保留五笔身份）。
- **词典更新**：固定项按身份解析，词典重载后同身份候选仍可被固定；无法解析的固定项不产生候选。
- **上文推荐与手动固定**：模型只在固定项之后挑，挑中的放在固定项之后。
- **格式演进**：Top-N v3（CXTOPN v3）为 0.4 磁盘基线，候选身份字段（text + syllables）禁止重排，未来只追加字段。

## 10. 测试

- `disabled_system_lexicon_test`：固定/偏好与系统词隐藏的交互；
- `dictionary_format_test`、`short_cache_test`：Top-N v3 身份与候选写入；
- `user_data_separation_test`：手动固定与偏好的事务、版本与失效语义；
- `session_manager_status_test` / `session_manager_integration_test`：服务端加载与 IPC；
- `wubi_engine_test`：五笔混合排序与固定/偏好；
- `dictionary_symbol_policy_test` / `symbol_catalog_test` / `ordinary_candidate_test`：系统符号剥离与用户数据的纯符号过滤；
- `pinyin_topn_pipeline_test` / `wubi_prefix_index_test`：默认排序构建验证。
