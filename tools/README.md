# CxxIME 开发工具

以下命令基于 `cxx-ime/` 目录，可直接拷贝执行。

前置：先编译。

```cmd
build.bat debug
```

---

## dict_query — 词典查询工具

拼音模式（需 `.bin` + `.spellings.bin`）：

```cmd
.\build\tools\dict_query\Debug\dict_query.exe --mode pinyin --dict .\data\pinyin.dict.bin --spellings .\data\pinyin.spellings.bin
```

五笔模式（需 `.bin`）：

```cmd
.\build\tools\dict_query\Debug\dict_query.exe --mode wubi --dict .\data\wubi86.dict.bin
```

| 输入 | 说明 |
|------|------|
| `sdf` | 查候选词 |
| `:s sdf` | 查看音节切分（仅拼音模式） |
| `:q` | 退出 |

---

## sqlite_query — SQLite .db 直读工具

```cmd
.\build\tools\sqlite_query\Debug\sqlite_query.exe .\data\wubi86.dict.db
```

| 输入 | 说明 |
|------|------|
| `sdf` | 按 code 前缀查词 |
| `:q` | 退出 |

---

## candidate_window_tool — 候选窗口可视化测试

```cmd
.\build\tools\candidate_window_tool\Debug\candidate_window_tool.exe
```

| 按键 | 说明 |
|------|------|
| `1-9` | 选择候选词 |
| `Space` | 提交高亮候选 |
| `Esc` | 隐藏候选窗口 |
| `PageUp/Down` | 翻页 |
| `T` | 循环切换主题（themes.json 全部预设） |
| `L` | 切换水平/垂直布局 |
| `D` | 切换 D2D/GDI 渲染后端 |
| `F` | 循环切换字体大小（12/14/16/18/20） |
| `P` | 切换 preedit 显示 |

窗口标题栏实时显示当前渲染器、主题、布局和字体大小。

---

## ipc_tool — IPC 测试工具

### 启动服务端

```cmd
.\build\server\Debug\cxxime-server.exe --dict .\data\pinyin.dict.bin --config .\data\default.json
```

### 启动客户端

```cmd
.\build\tools\ipc_test\Debug\ipc_tool.exe
```

### 命令列表

| 命令 | 说明 |
|------|------|
| `connect` | 连接服务端 |
| `disconnect` | 断开连接 |
| `status` | 显示连接状态 |
| `session start` | 创建会话 |
| `session end <id>` | 结束会话 |
| `key <vk> [mods]` | 发送按键（VK 十六进制） |
| `select <index>` | 选择候选（0-based） |
| `commit` | 提交组合文本 |
| `focus in/out` | 焦点事件 |
| `bench <n>` | 基准测试 n 次（自建 session） |
| `stress <n> [c]` | 压力测试 n 请求 c 并发 |
| `help` | 帮助 |
| `quit` | 退出 |

VK 参考：A-Z=`41`-`5A`、Space=`20`、Enter=`0D`、Backspace=`08`、Esc=`1B`
修饰键：Shift=1、Ctrl=2、Alt=4（可叠加）

### 输出字段

```
status=0 rtt=74us ascii=0 composing=1
  preedit: nihao
  candidates (5):
    1. 你好 <===
    2. 拟好
```

| 字段 | 说明 |
|------|------|
| `status` | 0=OK, 1=未知命令, 2=无效会话, 101=引擎处理失败 |
| `rtt` | 往返延迟（微秒） |
| `preedit` | 拼音缓冲区 |
| `candidates` | 候选列表，`<===` 为高亮项 |
| `commit` | 上屏文本 |

---

## tray_icon_tool — 托盘图标预览工具

将 ICO 文件放入真实系统托盘，与系统图标并排查看实际效果。

```cmd
.\build\tools\tray_icon_tool\Debug\tray_icon_tool.exe
```

启动后图标出现在系统托盘区域（右下角）。附带一个预览窗口展示所有变体。

