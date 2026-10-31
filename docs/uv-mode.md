# U 模式和 V 模式

和微软拼音一样（#41）：先打 `v` 输入中文数字、日期、时间和算式；先打 `u` 按笔画或部件输入不认识的字。

## 怎么进入

- **全拼**（包括首字母）：直接打 `u` 或 `v`。全拼里没有以 u、v 开头的音节，所以不冲突；`lv`、`nv` 里的 v 仍是 ü。
- **双拼和五笔**：u、v 是编码，改按 **Shift+U** / **Shift+V**。
- 只打 `u` 或 `v` 时出一个候选（字母本身），后面写着这个模式能做什么。
- Backspace 删掉开头的 u / v 就退出；Esc 取消；回车上屏打的字母原样。
- 开关在“设置 > 高级”（配置文件 `advanced.u_mode`、`advanced.v_mode`，默认都开）。

## V 模式

v 后面的数字和 `. + - * / ^ % ! ( ) , :` 都是输入，所以候选用 **a、b、c…** 选（空格选第一个）。
打代码（`vu…`、`vgb…`）或函数名时字母是输入，候选用空格选。

| 输入 | 候选 |
|---|---|
| `v123` | 一百二十三、壹佰贰拾叁、一二三、壹贰叁、一百二十三元、壹佰贰拾叁元整、CXXIII |
| `v123.45` | 一百二十三元四角五分、壹佰贰拾叁元肆角伍分、一百二十三点四五、壹佰贰拾叁点肆伍、一二三点四五、壹贰叁点肆伍 |
| `v12345.0` | 一万二千三百四十五元、壹万贰仟叁佰肆拾伍元整 …（整数金额） |
| `v2026.10.31`（`-`、`/` 也行） | 2026年10月31日、二〇二六年十月三十一日、贰零贰陆年壹拾月叁拾壹日、农历二〇二六年九月廿二、星期六 |
| `v10.31` | 10月31日、十月三十一日、壹拾月叁拾壹日，然后是数字的写法 |
| `v12:34` | 十二时三十四分、12时34分、十二分三十四秒、12分34秒（`v8:05:09` 带秒） |
| `v1+2*3` | 7、1+2*3=7 |
| `vu6d3e` | 派（Unicode 码位） |
| `vgbc5c9` | 派（GB 18030 编码） |

- 数字最多读 16 位（到“万亿”），更长的只出逐位的写法；金额最多到分，三位以上小数不出金额。
- 罗马数字只到 3999。
- 大写金额以“整”结尾（壹佰贰拾叁元整），和支票、发票的写法一致。
- 农历用 Windows 自带的 ICU 日历（Windows 10 1903 及以后），没有时不出农历。
- 算式：`+ - * / ^`、括号、`!`（阶乘）、`%`（百分之）；函数 `sqrt cbrt abs sin cos tan cot asin acos atan
  （arcsin…） ln log lg exp floor ceil round`，多个参数的 `sum avg max min var stdev pow log(底, x)`；常数 `pi`、`e`。
  三角函数按角度算（`sin(30)` = 0.5）；`var`、`stdev` 是样本方差、样本标准差。除以 0、算不出的不出结果。

## U 模式

候选后面写着读音（最多三个）。候选用数字选。

- **笔画**：`h` 横、`s` 竖、`p` 撇、`n` 捺（点）、`z` 折，按笔顺打前几笔即可：`uspnp` 出“少”，`uh` 出“一 十 二…”。
  笔画正好这样的字在前，其余按字频。同一个字的笔顺有几种写法时都认。
- **拆字**：按顺序打各部件的读音：`umumu` 出“林”“相”，`umumumu` 出“森”，`ushuishuishui` 出“淼”。
  偏旁按名称读：氵 shui、扌 shou、亻 ren、艹 cao、讠 yan、钅 jin、忄 xin、纟 si、阝 fu / er、刂 dao、礻 shi、
  衤 yi、犭 quan、辶 chuo、宀 mian、冖 mi、疒 bing、攵 wen、灬 huo 等。
- **混合**：部件也可以用它的前几笔代替：`uqips` 是“气”加一个以 丿丨 开头的部件（氚、氘）。部件之间可以用 `'` 隔开。
- 所有部件都打了的字在前，然后是读音打全的，最后按字频；打了前几个部件时也列出以它们开头的字。
- **符号**：`uu` 加缩写，和 `/` 菜单一样：`uudw` 单位、`uuxh` 序号、`uuts` 特殊符号、`uubd` 标点、`uusx` 数学、
  `uujh` 几何（特殊符号）、`uuzm` 字母（希腊字母）；`/` 菜单的其他缩写（`uujt` 箭头、`uubq` 表情 …）也能用。
  只打 `uu` 时列出各类，选一类进入 `/` 菜单。

## 和其他功能

- 选中的结果不记入学习（候选来源是符号）。
- 中文状态下打以 u、v 开头的英文单词（`very`、`user`、`update`）时，单词仍在第 2 位。

## 数据

U 模式的字表 `data/uv_chars.tsv` 由 `data/tools/build_uv_data.py` 生成：

- 笔画来自 [rime-stroke](https://github.com/rime/rime-stroke)（LGPL-3.0），约 11 万字；
- 拆字来自开放词典网的[汉语拆字字典](https://github.com/kfcd/chaizi)（CC BY 3.0），约 1.4 万字；
- 读音来自 [pinyin-data](https://github.com/mozillazg/pinyin-data)（MIT），偏旁名称由工具补上；
- 字频来自拼音词库的单字。

许可证见 `THIRD_PARTY_NOTICES.txt` 和 `data/licenses/`。

## 实现

- `engine/include/cxxime/uv_mode.h`：V 模式的转换（`v_mode.cc`）和 U 模式的字表与查询（`u_mode.cc`）。
- `engine/src/uv_mode_processor.cc`：按键；组合方式 `CompositionScheme::kUvMode`，输入以 u / v 开头。
- `engine/src/engine.cc`（`uv_mode_trigger`、`translate_uv_mode`）：进入模式、出候选、翻页。
- 候选用字母标号：`TranslationResult::letter_labels` → `PreeditPresentationFlag::LetterLabels` → 候选窗口。
- `settings/editor_advanced_panel.cc`：“高级”页。
