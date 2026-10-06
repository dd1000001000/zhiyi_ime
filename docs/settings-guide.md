# 设置指南

知意输入法有两种设置方式：**设置程序**（`zhiyi-settings.exe`，日常使用）和**配置文件**
（`default.json`，手工编辑，见文末“配置文件”，部分取值只能在这里改）。另外，任务栏上的“中/英”
提供快速切换。

## 打开设置

- 右键任务栏上的“中/英”，选择“设置…”；或者从开始菜单打开“知意输入法设置”。
- 设置程序只会打开一个窗口：再次打开时会切换到已经打开的窗口。
- 底部按钮：**确定**（保存并关闭）、**取消**（不保存）、**应用**（保存但不关闭）。保存后立即生效。
- 从任务栏菜单改过的状态，切换到设置的另一页时会自动刷新显示。

## 任务栏“中/英”

- **左键单击**：切换中文 / 英文。
- **右键菜单**：中文 / 英文；全拼 / 首字母 / 五笔；英文单词联想；中文标点；全角；设置…；关于。
  打勾的项是当前状态，点一下即切换；全拼 / 首字母 / 五笔与英文单词联想的改动会保存下来。
- **鼠标悬停**：显示全部状态，例如“中文 · 全拼 · 半角 · 中文标点”。
- 开着大写锁定时图标显示 **A**。

## 常规

| 选项 | 说明 |
|------|------|
| 中文输入 | 拼音或五笔（86 版） |
| 拼音方式 | **全拼**：也能识别首字母简拼和混合输入（`wsyige` → 我是一个）；**首字母**：每个字母对应一个字（`zgr` → 中国人） |
| 主题 | 浅色或深色，候选窗口和设置程序一起切换 |
| 字号 | 小 / 中 / 大 |
| 候选词数 | 每页 3–10 个，默认 7 |
| 界面语言 | 跟随系统、简体中文或 English；设置程序与任务栏菜单使用这个语言 |
| 英文单词 · 拼写纠错 | 打错时在候选里给出正确拼写，第一个候选仍是原文，不会自动替换 |

## 模糊音

总开关加 7 组模糊音：z=zh、c=ch、s=sh、n=l（声母）和 an=ang、en=eng、in=ing（韵母）。新安装默认打开，
只勾选 en=eng、in=ing。模糊匹配出的词排在原音之后，并在后面用括号标出正确拼音，如 `zongguo` → 中国(zhong guo)。

## 按键

四个切换键：

| 切换键 | 默认 | 作用 |
|--------|------|------|
| 中英切换 | 轻按 Shift | 中文 / 英文 |
| 输入方式切换 | Ctrl + Space | 中文拼音：全拼 / 首字母；英文：单词联想 / 逐字母 |
| 中英文标点 | Ctrl + . | 中文标点 / 英文标点 |
| 全角/半角 | Shift + Space | 全角 / 半角 |

- **左键**点击按键框后按下新的按键（Esc 取消），**右键**点击清空（不使用这个切换键）。
- 中英切换可以是单独轻按 Shift 或 Ctrl，其他切换键需要组合键：Ctrl / Alt 组合、Shift + Space 或 F1–F11。
- 不能和其他切换键重复，也不能用复制、粘贴等常用快捷键；重复时保留原来的设置。
- 按键框下方的提示：灰色“已接管系统的××热键”表示和 Windows 输入法热键相同，由知意输入法处理；
  黄色“已被其他程序占用，可能无效”表示其他程序注册了这个全局热键。
- “恢复默认”把四个切换键恢复为上表的默认值（会先确认）。
- 配置文件中这四项无效（例如手工改出了重复的键）时，会自动恢复默认值。

## 词库

- **自学习**：选过的词会排到前面，逐字选出的新词会被记住；英文也会记住选过的纠正和常打的新词。
- **清除学习记录**：删除全部学习记录（会先确认，无法撤销）。

## 隐私

用户体验改进计划，两档都默认关闭，记录只保存在本机：

- **加入用户体验改进计划**：记录版本、设置和出错情况等基本信息，不含任何输入内容。
- **允许收集您的输入信息**：还会记录输入内容、使用统计和正在使用的程序；勾选时会先确认，并同时勾选上一项。

取消勾选后立即停止记录，已有记录保留；“删除所有记录”删除两档的全部记录，“打开日志文件夹”查看记录文件。
每项记录的具体内容见 [隐私说明](privacy.md)。

## 更新

