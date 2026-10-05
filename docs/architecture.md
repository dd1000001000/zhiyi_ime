# CxxIME 架构总览

描述 CxxIME 的总体架构、模块划分、技术选型与数据架构现状。面向维护者与开发者，专题细节见文末相关文档。

**关键指标（现状）：**

| 指标 | 数值 | 说明 |
|------|------|------|
| 安装包 | ~73 MB | 压缩后的单文件安装器，含全部词典数据 |
| Server 内存 | 工作集 ~58 MB、私有提交 ~503 MB | 私有提交以词典文件全量堆载为主，工作集随访问的词典页变化 |
| IPC 往返延迟 | < 1 ms | 实测 preedit 平均 ~50 µs（见 [IPC 架构设计](ipc-architecture.md)） |
| 启动 | 词典一次性读入 | 按顺序读盘，运行期不再有 mmap 换页 |

---

## 1. 项目定位

Windows TSF 输入法：拼音 / 五笔 86 / 混输三种模式，客户端（TSF DLL）/ 服务端（后台进程）分离，支持 Windows 10/11。以 TSF 输入处理器为主，同时提供 IMM 兼容模块（`cxxime_ime_<arch>.ime`）供传统应用使用。

**设计原则：**

1. **轻量依赖** — 第三方库仅 nlohmann/json（header-only）；SQLite 仅构建时使用；无 Boost
2. **客户端/服务端分离** — TSF DLL 只做按键捕获与展示，引擎与词典集中在服务端
3. **模块化** — 引擎层与 UI 层完全解耦
4. **TSF 为主、IMM 兼容** — 在 Windows 10/11 上验证；附带 IMM 兼容模块，覆盖仅支持 IMM 的传统应用

---

## 2. 总体架构

```
┌──────────────────────┐    Named Pipe (IOCP)    ┌───────────────────────────┐
│  TSF DLL (x64/x86)   │  ◄════════════════════► │     cxxime-server.exe     │
│  ┌────────────────┐  │                         │  ┌─────────────────────┐  │
│  │ KeyEventSink   │  │                         │  │ SharedResources     │  │
│  │ EditSession    │  │                         │  │ 词典/拼写/配置/标点   │  │
│  ├────────────────┤  │                         │  ├─────────────────────┤  │
│  │ IPC Client     │  │                         │  │ SessionManager      │  │
│  ├────────────────┤  │                         │  │ + GlobalVisibleState│  │
│  │ CandidateWindow│  │                         │  ├─────────────────────┤  │
│  │ StatusController│ │                         │  │ Engine (per session)│  │
│  │ LanguageBar    │  │                         │  ├─────────────────────┤  │
│  └────────────────┘  │                         │  │ DictionaryMonitor   │  │
└──────────────────────┘                         │  └─────────────────────┘  │
          ▲                                      └───────────────────────────┘
          │ IPC（按键 / 编辑会话 / 状态）                      ▲
┌──────────────────────┐                                     │
│ cxxime-settings.exe  │─────────────────────────────────────┘
│ 配置编辑 / 用户数据管理 │   控制通道（配置快照、用户配置与词库写入、备份）
└──────────────────────┘
```

**输入流程：** 按键 → TSF DLL → IPC → Server（SessionManager → Engine）→ IPC → TSF DLL → 上屏/候选窗口

**状态流程：** 可见状态（中英文/Caps/全半角/标点/模式）为服务端全局状态，经 IPC（GET_STATUS / 心跳）同步给各 TSF 客户端，驱动任务栏输入指示器（语言栏）。

---

## 3. 模块划分

### 3.1 Engine（输入引擎核心）

| 子模块 | 功能 | 实现 |
|--------|------|------|
| **Processor** | 拼音按键处理 | `PinyinProcessor` |
| **WubiProcessor** | 五笔按键处理（编码输入、Z 键通配、候选选择） | `WubiProcessor` |
| **Translator** | 拼音→汉字候选翻译（Syllabifier 主路径 + PinyinSegmentor 回退，长拼音查询页缓存） | `PinyinTranslator` |
| **WubiTranslator** | 五笔→汉字候选翻译（按编码精确/前缀查找） | `WubiTranslator` |
| **MixedTranslator** | 混合模式翻译（拼音+五笔交叉排序，三种排序策略） | `MixedTranslator` |
| **Segmentor** | 音节切分 | `Syllabifier`（BFS+DFS）+ `PinyinSegmentor`（回退） |
| **SpellingsIndex** | Patricia trie 拼写索引（缩写扩展，Prism 层） | `SpellingsIndex` |
| **AsciiComposer** | 可配置中英文切换，CapsLock overlay | `AsciiComposer` |
| **OutputComposer** | 输出合成（全角/CapsLock/按键拦截） | `OutputComposer` |
| **ShortCodeCache** | 短码候选缓存（DAT-16 Top-N 索引，Darts trie 查找，短输入快速路径） | `ShortCodeCache` |
| **Dict** | 词典加载与查询 | 二进制加载主词典 + 内存用户词库 / 候选偏好 / 手动候选顺序 |
| **Config** | 配置加载 | JSON（nlohmann/json） |

