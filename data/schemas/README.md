# 拼音方案 schema 规则说明

拼音方案由 `data/schemas/` 下的 schema 文件描述，加载与校验见 `data/tools/generate_pinyin_spellings.py`：

- `pinyin.full-pinyin.schema.json`：全拼方案的拼写代数（spelling algebra）规则，规则语法与 librime schema 兼容；本 README 自「文件结构」起的逐条说明即针对该文件。
- `pinyin.<方案名>-shuangpin.schema.json`：微软（`microsoft`）、小鹤（`xiaohe`）、自然码（`ziranma`）、搜狗（`sogou`）、智能ABC（`abc`）五种双拼方案，各用 `initials` / `finals` / `zero_initials` / `final_aliases_by_initial` 等映射表描述，并声明 `passthrough_syllables`、`ignored_syllables`、`expected_collisions` 与 `fuzzy_groups`（模糊音组合，与全拼相同的名称和位）。

每份 schema 都带 `scheme_id` 与 `speller.type`，字段集必须与实现完全一致（多一个少一个字段都会被拒绝）。JSON 不支持注释，因此每条规则的语义统一在此文档中描述。

## 文件结构

```json
{
  "scheme_id": "full_pinyin",
  "speller": {
    "type": "full_pinyin",
    "algebra": [ "规则1", "规则2", ... ]
  }
}
```

- `scheme_id`：方案标识（`full_pinyin` / `microsoft_shuangpin` 等），必须是稳定的 snake_case。
- `speller.type`：`full_pinyin` 表示用 `speller.algebra` 逐条展开拼写，`shuangpin` 表示按映射表编码。
- `speller.algebra`：拼写代数（spelling algebra）规则数组，按**顺序**逐条应用于音节表。
- 每一条规则都是一个字符串，采用 librime 兼容的 `token/pattern/replacement/` 写法，以规则类型（token）开头，用第一个非小写字母字符（此处为 `/`）作为分隔符。

## 规则类型与行为

| 类型 | 行为 | 候选类型 | 可信度惩罚 |
| --- | --- | --- | --- |
| `abbrev` | 保留原拼写，并新增缩写形式 | 缩写（K_ABBREV） | -0.693 |
| `derive` | 保留原拼写，并新增派生形式 | 常规（K_NORMAL） | 0 |
| `fuzz` | 保留原拼写，并新增模糊音形式 | 模糊（K_FUZZY） | -0.693 |
| `xform` | 替换原拼写（不保留原形式） | 常规（K_NORMAL） | 0 |
| `erase` | 删除匹配的拼写 | - | - |
| `xlit` | 逐字符转写 | 常规（K_NORMAL） | 0 |

当前文件只使用了 `abbrev`、`derive`、`fuzz` 三种。

## 规则逐条说明

### 缩写规则（abbrev）

**1. `abbrev/^([a-z]).+$/$1/` — 单字母简拼**

- 匹配：以单个 `a-z` 字母开头、且长度大于 1 的完整拼音（如 `di`、`fang`）。
- 效果：新增首字母作为缩写候选，如 `d` 可匹配 `di`，`f` 可匹配 `fang`。
- 候选类型：缩写（K_ABBREV），可信度 -0.693。

**2. `abbrev/^([zcs]h).+$/$1/` — 双字母简拼**

- 匹配：以 `zh`、`ch`、`sh` 开头、且长度大于 2 的完整拼音（如 `zhang`、`chu`、`shui`）。
- 效果：新增 `zh`、`ch`、`sh` 作为缩写候选。
- 候选类型：缩写（K_ABBREV），可信度 -0.693。

### 派生规则（derive）

**3. `derive/^([nl])ve$/$1ue/` — nve/lve → nue/lue**

- 匹配：`nve`、`lve`。
- 效果：额外接受 `nue`、`lue`（例如 `nüe` 的另一种拼写）。

**4. `derive/^([jqxy])u/$1v/` — ju/qu/xu/yu → jv/qv/xv/yv**