- **有新版本时提醒我**（默认打开）：打开设置时检查 GitHub 上的最新版本，有新版本时弹出提醒
  （立即更新 / 稍后提醒 / 跳过这个版本）。检查只下载版本信息，不发送任何数据。
- **检查更新**：随时手动检查。
- **立即更新**：下载安装包（可取消，中断后继续下载），校验签名和 SHA-256 后启动安装程序（需要管理员确认），
  设置窗口随之关闭；安装完成后重新打开设置并显示“已更新”。已经打开的程序重新打开后才会用上新版本。

## 关于

版本号、许可证（GPL-3.0）、项目主页与上游项目 CxxIME 的链接。

---

## 配置文件

- 程序目录下的 `data\default.json`：出厂默认值。
- `%USERPROFILE%\zhiyi\default.json`：用户设置，覆盖同名字段。设置程序保存的就是这个文件，旧版本留下的未知字段
  （如已移除的 `status_window`）会被忽略。

修改用户配置文件后，后台服务会自动重新加载。常用字段：

| 字段 | 默认 | 说明 |
|------|------|------|
| `engine.input_mode` | 0 | 0 = 拼音，1 = 五笔 |
| `engine.pinyin_initials` | false | 首字母模式 |
| `engine.page_size` | 7 | 每页候选数（3–10） |
| `engine.candidate_learning` | true | 自学习 |
| `engine.fuzzy_pinyin` / `engine.fuzzy_groups` | true / `["en_eng","in_ing"]` | 模糊音开关与组合（`z_zh`、`c_ch`、`s_sh`、`n_l`、`an_ang`、`en_eng`、`in_ing`） |
| `engine.wubi_code_hint` | false | 五笔候选后显示剩余编码 |
| `initial_state.full_shape` / `chinese_punct` | false / true | 启动时的全角与中文标点状态 |
| `style.font_point` | 14 | 候选字号 |
| `style.layout` | horizontal | 候选横排（horizontal）或竖排（vertical） |
| `style.render_backend` | d2d | 候选窗口渲染：d2d 或 gdi |
| `theme` | moon_light | 候选窗口配色（`moon_light` / `moon_dark`；`themes.json` 中还有其他预设） |
| `ui.language` | auto | 界面语言：auto、zh-CN、en-US |
| `shortcuts.ascii_toggle` / `english_style` / `punct_toggle` / `shape_toggle` | 见“按键” | 切换键，如 `Ctrl+Space`；`disabled` 表示不用 |
| `ascii_composer.switch_key` | Shift 切换 | 单独轻按 Shift / Ctrl 的行为（`code`、`clear`、`set_ascii_mode`、`noop` 等，见 [组合输入](ascii-composer.md)） |
| `english.word_mode` | true | 英文模式默认单词联想 |
| `english.correction` | true | 英文拼写纠错 |
| `english.mixed_in_chinese` | true | 中文模式下提供完整的英文单词 |
| `laya.enable` / `laya.english` | true / true | 上文推荐（中文 / 英文） |
| `laya.context_chars` / `laya.english_context_chars` | 128 / 192 | 交给模型的上文长度（光标前的字符数） |
| `laya.threads` | 4 | 推理线程数 |
| `laya.cache` | true | 把模型权重按本机 CPU 打包后的副本和分词器的紧凑镜像存在 `%LOCALAPPDATA%\zhiyi\laya-cache`（首次启动生成，约 350 MB），以后启动直接映射，服务端少占约 270 MB 内存、少花约 0.8 s；false = 每次启动在内存里打包和解析，不写缓存 |
| `laya.max_candidates` | 0 | 模型查看的前几个候选（其中覆盖全部输入的参与比较）；0 = 每页候选数的 2 倍 |
| `laya.rank_prior_weight` / `laya.english_rank_prior_weight` | 0.4 / 0.2 | 引擎排位先验的权重（模型分数加上 权重 × log P(正确词排第 r 位)）；0 = 只看模型 |
| `privacy.experience_program` / `collect_input` | false / false | 用户体验改进计划两档 |
| `update.notify` | true | 打开设置时检查更新 |
| `diagnostics.trace_mode` | off | 开发诊断日志（off / error / normal / verbose），见 [可观测性设计](observability.md) |

### 设置程序的命令行参数

| 参数 | 说明 |
|------|------|
| `--panel dictionary` | 打开后直接显示“词库”页 |
| `--data <目录>` | 指定数据目录（开发调试用） |