| 按键 | 说明 |
|------|------|
| `1`/`2`/`3`/`4` | 切换版本组：v4 / v5 / v6 / legacy |
| `Tab` | 切换当前版本的下一个变体 |
| `Enter` | 从磁盘重新加载图标 |
| 左键点击托盘图标 | 循环切换变体 |
| 右键点击托盘图标 | 上下文菜单（选择变体/版本） |
| `Esc` | 退出 |

---

## tsf_position_tool — 候选窗口定位测试

```cmd
.\build\tools\tsf_position_tool\Debug\tsf_position_tool.exe
```

验证候选窗口跟随光标定位、屏幕边缘 clamp 与多显示器适配逻辑。

| 按键 | 说明 |
|------|------|
| 方向键 | 移动光标（每次 2px） |
| `Shift`+方向键 | 快速移动（每次 20px） |
| `L` | 切换水平/垂直布局 |
| `T` | 循环切换主题（themes.json 全部预设） |
| `D` | 切换 D2D/GDI 渲染后端 |
| `Space` | 刷新 |
| `1`-`7` | 选择候选词 |
| `Esc` | 隐藏候选窗口 |

---

## punct_test — 标点映射测试

```cmd
.\build\tools\punct_test\Debug\punct_test.exe
```

加载 `data/punctuation.json`，不经过 IPC 直接测试标点与全角转换逻辑（含成对与轮换标点状态）。

| 命令 | 说明 |
|------|------|
| `p` | 切换中文/英文标点 |
| `f` | 切换全角/半角 |
| `s` | 显示当前状态 |
| `r` | 重置成对/轮换状态 |
| `h` | 显示帮助 |
| `q` | 退出 |

其他输入逐字符处理，输出转换结果与 Unicode 码位。默认数据目录可用 `--data <dir>` 覆盖。

---

## query_bench — 查询性能基准

```cmd
.\build\tools\query_bench\Debug\query_bench.exe --data .\data --input s,sd,sdf,sddf,bj,srf,shrf,zguo,nihaoshijie --repeat 1000
```

| 参数 | 说明 |
|------|------|
| `--data <dir>` | 数据目录（必需） |
| `--input <list>` | 逗号分隔的输入串 |
| `--file <path>` | 输入文件（每行一个） |
| `--repeat <n>` | 重复次数（默认 1000） |
| `--warmup <n>` | 预热次数（默认 100） |
| `--page-size <n>` | 每页候选数（默认 7） |
| `--deadline-ms <n>` | 查询截止时间（默认 30ms） |
| `--mode <m>` | `final_key` / `full_typing` / `both` |
| `--cache <m>` | `warm` / `cold` / `both` |
| `--sentence-composition <m>` | `on` / `off` / `both` |
| `--trace-log` | 显示 should_log 触发率 |
| `--require-topn` | Top-N 缓存未加载则失败 |
| `--require-cache-hit` | 物化前缀缓存未命中则失败 |
| `--json <path>` | 输出 JSONL trace |
| `--help` | 帮助 |

基准回归脚本为 `scripts/check_query_bench.py`，详细说明见 `docs/observability.md`。

---

## topn_index — Top-N 索引工具

`topn_builder`：将 `scripts/build_pinyin_topn.py` 生成的中间文件转换为运行时索引：

```cmd
.\build\tools\topn_index\Release\topn_builder.exe --input .\data\pinyin.topn.bin --output .\data\pinyin.topn.bin --format dat16
```

| 参数 | 说明 |
|------|------|
| `--input <file>` | 中间文件（`build_pinyin_topn.py` 输出） |
| `--output <file>` | 输出运行时索引 |
| `--format <f>` | `flat16` / `dat16` / `dat8` |

`topn_benchmark`：不同索引格式的读取基准：

```cmd
.\build\tools\topn_index\Release\topn_benchmark.exe --baseline <intermediate> --dat16 <file> --queries 100000 --threads 4
```

参数：`--baseline`、`--flat16`、`--dat16`、`--dat8`、`--queries`、`--threads`。