**数据存储：**
- **主词典：** 二进制堆加载词典 + Patricia trie 拼写索引（一次性读入），详见 [词典系统设计](dictionary.md)
- **用户数据：** 用户词库（手工词条，多路索引）、候选偏好（学习记录）与手动候选顺序，TSV 文件持久化，详见 [用户词库与候选偏好](user-dictionary.md)

### 3.2 TSF DLL（输入法前端）

实现的 TSF 接口：

```
ITfTextInputProcessorEx     — 输入处理器激活/停用
ITfKeyEventSink             — 按键事件接收
ITfCompositionSink          — 组合输入管理
ITfEditSession              — 编辑会话回调
ITfDisplayAttributeProvider — 显示属性（下划线等）
ITfThreadFocusSink          — 线程焦点通知
```

**关键流程：**

```
1. Windows TSF 加载 DLL → DllGetClassObject → ITfTextInputProcessorEx
2. 应用获焦 → ActivateEx() → 创建 IPC 连接
3. 用户按键 → OnKeyDown() → IPC 发送到 Server → 返回候选
4. 用户选词 → select_candidate → IPC 提交 → 插入文本到应用
5. 应用失焦 → Deactivate() → 断开 IPC
```

**注册方式：** `regsvr32` 注册 COM DLL，在 `HKLM\SOFTWARE\Microsoft\CTF\TIP\` 下注册输入处理器；x64 与 x86 DLL 分别注册，系统按进程位数加载。

### 3.3 Server（后台服务进程）

| 功能 | 说明 |
|------|------|
| 共享资源 | 词典/拼写索引/配置/标点映射启动时加载一次，所有 session 共享 |
| 会话管理 | 创建/销毁输入会话，per-session Engine 引用共享资源 |
| 全局可见状态 | GlobalVisibleState 保证跨窗口中英文/模式等状态一致 |
| IPC 服务 | 命名管道监听（IOCP），处理请求/响应 |
| 热重载 | 控制通道 `ConfigWriteCoordinator`（配置/词库写入）、DictionaryMonitor（manifest 轮询） |

### 3.4 UI（候选窗口）

- **渲染后端：** Direct2D + DirectWrite（默认），GDI 可选（`render_backend` 配置）
- **布局：** 横排（默认）/ 竖排，跟随光标定位，屏幕边缘 clamp，DPI 感知，圆角窗口
- **主题：** 12 套配色预设（`themes.json`，兼容 Weasel 配色格式）
- **状态显示：** 没有悬浮状态窗口；任务栏输入指示器显示中/英/大写，右键菜单切换各项状态

### 3.5 IPC 层

Named Pipe（每用户 `\\.\pipe\<username>\CxxIME`），Server 端 IOCP 线程池（2-4 worker），Client 端同步 I/O。固定结构体 + memcpy 序列化。

协议定义见 `shared/include/cxxime/ipc_protocol.h`（`IPCCommand` / `IPCRequest` / `IPCResponse` / `ImeStatus`），涵盖会话、按键、候选、状态切换、用户词典管理与重载命令。架构细节见 [IPC 架构设计](ipc-architecture.md)。

### 3.6 配置系统

JSON 配置（`default.json` + `themes.json`），设置编辑器（Win32 原生 GUI）修改后经控制通道（`ConfigWriteCoordinator`）写入，服务端发布新快照并在各 session 下次按键时热重载。配置项与界面说明详见 [设置指南](settings-guide.md)，中英文切换配置详见 [中英文切换机制](ascii-composer.md)。

---

## 4. 技术选型

| 技术领域 | 选型 | 理由 |
|----------|------|------|
| 引擎 | 自研（C++17） | 按需实现拼音/五笔，无需完整输入法框架 |
| 输入处理器 | TSF + IMM 兼容模块 | TSF 为主；`cxxime_ime_<arch>.ime` 覆盖传统 IMM 应用 |
| 序列化 | 固定结构体 + memcpy | 简单高效 |
| IPC | Named Pipe + IOCP | 零外部依赖，< 1ms 往返 |
| 词典 | 二进制堆加载 + DAT-16 Top-N 索引 | 一次性读入，Darts trie O(k) 查找，运行时无 SQLite |
| 配置 | nlohmann/json | header-only，轻量 |
| UI 渲染 | Direct2D/DirectWrite（默认）+ GDI（可选） | 高质量渲染，双后端可配置 |
| 日志 | CXXIME_LOG（自研 OutputDebugString 宏） | 零依赖 |
| 安装 | NSIS | 成熟的 Windows 安装方案 |
| 运行库 | 静态 VC++ 运行时 | 无 vcredist 依赖 |

### 依赖清单

| 依赖 | 用途 | 获取方式 |
|------|------|----------|
| Windows SDK | TSF/COM/Direct2D/GDI | 系统自带 |
| SQLite3 | 构建工具、sqlite_query 工具 | 源码编译（amalgamation，FTS5 + JSON1） |
| Darts-clone | Top-N 索引键查找（Double Array Trie） | 源码编译（bundled in third_party/） |
| nlohmann/json | 配置解析 | 头文件 only |
| Python 3.10+ | 词典数据工具 | 可选（仅构建词典时需要） |

---

## 5. 项目目录结构

```text
cxx-ime/
├── shared/          共享类型、IPC 协议、日志、数据路径解析
├── engine/          输入引擎：音节切分、翻译器（拼音/五笔/混输）、词典、配置
├── ipc/             命名管道 IPC 客户端/服务端（IOCP）
├── server/          后台服务进程（共享资源 + 会话管理 + 配置/词典热重载）
├── tsf/             TSF 文本服务 DLL（由 Windows 加载）
├── ui/              候选窗口（D2D / GDI 双后端渲染）
├── settings/        配置编辑器 GUI（Win32 原生控件）
├── docs/            项目文档（设计与实现、安装、配置指南）
├── data/            词典文件、Python 工具和默认配置
├── resource/        图标与资源 DLL 素材
├── scripts/         打包、词典准备、校验脚本
├── tools/           开发调试工具
├── test/            测试套件（C++ + Python）
└── third_party/     sqlite3, nlohmann/json, darts-clone
```

---

## 6. 词典数据架构

三层架构（详见 [词典系统设计](dictionary.md)）：

| 层 | 格式 | 用途 |
|----|------|------|
| **Spelling Algebra** | Python 构建时规则引擎（`pinyin.schema.json`） | 预计算缩写/模糊音变体 |
| **Prism**（SpellingsIndex） | Patricia trie 二进制堆加载 | 输入串→音节序列映射，前缀搜索 |
| **Table**（Dict） | 二进制堆加载（按音节 ID 序列索引） | 词条精确查询，二分查找 |

**SQLite 的角色：** 仅用于构建时源数据，运行时无 SQLite 依赖。

**词典来源：** rime-ice（雾凇拼音，~190 万词条）+ rime-wubi86-jidian（五笔 86）。

**主要二进制文件：**

| 文件 | 大小 | 说明 |
|------|------|------|
| `pinyin.dict.bin` | ~69.5 MB | 拼音主词典（按 syllable_ids 排序） |
| `pinyin.dict.idx` | ~46.2 MB | 拼音整数 ID 索引（音节→词条映射） |
| `pinyin.topn.bin` | ~291 MB | 拼音 Top-N 候选索引（CXTOPN v3 DAT-16，Darts trie 查找） |
| `pinyin.spellings.bin` | ~0.03 MB (30 KB) | Patricia trie 拼写索引 |
| `pinyin.reverse.idx` | ~7.3 MB | 拼音词语反查索引（Settings 反查） |
| `wubi86.dict.bin` | ~2.5 MB | 五笔主词典 |
| `wubi86.dict.idx` | ~2.3 MB | 五笔完整前缀索引 |
| `wubi86.reverse.idx` | ~0.3 MB | 五笔词语反查索引（Settings 反查） |

---

## 7. 相关文档

- [候选排序设计](candidate-ordering.md) — 手动固定 / 学习偏好 / 默认排序 / 分页的四层优先级
- [候选词选词算法](candidate-selection.md) — 查询管道与路径枚举
- [查询预算与候选收集](query-control.md) — QueryBudget、TopKCollector、扫描限制、超时检查点
- [中英文切换机制](ascii-composer.md) — AsciiComposer 配置与状态同步链路
- [词典系统设计](dictionary.md) — 三层架构、二进制格式、查询流程
- [用户词库与候选偏好](user-dictionary.md) — 手工词条与学习记录的独立存储与索引
- [IPC 架构设计](ipc-architecture.md) — IOCP 事件循环、管道安全、测试与性能基准
- [共享资源预加载](shared-resources.md) — 共享资源、全局状态与热重载
- [短输入快速路径](short-input-fast-path.md) — ShortCodeCache 与 topn.bin 缓存
- [可观测性设计](observability.md) — QueryTrace、TSF 事件追踪、日志、benchmark
- [安装与卸载](installation.md) — 构建、打包、注册
- [设置指南](settings-guide.md) — 配置项与设置界面
- [性能基准记录](benchmark-data.md) — 各阶段优化的历史数据
- [路径解析](path-resolution.md) — 数据目录、多版本安装布局与脚本路径规则
