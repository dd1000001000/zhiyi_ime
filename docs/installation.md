# 安装、更新与卸载

面向维护者：安装程序做了什么、怎么卸载和收集诊断信息。普通用户的安装说明见 [README](../README.md)；
构建见 [开发说明](development.md)。安装包由维护者用不公开的打包工具（NSIS）生成，包含程序、Laya 模型、
ONNX Runtime、VC++ 运行时与运行时词典。

## 安装

安装程序的页面：选择语言（默认取已有设置的界面语言或 Windows 语言）→ 欢迎 → 许可协议 → 安装目录 →
用户体验改进计划（两档，默认不勾，见 [隐私说明](privacy.md)）→ 安装 → 完成。安装需要管理员权限，主要步骤：

1. 检查占用旧版本文件的应用（Restart Manager + 安装锁报告），停止后台服务
2. 将程序和出厂数据解压到同卷暂存目录（`<基目录>\update\`），再用 `Rename` 原子切换为新的版本目录
   `<基目录>\<版本号>.<8位十六进制>\`（默认基目录 `C:\Program Files\ZhiyiIME`）
3. 注册 TSF、写入安装信息与自启动项 `ZhiyiIMEServer`，写入所选界面语言与隐私选项，启动后台服务
4. 把 `zhiyi_ime_x64.ime` / `zhiyi_ime_x86.ime` 复制为系统模块 `%WINDIR%\Sysnative\zhiyi.ime` 与
   `%WINDIR%\SysWOW64\zhiyi.ime`（被占用时改为重启后替换）
5. 提交生命周期状态（`InstallLocation` 指向新版本，旧版本进入待清理列表），创建开始菜单快捷方式
   （知意输入法设置、Collect Diagnostics、卸载）

### 更新模式

设置程序“更新”页下载并校验安装包后，以 `/UPDATE` 参数运行安装程序：跳过语言、目录和隐私页面（沿用已有选择），
直接安装；完成后通过资源管理器以普通权限重新打开设置。只有还需要重新打开程序或重启时才显示完成页。
签名与下载的细节见 `update/include/cxxime/update.h`。

### 多版本安装

当前安装器采用**多版本布局**：每个版本独占一个版本目录，升级/降级不覆盖旧版本文件，避免新旧模块混装和文件占用导致的安装失败：

- 版本目录名由生命周期模块生成，格式为 `<版本号>.<8位十六进制>`（`installer/src/installer_lifecycle_state.cc` 的 `allocate_target()`，八位十六进制取自 `CoCreateGuid()`），同一版本重复安装会得到不同的目录，不会与已注册目录冲突；
- 首次安装：在基目录下分配新的版本目录，并写入 `%InstallBaseDir%\maintenance\install-state.json`；
- 已存在其他版本：新版本成为活动版本并写入注册表 `InstallLocation`，旧版本进入 `retired` 列表（状态文件中的 `retired`，NSIS 侧记为 `PreviousInstallLocation`）；
- 暂存与新版本目录切换：程序先解压到 `<基目录>\update\`，再用 `Rename` 原子切换到目标版本目录；
- 旧版本清理：无进程占用时在安装提交阶段删除；仍被占用时在安装完成页提示"部分应用仍使用上一版本，重新打开后即可切换"，并在后续安装或系统重启后清理。

安装器使用 `Global\ZhiyiIME.Installation` 命名互斥锁保证同一时间只有一个安装/卸载进程。切换程序目录前，安装器将旧程序状态、64 位和 32 位 TSF 模块的实际注册状态、系统 IMM 模块和安装注册表状态写入持久事务文件，不会根据 DLL 是否存在推断 TSF 是否已注册。TSF 注册、系统 IMM 模块复制或安装信息写入失败时，会按事务文件恢复原状态；安装提交成功后才删除事务数据和待清理版本。

如果安装进程异常终止，下次运行安装器会先停止服务端、检查暂存目录和备份目录的文件占用，再恢复未提交的安装或清理已经提交的备份。恢复未完成时不会继续覆盖文件。

覆盖安装会沿用注册表记录的活动版本目录。安装程序不会覆盖
`%USERPROFILE%\zhiyi\default.json` 和用户数据（用户词库、选词偏好与手动候选顺序）。

如果有应用正在使用知意输入法，安装器会列出 Restart Manager 检测到的进程，要求关闭后
重试。仅切换到其他输入法不能保证 TSF DLL 已从宿主进程卸载；Windows 系统进程仍占用文件时，
需要注销或重启后再运行安装器。安装阶段不使用重启后延迟覆盖，避免新旧模块混装。

安装完成后按 `Win+Space` 切换到知意输入法。已经打开的程序继续使用它们启动时加载的版本，重新打开后才用上新版本；若输入法列表里没有知意输入法，注销后重新登录。

## 卸载

- **推荐：** Windows 设置 → 应用 → 已安装的应用 → 知意输入法 → 卸载
- 或开始菜单 → 知意输入法 → 卸载
- 或控制面板 → 添加/删除程序 → 知意输入法

卸载流程（NSIS `Section Uninstall`）：释放输入处理器 → 停止服务端 → 检查文件占用 →
校验生命周期状态 → 创建卸载事务 → 准备系统 IMM 模块移除 → 反注册 TSF DLL →
删除注册表项 → 移除系统 IMM 模块 → 提交生命周期状态 → 删除暂存与 `maintenance` 残留 →
（勾选时）删除用户数据目录 → 删除快捷方式与基目录。

默认卸载只删除程序文件、开始菜单快捷方式、TSF 注册项、自启动项和卸载项。用户目录
`%USERPROFILE%\zhiyi\` 下的配置、用户词库、选词偏好与手动候选顺序会保留，便于重新安装或升级后继续使用；卸载向导提供"删除用户配置和词库数据"复选框，勾选后才会删除用户目录。

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

开始菜单提供 "知意输入法 → Collect Diagnostics" 入口。诊断导出不会修改系统状态，默认收集：

- 版本、系统、PowerShell、当前用户等环境信息
- 安装目录、出厂数据目录、用户目录、日志目录
- 关键程序文件和数据文件的大小、时间戳、SHA256
- 日志文件清单和 trace-summary.txt 近期错误/慢路径摘要
- 知意输入法注册表卸载项、TIP 注册项、键盘预加载状态
- `zhiyi-server.exe`、`zhiyi-settings.exe` 的运行状态，以及 `zhiyi_tsf_x64.dll` / `zhiyi_tsf_x86.dll` 与系统 `zhiyi.ime` 模块的文件信息

默认不会复制日志、用户配置或用户数据（词库与偏好）。需要进一步排查时，可在安装目录运行：

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -IncludeLogs
powershell -NoProfile -ExecutionPolicy Bypass -File collect_diagnostics.ps1 -IncludeUserConfig -IncludeUserDict -IncludeCandidatePreferences -IncludeDisabledSystemLexicon
```

