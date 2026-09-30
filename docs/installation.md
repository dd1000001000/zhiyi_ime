# 安装与卸载

## 构建与打包

### 环境要求

- Windows 10/11
- Visual Studio 2017 或更新版本（或 Build Tools），需要 C++ 桌面开发工作负载
- CMake 3.15+
- Python 3.10+（词典生成、测试和打包时需要；运行安装包不需要）

### 开发构建

```cmd
build.bat debug          # Debug 构建（自动设置 CXXIME_PRODUCTION_BUILD=OFF）
build.bat                # Release 构建
build.bat clean          # 清理构建目录
```

`build.bat` 自动传入 `-DCXXIME_PRODUCTION_BUILD=OFF`，`data_dir()` 返回源码目录下的 `data/`，可直接运行测试和开发联调。

### 打包分发

```cmd
scripts\package.py                    # Release 打包
scripts\package.py --debug            # Debug 打包
scripts\package.py --fast --skip-dict # 快速复用已有 dist\data 词典打包
scripts\package.py --host-diag        # 宿主诊断包
```

安装包输出到 `..\output\cxxime-v<version>-setup.exe`；诊断包文件名包含
`-host-diag` 后缀。`<version>` 取自仓库根目录的 `VERSION`。

默认包关闭宿主诊断，不包含 IME Host Probe、宿主跟踪导出脚本及其快捷方式。
`--host-diag` 同时启用产品内宿主诊断并打包这些工具。两种包都保留通用的
`collect_diagnostics.ps1`。

