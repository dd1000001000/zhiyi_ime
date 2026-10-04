# CxxIME 设置指南

## 概述

本文档覆盖 CxxIME 的两种配置方式：**设置窗口**（`cxxime-settings.exe`，面向日常使用）与**配置文件**（`default.json`，面向手工编辑，见文末「配置文件格式」——部分取值仅能通过配置文件设置）。

设置窗口采用 Win32 原生控件构建。窗口左侧为导航栏，右侧为对应设置面板，底部提供"确定"、"取消"、"应用"三个按钮。

- **确定**：保存配置并关闭窗口
- **取消**：不保存直接关闭
- **应用**：保存配置但不关闭窗口，方便继续调整

配置文件保存位置：`%USERPROFILE%\cxxime\default.json`

设置窗口启动后自动加载程序目录下的 `default.json`（默认值），再叠加用户目录下的 `default.json`（用户自定义值）。

---

## 面板一：输入

### 输入模式

**配置项**：`engine.input_mode`（0=拼音，1=五笔，2=混输）

| 选项 | 说明 |
|------|------|
| 拼音 | 全拼输入，如 `nihao` → 你好 |
| 五笔 | 86 版五笔字型输入 |
| 混输 | 拼音和五笔混合输入，自动识别编码类型 |

切换输入模式后，点"确定"或"应用"会立即通知服务端切换，无需重启输入法。

### 混输排序

**配置项**：`engine.mixed_candidate_preference`（`auto` / `wubi`，出厂配置 `auto`）

仅在混输模式下生效，控制拼音与五笔候选的混合顺序：

| 选项 | 说明 |
|------|------|
| 智能排序 | 拼音与五笔候选按综合评分混合（默认） |
| 五笔首选 | 混输时优先显示五笔候选 |

### 内联显示

**配置项**：`style.inline_preedit`（布尔值）

控制 TSF 内联文本是否写入应用文本区（如记事本）。

| 状态 | 行为 |
|------|------|
| 关闭（默认） | 应用文本区无内容。候选窗口显示拼音 + 候选列表（composition 模式） |
| 开启 | 应用文本区显示编码或首选候选。候选窗口仅显示候选列表（OFF 模式） |

### 显示内容

**配置项**：`style.preedit_type`（枚举：`composition` / `preview`）

仅在"内联显示"开启时生效。控制内联文本的内容：