- 匹配：`j`、`q`、`x`、`y` 后接 `u` 的拼音。
- 效果：额外接受 `jv`、`qv`、`xv`、`yv`（ü 的键盘替代写法）。

**5. `derive/un$/uen/` — un → uen**

- 匹配：以 `un` 结尾的拼音（如 `lun`）。
- 效果：额外接受 `uen` 形式（如 `luen`）。

**6. `derive/ui$/uei/` — ui → uei**

- 匹配：以 `ui` 结尾的拼音（如 `hui`）。
- 效果：额外接受 `uei` 形式（如 `huei`）。

**7. `derive/iu$/iou/` — iu → iou**

- 匹配：以 `iu` 结尾的拼音（如 `liu`）。
- 效果：额外接受 `iou` 形式（如 `liou`）。

**8. `derive/([aeiou])ng$/$1gn/` — 后鼻音尾变形**

- 匹配：以 `ang`、`eng`、`ing`、`ong` 结尾的拼音。
- 效果：额外接受把 `ng` 写成 `gn` 的形式（如 `ang`→`agn`、`fang`→`fagn`），兼容按键次序颠倒的输入。

### 模糊音规则（fuzz / 模糊派生）

**9. `derive/^([zcs])h/$1/` — zh↔z、ch↔c、sh↔s**

- 匹配：以 `zh`、`ch`、`sh` 开头的拼音。
- 效果：额外接受对应的平舌音形式（如 `zha`→`za`、`chang`→`cang`）。
- 说明：该规则以 `derive` 开头，但知意输入法的 `generate_pinyin_spellings.py` 会将该模式识别为模糊派生，候选类型为模糊（K_FUZZY），可信度 -0.693。

**10. `fuzz/^n(.*)/l$1/` — n ↔ l**

- 匹配：以 `n` 开头的拼音（如 `na`、`niu`）。
- 效果：额外接受对应 `l` 开头的模糊形式（如 `la`、`liu`）。
- 候选类型：模糊（K_FUZZY），可信度 -0.693。

**11. `derive/^(.*)eng$/$1en/` — eng ↔ en**

- 匹配：以 `eng` 结尾的拼音（如 `feng`、`geng`）。
- 效果：额外接受对应 `en` 结尾的形式（如 `fen`、`gen`）。
- 说明：与规则 9 相同，该模式被引擎识别为模糊派生（K_FUZZY，-0.693）。

**12. `fuzz/^l/r/`、`fuzz/^r/l/` — l ↔ r（`l_r`）**

- 效果：`l` 开头的拼音也可打成 `r`，反之亦然（如 `re`→`le`、`lou`→`rou`）。

**13. `fuzz/^f/h/`、`fuzz/^h/f/` — f ↔ h（`f_h`）**

- 效果：`f` 开头的拼音也可打成 `h`，反之亦然（如 `fei`→`hei`、`hu`→`fu`）。双拼里按原样输入的字母音节（`f`）没有对应的 `h` 编码，跳过。

**14. `fuzz/^wang$/huang/`、`fuzz/^huang$/wang/` — wang ↔ huang（`wang_huang`）**

- 效果：整音节互换，`wang` 与 `huang` 互通。

每组的 `part` 标出它改的是音节的哪一部分（`initial` 声母、`final` 韵母、`syllable` 整音节）。同一部分最多模糊
一次：一条规则不作用于已被同一部分的模糊组改过的拼写（n=l 与 l=r 同开时 n、r 不互通），声母组与韵母组可以叠加
（`zan` → zhang），整音节组不与任何组叠加。

## 应用规则

规则按数组顺序逐条作用：每条规则都会遍历当前的全部输入形式，命中时按该规则类型生成新候选并追加到拼写表中；`derive` 与 `fuzz` 均保留原形式。规则的先后顺序会影响最终候选集合，修改规则时请勿随意调整数组顺序。

`pinyin.full-pinyin.schema.json` 是全拼规则的唯一来源，修改后可直接供 `generate_pinyin_spellings.py` 及后续构建流程使用；微软双拼表的调整同样以 `pinyin.microsoft-shuangpin.schema.json` 为准。