需要预先安装 [NSIS 3.x](https://nsis.sourceforge.io/)。`package.py` 执行：

1. 检查并触发构建（如未构建）
2. 复制 `cxxime_tsf_x64.dll`、`cxxime_tsf_x86.dll`、`cxxime-resources.dll`、`cxxime-server.exe`、`cxxime-settings.exe`、`collect_diagnostics.ps1`
3. 复制 `default.json`、`themes.json`、`settings_presets.json`、`punctuation.json`
4. 调用 `prepare_dictionary_bundle.py` 准备运行时词典（`.bin` / `.idx` / `.spellings.bin`（全拼与四种双拼各一份，共五份）/ `.topn.bin`）
5. 校验发布数据文件、CRT 依赖和热路径日志
6. 调用 `makensis.exe` 编译 NSIS 安装脚本
7. 输出 `..\output\cxxime-v<version>-setup.exe`

`package.py` 默认使用独立的 `build-package\` 构建目录，避免和 `build.bat` 使用的开发构建目录互相污染。 CMake 生成器默认交给 CMake 和当前命令行环境决定; 如需要显示指定，可使用：

```cmd
python scripts\package.py --generator "Visual Studio 17 2022" --platform x64
python scripts\package.py --output-dir output    # 自定义安装包输出目录（默认 ..\output）
```

## 安装

运行 `cxxime-v<version>-setup.exe`，按向导操作：

1. 许可协议
2. 选择安装基目录，默认 `C:\Program Files\CxxIME`；新版本会安装到 `<基目录>\<版本号>.<8位十六进制>\`
3. 检查占用旧版 CxxIME 文件的应用（Restart Manager + 安装锁报告）
4. 将新程序和出厂数据解压到同卷暂存目录（`<基目录>\update\`）
5. 用 `Rename` 把暂存目录原子切换为新的版本目录 `<基目录>\<版本号>.<8位十六进制>\`
6. 注册 TSF、启动服务端、准备系统 IMM 模块更新、写入安装标记
7. 提交生命周期状态（`InstallLocation` 指向新版本，旧版本进入待清理列表），清理事务与待清理版本
8. 把 `cxxime_ime_x64.ime` / `cxxime_ime_x86.ime` 复制为系统模块 `%WINDIR%\Sysnative\cxxime.ime` 与 `%WINDIR%\SysWOW64\cxxime.ime`（被占用时改为重启后替换）
9. 初始化用户配置目录 `%USERPROFILE%\cxxime\`（已有 `default.json` 时不覆盖），创建快捷方式

### 多版本安装

当前安装器采用**多版本布局**：每个版本独占一个版本目录，升级/降级不覆盖旧版本文件，避免新旧模块混装和文件占用导致的安装失败：

- 版本目录名由生命周期模块生成，格式为 `<版本号>.<8位十六进制>`（`installer/src/installer_lifecycle_state.cc` 的 `allocate_target()`，八位十六进制取自 `CoCreateGuid()`），同一版本重复安装会得到不同的目录，不会与已注册目录冲突；
- 首次安装：在基目录下分配新的版本目录，并写入 `%InstallBaseDir%\maintenance\install-state.json`；
- 已存在其他版本：新版本成为活动版本并写入注册表 `InstallLocation`，旧版本进入 `retired` 列表（状态文件中的 `retired`，NSIS 侧记为 `PreviousInstallLocation`）；
- 暂存与新版本目录切换：程序先解压到 `<基目录>\update\`，再用 `Rename` 原子切换到目标版本目录；
- 旧版本清理：无进程占用时在安装提交阶段删除；仍被占用时在安装完成页提示"部分应用仍使用上一版本，重新打开后即可切换"，并在后续安装或系统重启后清理。

安装器使用 `Global\CxxIME.Installation` 命名互斥锁保证同一时间只有一个安装/卸载进程。切换程序目录前，安装器将旧程序状态、64 位和 32 位 TSF 模块的实际注册状态、系统 IMM 模块和安装注册表状态写入持久事务文件，不会根据 DLL 是否存在推断 TSF 是否已注册。TSF 注册、系统 IMM 模块复制或安装信息写入失败时，会按事务文件恢复原状态；安装提交成功后才删除事务数据和待清理版本。

如果安装进程异常终止，下次运行安装器会先停止服务端、检查暂存目录和备份目录的文件占用，再恢复未提交的安装或清理已经提交的备份。恢复未完成时不会继续覆盖文件。

覆盖安装会沿用注册表记录的活动版本目录。安装程序不会覆盖
`%USERPROFILE%\cxxime\default.json` 和用户数据（用户词库、选词偏好与手动候选顺序）。

如果有应用正在使用 CxxIME，安装器会列出 Restart Manager 检测到的进程，要求关闭后
重试。仅切换到其他输入法不能保证 TSF DLL 已从宿主进程卸载；Windows 系统进程仍占用文件时，
需要注销或重启后再运行安装器。安装阶段不使用重启后延迟覆盖，避免新旧模块混装。

安装完成后**注销并重新登录**，输入法出现在系统输入法列表中。按 `Ctrl+Space` 或 `Win+Space` 切换。

## 卸载

- **推荐：** 开始菜单 → CxxIME → 卸载 CxxIME
- 或控制面板 → 添加/删除程序 → CxxIME

卸载流程（NSIS `Section Uninstall`）：释放输入处理器 → 停止服务端 → 检查文件占用 →
校验生命周期状态 → 创建卸载事务 → 准备系统 IMM 模块移除 → 反注册 TSF DLL →
删除注册表项 → 移除系统 IMM 模块 → 提交生命周期状态 → 删除暂存与 `maintenance` 残留 →
（勾选时）删除用户数据目录 → 删除快捷方式与基目录。

默认卸载只删除程序文件、开始菜单快捷方式、TSF 注册项、自启动项和卸载项。用户目录
`%USERPROFILE%\cxxime\` 下的配置、用户词库、选词偏好与手动候选顺序会保留，便于重新安装或升级后继续使用；卸载向导提供"删除用户配置和词库数据"复选框，勾选后才会删除用户目录。

多版本布局下，卸载活动版本的同时会清理生命周期状态、待清理版本与 `<基目录>\update\` 残留。若 TSF DLL 或系统 IME 模块仍被占用，卸载进入**延期卸载**流程：相关文件与注册表项带 `/REBOOTOK` 标记，重启后由系统完成删除；卸载中断后可再次运行卸载器继续处理。

卸载器只删除安装器拥有的文件；安装目录中无法识别的文件会保留。删除程序文件成功前
控制面板卸载项和 `uninstall.exe` 保持可用。卸载中断后可再次运行卸载器继续处理；删除程序文件
前发生错误时会按卸载前记录的 64 位和 32 位状态恢复 TSF 注册和系统 IMM 模块。
全部程序文件安全移入同卷暂存目录后，卸载事务进入提交阶段；此后即使卸载中断，再次运行也会
继续删除和清理注册表，不会尝试恢复已经永久删除的文件。

如果 TSF DLL 或其他程序文件仍被宿主进程占用，卸载器会列出相关应用并要求关闭后重试。
卸载不会修改当前用户的键盘布局预加载项，卸载后如果安装目录暂时残留，重启后应自动清理。

> **注意**：卸载完成后建议注销重新登录。如果 TSF DLL 被占用，重启后才能完成清理。

## 诊断包

设置窗口"关于"页提供"导出诊断包"按钮，开始菜单也提供 "CxxIME → Collect Diagnostics" 入口。诊断导出不会修改系统状态，默认收集：

- 版本、系统、PowerShell、当前用户等环境信息
- 安装目录、出厂数据目录、用户目录、日志目录
- 关键程序文件和数据文件的大小、时间戳、SHA256
- 日志文件清单和 trace-summary.txt 近期错误/慢路径摘要
- CxxIME 注册表卸载项、TIP 注册项、键盘预加载状态
- `cxxime-server.exe`、`cxxime-settings.exe` 的运行状态，以及 `cxxime_tsf_x64.dll` / `cxxime_tsf_x86.dll` 与系统 `cxxime.ime` 模块的文件信息

默认不会复制日志、用户配置或用户数据（词库与偏好）。需要进一步排查时，可在安装目录运行：

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -IncludeLogs
powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -IncludeUserConfig -IncludeUserDict -IncludeCandidatePreferences -IncludeDisabledSystemLexicon
```

注意：日志可能包含输入编码，用户数据包含个人词条与选词记录。对外反馈问题前应确认是否可以附带这些内容。

## 安装模式

当前 NSIS 安装器使用多版本程序目录安装模式：

| 类型 | 位置 | 说明 |
|---|---|---|
| 程序文件 | `C:\Program Files\CxxIME\<版本号>.<8位十六进制>\`（基目录可在安装向导中修改） | 每个版本自包含 `cxxime-server.exe`、`cxxime-settings.exe`、`cxxime_tsf_x64.dll`、`cxxime_tsf_x86.dll`、`cxxime_ime_x64.ime`、`cxxime_ime_x86.ime`、`cxxime-resources.dll`、`collect_diagnostics.ps1`、`uninstall.exe` |
| 出厂数据 | `<版本目录>\data\` | 出厂配置、主题、标点、符号、二进制词典、Top-N 索引及清单 |
| 用户数据 | `%USERPROFILE%\cxxime\` | 用户配置、主题覆盖、标点覆盖、用户词库、选词偏好、整句学习与手动候选顺序（跨版本共享） |

用户数据目录由安装器初始化，后续覆盖安装不会覆盖已有用户配置。

## 数据目录结构

### 程序安装目录

```
<安装基目录>\
├── <版本号>.<8位十六进制>\          活动版本（每个版本自包含以下结构）
│   ├── cxxime_tsf_x64.dll
│   ├── cxxime_tsf_x86.dll
│   ├── cxxime_ime_x64.ime / cxxime_ime_x86.ime
│   ├── cxxime-resources.dll
│   ├── cxxime-server.exe
│   ├── cxxime-settings.exe
│   ├── collect_diagnostics.ps1
│   ├── install-manifest.json
│   ├── license.txt
│   ├── THIRD_PARTY_NOTICES.txt
│   ├── licenses\
│   │   ├── miniz-MIT.txt
│   │   └── rime-ice-GPL-3.0.txt
│   ├── uninstall.exe
│   └── data\
│       ├── default.json
│       ├── dictionary_manifest.json
│       ├── settings_presets.json
│       ├── themes.json
│       ├── punctuation.json
│       ├── symbols.json
│       ├── pinyin.dict.bin
│       ├── pinyin.dict.idx
│       ├── pinyin.spellings.bin
│       ├── pinyin.microsoft-shuangpin.spellings.bin
│       ├── pinyin.xiaohe-shuangpin.spellings.bin
│       ├── pinyin.ziranma-shuangpin.spellings.bin
│       ├── pinyin.sogou-shuangpin.spellings.bin
│       ├── pinyin.topn.bin
│       ├── pinyin.reverse.idx
│       ├── wubi86.dict.bin
│       ├── wubi86.dict.idx
│       └── wubi86.reverse.idx
├── update\                       安装暂存目录
├── maintenance\
│   └── install-state.json        生命周期状态（active / prepared / retired）
└── .cxxime-*                     安装与卸载事务标记
```

### 用户数据目录

```
%USERPROFILE%\cxxime\
├── default.json
├── themes.json
├── punctuation.json
├── user_pinyin.tsv           (自动生成)
├── user_wubi.tsv             (自动生成)
├── learning_pinyin.tsv       (自动生成)
├── learning_wubi.tsv         (自动生成)
├── learning_composition.tsv  (自动生成，整句学习)
├── candidate_order_pinyin.tsv (自动生成)
├── candidate_order_wubi.tsv   (自动生成)
├── disabled_pinyin.tsv       (自动生成)
└── disabled_wubi.tsv         (自动生成)
```

## 命令行参数

### cxxime-server.exe

```
cxxime-server.exe --data "D:\MyData\CxxIME"    # 指定数据目录
cxxime-server.exe --dict "D:\dict\pinyin.dict.bin"   # 指定词典路径
cxxime-server.exe --config "D:\config.json"          # 指定配置文件
```

| 参数 | 说明 |
|------|------|
| `--data <dir>` | 数据根目录，覆盖默认路径 |
| `--dict <path>` | 词典文件完整路径（`.bin` 格式） |
| `--config <path>` | 配置文件完整路径 |

## 配置编辑器

运行开始菜单中的 "CxxIME 设置" 或直接启动 `cxxime-settings.exe`。

编辑器按面板组织（输入 / 界面 / 高级布局 / 快捷键 / 词库管理 / 特殊符号 / 备份与导入 / 故障排查 / 关于），各面板与配置项详见 [设置指南](settings-guide.md)。修改配置后点击"确定 / 应用"，配置立即写入用户目录并经服务端热重载生效。

## 常见问题

### 安装后输入法列表里找不到 CxxIME

1. 确认已注销并重新登录
2. 检查 `regsvr32` 是否成功：手动运行以下命令注册 x64 和 x86 两个架构的 DLL（路径为活动版本目录）：
   ```cmd
   regsvr32 "C:\Program Files\CxxIME\<版本号>.<8位十六进制>\cxxime_tsf_x64.dll"
   regsvr32 "C:\Program Files\CxxIME\<版本号>.<8位十六进制>\cxxime_tsf_x86.dll"
   ```
3. 在"设置 → 时间和语言 → 语言和区域 → 中文(简体)"中添加输入法

### 服务端启动后立即退出

通常是词典文件缺失。检查 `C:\Program Files\CxxIME\<版本号>.<8位十六进制>\data\pinyin.dict.bin` 是否存在。若缺失，重新运行 `scripts\package.py` 生成二进制词典后重新安装。配置里把拼音方案选为某个双拼方案时，还需要同目录下对应的 `pinyin.<方案名>-shuangpin.spellings.bin`（如 `pinyin.microsoft-shuangpin.spellings.bin`），缺失会导致服务端启动失败。

### 切换输入法后打字无反应

使用 [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) 查看日志输出（Debug 构建）。Release 构建不输出日志。

### 覆盖安装后配置丢失

覆盖安装不会删除用户词库、选词偏好与手动候选顺序文件。若 `default.json` 被覆盖，可通过配置编辑器重新修改。