| 选项 | 应用文本区显示 | 示例 |
|------|---------------|------|
| 编码 (ni'hao) | 原始拼音 | `ni'hao` |
| 首选 (你好) | 第一个候选词 | `你好` |

当"内联显示"关闭时，此选项不生效，但选择会被保存。重新开启"内联显示"后立即生效。

**行为矩阵**：

| 内联显示 | 显示内容 | 候选窗口 | 应用文本区 |
|----------|----------|----------|------------|
| 关 | 编码 | 上=拼音，下=候选 | 无 |
| 关 | 首选 | 上=拼音，下=候选 | 无 |
| 开 | 编码 | 仅候选列表 | `ni'hao` |
| 开 | 首选 | 仅候选列表 | `你好` |

### 候选窗显示光标

**配置项**：`style.show_preedit_cursor`（布尔值，出厂配置 `true`）

控制候选窗口中的编码区是否显示光标，指示当前输入位置。

### 模糊拼音

**配置项**：`engine.fuzzy_pinyin`（布尔值）

启用后，部分声母/韵母可互通，例如：

- `z`/`zh`、`c`/`ch`、`s`/`sh`
- `n`/`l`、`f`/`h`、`r`/`l`
- `an`/`ang`、`en`/`eng`、`in`/`ing`

适合发音不标准的用户。关闭后要求精确拼音。

### 候选数量

**配置项**：`engine.page_size`（整数，出厂配置 7）

每页显示的候选词数量，范围建议 5–15。设置后按翻页键可查看超出部分。

### 五笔设置

仅在五笔/混输模式下生效。

**四码自动上屏**：`engine.wubi_auto_commit`（布尔值，出厂配置 `true`）。输入四码且唯一候选时自动上屏，无需按空格。

**第五码首选上屏**：`engine.wubi_commit_first_on_fifth_key`（布尔值，出厂配置 `true`）。输入第五码时，若当前四码存在多个候选，先自动上屏首选候选再继续输入。

**显示最短补码**：`engine.wubi_code_hint`（布尔值，出厂配置 `false`）。输入未满四码时，在候选窗口中显示最短补码提示。

### 候选学习

**配置项**：`engine.candidate_learning`（布尔值，出厂配置 `false`）

控制选词后是否自动记录候选偏好。启用后选中的候选会作为独立的偏好数据保存（不写入用户词库），仅用于本地候选排序；可在"词库管理"面板的"选词偏好"页中查看和清空。候选排序的完整优先级（手动固定 > 学习偏好 > 随包默认排序 > 分页）见 [候选排序设计](candidate-ordering.md)。

### 初始状态

控制输入法启动时的默认状态：

| 配置项 | 说明 |
|--------|------|
| `initial_state.chinese_punct` | 中文标点模式（出厂配置 `true`，即默认使用中文标点） |
| `initial_state.full_shape` | 全角字符模式（出厂配置 `false`） |

---

## 面板二：候选窗口

### 主题

**配置项**：`theme`（字符串）

从 `themes.json` 中加载的预设配色方案。下拉框列出所有可用主题，选择后可通过"候选预览"行的"打开预览"按钮查看效果。

内置 12 个预设主题（`moon_light`、`moon_dark`、`sky_light`、`sky_dark`、`jade_light`、`jade_dark`、`amber_light`、`amber_dark`、`coral_light`、`coral_dark`、`iris_light`、`iris_dark`，默认 `moon_light`），每个主题定义以下颜色：

- 背景色、文字色、边框色
- 候选文字色、标签文字色
- 高亮候选文字色、高亮背景色
- 注释文字色、翻页按钮色

### 字体

**配置项**：`style.font_face`（字符串，出厂配置 `Microsoft YaHei UI`）

点击 `...` 按钮打开系统字体选择对话框。可同时修改字体名称和字号。

### 候选字号

**配置项**：`style.font_point`（整数，出厂配置 14，最小 8）

候选词的显示字号（磅值）。与字体选择器联动——在字体对话框中修改字号后自动同步。

### 预编辑字号

**配置项**：`layout.label_font_point`（整数，出厂配置 0）

候选窗口中拼音编码（preedit）和序号标签的字号。

- `0`：自动比候选字号小 2 磅
- 其他值：使用指定字号

### 布局方向

**配置项**：`style.layout`（字符串：`horizontal` / `vertical`）

| 选项 | 说明 |
|------|------|
| 横向 | 候选词水平排列，一行放满后换行（默认） |
| 纵向 | 候选词垂直排列，每行一个 |

### 状态窗口

**配置项**：`status_window.enable`（布尔值）

控制是否显示独立的状态窗口（中/英状态指示器）。关闭后仅通过语言栏图标指示状态。

### 视觉风格

通过下拉框快速调整候选窗口的整体外观，涵盖以下几个维度：

| 配置项 | 选项 | 说明 |
|--------|------|------|
| 内容密度 | 紧凑 / 标准 / 宽松 | 控制候选词之间的疏密程度 |
| 高亮区域 | 紧凑 / 标准 / 宽松 | 控制当前高亮候选的内边距 |
| 窗口圆角 | 直角 / 轻微 / 圆润 | 控制候选窗口整体圆角半径 |
| 窗口边框 | 无 / 细 / 明显 | 控制候选窗口边框粗细 |
| 窗口宽度 | 自动 / 限制 | 自动适应内容宽度或限制最大宽度 |

### 快捷操作

- **推荐布局**：应用预设的推荐布局参数
- **恢复默认**：将候选窗口外观恢复为出厂默认值
- **打开预览**：打开独立的候选窗口预览，实时反映当前设置效果

---

## 面板三：高级布局

该面板提供 13 个数值参数，精细控制候选窗口的尺寸和间距。点击"打开预览"按钮可打开独立预览窗口查看效果。

### 尺寸参数

| 参数 | 配置项 | 出厂值 | 说明 |
|------|--------|--------|------|
| 最小宽度 | `layout.min_width` | 160 | 候选窗口最小宽度（像素） |
| 最大宽度 | `layout.max_width` | 0 | 最大宽度，0 表示不限制 |
| 最大高度 | `layout.max_height` | 0 | 最大高度，0 表示不限制 |

### 边距和间距

| 参数 | 配置项 | 出厂值 | 说明 |
|------|--------|--------|------|
| 水平边距 | `layout.margin_x` | 12 | 窗口内容区左右内边距 |
| 垂直边距 | `layout.margin_y` | 12 | 窗口内容区上下内边距 |
| 预编辑间距 | `layout.spacing` | 10 | 编码区与候选区之间的间距 |
| 候选间距 | `layout.candidate_spacing` | 11 | 候选词之间的间距 |

### 高亮区域

| 参数 | 配置项 | 出厂值 | 说明 |
|------|--------|--------|------|
| 高亮横向留白 | `layout.hilite_padding_x` | 5 | 高亮矩形水平内边距 |
| 高亮纵向留白 | `layout.hilite_padding_y` | 2 | 高亮矩形垂直内边距 |
| 高亮内部间距 | `layout.hilite_spacing` | 4 | 高亮区域内标签与文字的间距 |
| 高亮圆角 | `layout.round_corner` | 4 | 高亮矩形的圆角半径 |

### 窗口外观

| 参数 | 配置项 | 出厂值 | 说明 |
|------|--------|--------|------|
| 窗口圆角 | `layout.round_corner_ex` | 4 | 候选窗口整体的圆角半径 |
| 边框宽度 | `layout.border_width` | 1 | 候选窗口边框宽度 |

### 渲染后端

**配置项**：`style.render_backend`（字符串：`d2d` / `gdi`）

| 选项 | 说明 |
|------|------|
| 默认渲染 (D2D) | Direct2D + DirectWrite 渲染，支持抗锯齿和 ClearType（默认） |
| 兼容渲染 (GDI) | 传统 GDI 渲染，兼容性更好，性能略低 |

---

## 面板四：快捷键

控制中英文模式切换的按键行为。每个按键独立配置。

### Shift 键行为

设置界面下拉提供 7 个选项：

| 选项 | 说明 |
|------|------|
| `inline_ascii` | 按 Shift 临时进入英文模式，松开后恢复中文。composing 时直接上屏已输入编码 |
| `code` | 提交编码原文并切换到英文模式 |
| `candidate` | 提交首个候选词并切换到英文模式 |
| `clear` | 清空输入缓冲区并切换到英文模式 |
| `set_ascii_mode` | 切换到英文模式，不提交内容 |
| `unset_ascii_mode` | 切换到中文模式，不提交内容 |
| `noop` | 不处理，仅作为修饰键 |

左右 Shift 键可分别配置不同行为。

### Control 键行为

可选项与 Shift 相同。左右 Control 键可分别配置。

### Caps Lock 行为

**配置项**：`ascii_composer.switch_key.Caps_Lock`

CapsLock 键在输入法中的行为模式。下拉框提供 5 个选项：

| 选项 | CapsLock 键 | CapsLock+字母 | 说明 |
|------|------------|---------------|------|
| `code` | 空闲：切换中/英；composing：提交编码原文+切英 | 直接上屏大写字母 | 保留已输入编码 |
| `candidate` | 空闲：切换中/英；composing：提交首个候选词+切英 | 直接上屏大写字母 | 智能提交 |
| `clear` | 空闲：切换中/英；composing：清空缓冲区+切英 | 直接上屏大写字母 | 传统行为 |
| `append` | 切换中/英（composing 时不做处理） | 追加到输入缓冲区 | 适合混合大小写输入 |
| `noop` | 不处理 | 直接上屏大写字母 | 保留系统 CapsLock 行为，输入法不做任何切换 |

**append 模式示例**：

1. 输入 `nihao` → 候选窗口出现
2. 按 CapsLock → 不清空、不切换
3. 输入 `SD` → 缓冲区变为 `nihaoSD`
4. 按空格 → `nihaoSD` 整体上屏

**CapsLock + Shift + 字母**：在所有模式下，CapsLock+Shift 抵消效果，输入小写字母。

> **注**：CapsLock 模式仅影响 A-Z 字母键。数字键、标点键在所有模式下行为一致。若直接编辑配置文件设为 `inline_ascii` / `set_ascii_mode` / `unset_ascii_mode`（与 CapsLock 的开关性质不兼容，设置界面不提供这些选项），引擎会自动降级为 `clear`。

### 自定义快捷键

除 Shift / Control / CapsLock 行为外，还可配置两个全局快捷键（出厂均关闭）：

| 配置项 | 说明 |
|--------|------|
| `shortcuts.input_mode_switch` | 切换输入模式（拼音 / 五笔 / 混输） |
| `shortcuts.activate_ime` | 切换到 CxxIME |

每个快捷键包含"启用"复选框与热键输入框。两个快捷键不能设置为相同组合；"切换到 CxxIME"仅接受可注册且未被其他程序占用的组合。

---

## 面板五：词库管理

### 词典选择

面板顶部提供"词条 / 候选排序 / 选词偏好"三个视图切换按钮与"拼音 / 五笔"下拉框。视图决定操作对象：

- **词条**：统一查询系统与用户来源的词条，管理用户词和系统词停用状态；
- **候选排序**：查看并固定某个编码的候选顺序（手动固定，优先级最高）；
- **选词偏好**：候选学习记录（自动偏好，优先级次之）。

切换后下方的查询结果、管理、导入导出和状态栏均针对当前视图与词典。

状态栏用于反馈操作结果（如"正在导入用户词库...""导入完成，正在刷新..."），不直接显示数据文件路径。

### 词典文件

| 文件 | 说明 |
|------|------|
| `pinyin.dict.bin` | 拼音二进制词典（堆加载格式，由 SQLite 源词典构建） |
| `wubi86.dict.bin` | 五笔 86 版二进制词典 |
| `wubi86.dict.idx` | 五笔完整前缀索引（构建期预排序，承载五笔默认候选顺序） |
| `pinyin.topn.bin` | 拼音 Top-N 短码索引（CXTOPN v3 DAT-16，承载拼音默认候选顺序） |
| `pinyin.reverse.idx` / `wubi86.reverse.idx` | 词语反查索引（Settings 拼音/五笔反查） |
| `user_pinyin.tsv` / `user_wubi.tsv` | 用户词库：手工添加的词条（TSV 格式，拼音和五笔各独立文件） |
| `learning_pinyin.tsv` / `learning_wubi.tsv` | 选词偏好：候选学习记录（TSV 格式，拼音和五笔各独立文件） |
| `candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv` | 候选排序：手动固定的候选顺序（TSV 格式，拼音和五笔各独立文件） |
| `disabled_pinyin.tsv` / `disabled_wubi.tsv` | 系统词隐藏列表（TSV 格式，每行一个被隐藏的系统词） |

### 管理词条

在"词条"视图下手动添加、保存或删除用户词，也可点击"隐藏系统词"/"恢复系统词"控制系统词条是否参与候选。操作后，面板顶部状态栏会即时反馈操作结果（如"已新增词条，列表已刷新""已保存修改，列表已刷新""已删除词条，列表已刷新"）。需要输入法服务端正在运行（通过 IPC 通信）。

切换至"选词偏好"视图后，编辑区隐藏，可点击"删除选中"删除单条偏好记录，或点击"清空全部偏好"按钮清除当前词典（拼音或五笔）的全部学习记录；偏好仅用于本机排序，不影响用户词库。

1. 在"词语"框输入要添加的词（如 `你好世界`）
2. 在"编码"框输入对应的编码（如 `nihaoshijie`）
3. 点击"新增"按钮

添加成功后词条立即生效，并原子写入对应的用户词库文件（`user_pinyin.tsv` / `user_wubi.tsv`）。如果服务端未运行，会弹出连接失败提示。

### 候选排序

"候选排序"视图用于把某个编码的候选固定为用户想要的顺序，优先级高于自动学习偏好（见 [候选排序设计](candidate-ordering.md)）。

1. 选择"拼音 / 五笔"，在"词语"框输入候选文本（如 `你好`），从候选编码列表中选择目标编码（如 `nihao`）；
2. 列表展示该编码下的全部候选，每行标注**排序原因**：`随包默认`（默认排序）、`用户词库`、`学习偏好`、`手动固定`；
3. 选中单个候选后可执行：
   - **固定置顶**：把该候选提到固定顺序第一位；
   - **追加固定**：把该候选追加到固定顺序末尾；
   - **上移 / 下移**：调整已固定候选的相对顺序；
   - **移除固定**：取消该候选的固定；
   - **恢复默认顺序**：清空该编码的全部固定项（需二次确认）。

每个编码最多固定 16 个候选。保存时携带读取到的版本号，若顺序已在其他位置被修改，会提示"候选顺序已在其他位置更新，请刷新后重试"并自动刷新列表。固定项持久化到 `candidate_order_pinyin.tsv` / `candidate_order_wubi.tsv`。

### 导入与导出

- **导入**（仅用户词库）：选择 `.tsv` 文件后在后台线程校验并导入，导入会整体替换当前用户词库。文件超过 64 MiB、内容含非法文本/编码/音节，或服务端未运行时会提示失败；成功后状态栏提示"导入完成，正在刷新..."。
- **导出**（仅用户词库）：先通过 IPC 请求服务端保存内存状态，然后复制到目标路径。若服务端保存失败，导出结果对话框会附加注意说明"服务未能立即保存最新内存状态"。

---

## 面板六：特殊符号

只读浏览随包符号表 `data/symbols.json`：下拉框选择分类（如「补充标点」），列表显示该分类的全部符号，状态栏提示对应的 `\` 助记码（如 `\bd`），点击「复制符号」把选中符号复制到剪贴板；符号表不可用时给出提示。

系统符号已在构建期从词典剥离，不在拼音或五笔候选中出现，输入法内统一通过 `\` 输入（见 [组合输入、内联 ASCII 与符号输入](ascii-composer.md)）。

## 面板七：诊断

- **启用诊断日志**：控制诊断日志开关（对应 `diagnostics.trace_mode`，勾选后从 `off` 切到 `normal`，取消勾选回到 `off`）。注意：诊断日志可能包含输入编码。
- **打开诊断日志目录**：打开日志目录（`%USERPROFILE%\cxxime\logs\`）。
- **导出诊断包**：点击后先提示"已开始导出诊断包，完成后会再次提示结果"，随后在后台等待 `collect_diagnostics.ps1` 脚本执行完成，完成后弹出结果对话框：
  - 脚本返回成功：提示"诊断包导出完成。请检查桌面的 cxxime-diagnostics-*.zip。"
  - 脚本返回失败：提示"诊断导出已结束，但脚本返回失败。请查看打开的 PowerShell 窗口输出。"

## 面板八：关于

显示知意输入法的版本信息和原项目链接：

- 版本号：取自仓库根目录的 `VERSION` 文件（如 `0.1.0-beta.7`）
- 许可证：GPL-3.0（基于 CxxIME 修改，CxxIME 为 Apache License 2.0）
- 原项目地址：https://github.com/deanxyuan/cxx-ime

---

## 配置文件格式

配置以 JSON 格式存储（嵌套结构），出厂配置示例：

```json
{
    "schema": {
        "name": "CxxIME",
        "version": "1.0",
        "description": "CxxIME default configuration"
    },
    "engine": {
        "page_size": 7,
        "max_pinyin_length": 64,
        "fuzzy_pinyin": true,
        "wubi_auto_commit": true,
        "wubi_commit_first_on_fifth_key": true,
        "wubi_code_hint": false,
        "candidate_learning": false,
        "mixed_candidate_preference": "auto"
    },
    "initial_state": {
        "full_shape": false,
        "chinese_punct": true
    },
    "style": {
        "font_face": "Microsoft YaHei UI",
        "font_point": 14,
        "layout": "horizontal",
        "render_backend": "d2d",
        "candidate_count": 7,
        "inline_preedit": false,
        "show_preedit_cursor": true,
        "preedit_type": "composition"
    },
    "layout": {
        "min_width": 160,
        "max_width": 0,
        "min_height": 0,
        "max_height": 0,
        "margin_x": 12,
        "margin_y": 12,
        "spacing": 10,
        "candidate_spacing": 11,
        "hilite_spacing": 4,
        "hilite_padding_x": 5,
        "hilite_padding_y": 2,
        "round_corner": 4,
        "round_corner_ex": 4,
        "label_font_point": 0,
        "border_width": 1
    },
    "ascii_composer": {
        "switch_key": {
            "Shift_L": "code",
            "Shift_R": "set_ascii_mode",
            "Control_L": "noop",
            "Control_R": "noop",
            "Caps_Lock": "clear"
        }
    },
    "shortcuts": {
        "input_mode_switch": "disabled",
        "activate_ime": "disabled"
    },
    "theme": "moon_light",
    "status_window": {
        "enable": true,
        "x": -1,
        "y": -1,
        "show_on_startup": true
    },
    "diagnostics": {
        "trace_mode": "off"
    }
}
```

> `diagnostics` 节还包含日志轮转与慢查询阈值等字段，详见 [可观测性设计](observability.md)。输入模式（`engine.input_mode`，0=拼音 1=五笔 2=混输）由服务端在切换时写回用户目录的配置文件持久化（程序目录可能只读）。

### 配置加载优先级

1. 程序目录 `default.json`（内置默认值）
2. `%USERPROFILE%\cxxime\default.json`（用户自定义，覆盖同名字段）

### 配置生效机制

点击"确定"或"应用"后：

1. 将当前面板的控件值写入内存中的 `Config` 对象
2. 保存到 `%USERPROFILE%\cxxime\default.json`
3. 如果修改了输入模式，通过 IPC 通知服务端立即切换
4. Server 端 `apply_config()` 重新加载引擎并同步输入模式，通过 control channel 广播配置快照至所有 TSF 客户端

---

## 命令行参数

```
cxxime-settings.exe [选项]
```

| 参数 | 说明 |
|------|------|
| `--panel dictionary` | 启动后直接跳转到"词库管理"面板 |
| `--data <目录>` | 指定数据目录（开发调试用），覆盖默认数据路径 |

---

## DPI 支持

设置窗口支持 Per-Monitor DPI 感知（`PROCESS_PER_MONITOR_DPI_AWARE`）。在高 DPI 显示器上：

- 所有控件尺寸按 DPI 比例缩放
- 字体大小自动适配
- 切换显示器时窗口自动调整布局（响应 `WM_DPICHANGED`）