注意：日志可能包含输入编码，用户数据包含个人词条与选词记录。对外反馈问题前应确认是否可以附带这些内容。

## 程序与数据目录

每个版本安装在独立的版本目录里，用户数据在 `%USERPROFILE%\zhiyi\`，跨版本共享，覆盖安装和更新都不会覆盖。
两处目录的完整结构见 [路径解析](path-resolution.md)。

## 命令行参数

### zhiyi-server.exe

```
zhiyi-server.exe --data "D:\MyData\知意输入法"    # 指定数据目录
zhiyi-server.exe --dict "D:\dict\pinyin.dict.bin"   # 指定词典路径
zhiyi-server.exe --config "D:\config.json"          # 指定配置文件
```

| 参数 | 说明 |
|------|------|
| `--data <dir>` | 数据根目录，覆盖默认路径 |
| `--dict <path>` | 词典文件完整路径（`.bin` 格式） |
| `--config <path>` | 配置文件完整路径 |

## 设置程序

从开始菜单的“知意输入法设置”或任务栏“中/英”的右键菜单打开 `zhiyi-settings.exe`，各页面与配置项见
[设置指南](settings-guide.md)。

## 常见问题

### 安装后输入法列表里找不到知意输入法

1. 注销并重新登录
2. 检查 `regsvr32` 是否成功：手动运行以下命令注册 x64 和 x86 两个架构的 DLL（路径为活动版本目录）：
   ```cmd
   regsvr32 "C:\Program Files\ZhiyiIME\<版本号>.<8位十六进制>\zhiyi_tsf_x64.dll"
   regsvr32 "C:\Program Files\ZhiyiIME\<版本号>.<8位十六进制>\zhiyi_tsf_x86.dll"
   ```
3. 在"设置 → 时间和语言 → 语言和区域 → 中文(简体)"中添加输入法

### 服务端启动后立即退出

通常是词典文件缺失。检查 `C:\Program Files\ZhiyiIME\<版本号>.<8位十六进制>\data\pinyin.dict.bin` 是否存在。若缺失，重新安装。配置文件里不要把 `engine.pinyin_scheme` 改成双拼方案：知意输入法不带双拼拼写表，服务端会启动失败。

### 切换输入法后打字无反应

先确认该程序是在安装或更新之后打开的（旧程序仍用旧版本）。Debug 构建的详细日志和 Release 构建的切换键日志（`[ZhiyiIME] switch:`）可以用 [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) 查看；也可以在配置文件中把 `diagnostics.trace_mode` 设为 `normal`，日志写在 `%USERPROFILE%\zhiyi\logs\`。

### 覆盖安装后配置丢失

覆盖安装不会删除用户词库、选词偏好与手动候选顺序文件。若 `default.json` 被覆盖，可通过配置编辑器重新修改。
