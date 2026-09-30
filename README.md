# CxxIME

[English](README_EN.md) | **中文**

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.15%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)
![Windows CI](https://img.shields.io/github/actions/workflow/status/deanxyuan/cxx-ime/windows-ci.yml?branch=master&label=Windows%20CI&style=flat-square)
![License](https://img.shields.io/github/license/deanxyuan/cxx-ime?style=flat-square)

> 轻量级 Windows TSF 输入法（拼音 / 五笔 / 混输）

CxxIME 是一款基于 Windows TSF（Text Services Framework）的轻量级输入法，支持拼音、五笔 86 和拼音五笔混输三种模式。客户端（TSF DLL）只负责按键捕获与候选呈现，拼音解析、词典查询和候选生成集中在独立服务端进程，所有输入会话共享同一份词典数据，引擎异常也不会拖垮正在输入的宿主应用。

## 特性

- 拼音、五笔 86 与混输三种模式，支持全拼、微软 / 小鹤 / 自然码 / 搜狗四种双拼、简拼、模糊音、动态组句与多段选词
- 候选按匹配质量分层排序：精确音节与接近完成的词不会被高频长词压过；长拼音可分段选择
- 五笔独立前缀索引，包含简码、补码提示、四码唯一候选自动上屏与第五码行为
- 系统符号与词典候选分离：输入 `\` 或分类助记码（如 `\bd`）按分类浏览与提交，设置中可浏览并复制符号
- 候选窗口支持横排 / 竖排、D2D 与 GDI 双渲染，内置 12 套配色主题（6 种色系 × 浅色 / 深色）
- 支持宿主通过 TSF UIElement 接管 inline preedit 与候选绘制（DOTA2 已验证）
- 选词学习默认关闭，开启后偏好独立持久化；用户词典、候选顺序与学习数据可在设置中分别管理
- 用户数据位于 `%USERPROFILE%\cxxime\`，卸载时默认保留，支持备份与合并导入

## 界面预览

候选窗口主题预览（6 种色系 × 浅色/深色）：

| 色系 | 浅色 | 深色 |
|------|------|------|
| 月白 | ![月白浅](docs/images/themes/moon_light.png) | ![月白深](docs/images/themes/moon_dark.png) |
| 晴空 | ![晴空浅](docs/images/themes/sky_light.png) | ![晴空深](docs/images/themes/sky_dark.png) |
| 新翠 | ![新翠浅](docs/images/themes/jade_light.png) | ![新翠深](docs/images/themes/jade_dark.png) |
| 琥珀 | ![琥珀浅](docs/images/themes/amber_light.png) | ![琥珀深](docs/images/themes/amber_dark.png) |
| 珊瑚 | ![珊瑚浅](docs/images/themes/coral_light.png) | ![珊瑚深](docs/images/themes/coral_dark.png) |
| 鸢尾 | ![鸢尾浅](docs/images/themes/iris_light.png) | ![鸢尾深](docs/images/themes/iris_dark.png) |

## 安装

1. 从 [Releases](https://github.com/deanxyuan/cxx-ime/releases) 下载 `cxxime-v<版本>-setup.exe`，按向导完成安装
2. 安装完成后**注销并重新登录**（TSF 文本服务需要重新登录才会被系统加载）
3. 通过 `Ctrl+Space` 或 `Win+Space` 切换到 CxxIME

每个版本独占一个版本目录，升级与降级不会覆盖旧版本文件；仍被宿主进程占用的旧版本文件会在应用退出后由后续安装或完整重启清理，安装过程不强制当场重启。卸载默认保留 `%USERPROFILE%\cxxime\` 下的配置、用户词库与学习数据。

安装、卸载与升级的详细说明见 [docs/installation.md](docs/installation.md)。

## 性能

词典和索引在服务端一次性载入内存；高频输入通过预构建索引查询，其他输入使用扫描预算、有界候选收集和查询截止时间控制延迟。

Release 历史基准中，Preedit IPC 平均时延往返约 `50 µs`，`nihao` 和 `nihaoshijie` 的查询 P50 分别不高于 `60 µs` 和约 `170 µs`。

测试结果随硬件和词典变化。完整数据及复现方案见 [docs/benchmark-data.md](docs/benchmark-data.md) 和 [docs/ipc-architecture.md](docs/ipc-architecture.md)。

## 配置

- 通过开始菜单打开 **CxxIME Settings** 图形化配置
- 或直接编辑用户配置文件 `%USERPROFILE%\cxxime\default.json`

所有配置项（输入模式、拼音方案、候选窗口、主题、词库管理、特殊符号、快捷键等）见 [docs/settings-guide.md](docs/settings-guide.md)。

## 词典

CxxIME 内置拼音与五笔 86 词库。拼音词典来自 [rime-ice](https://github.com/iDvel/rime-ice)（约 190 万词条，GPL-3.0-only），五笔词典来自 [KyleBing/rime-wubi86-jidian](https://github.com/KyleBing/rime-wubi86-jidian)（Apache-2.0）。词库来源与授权见 [data/README.md](data/README.md)，数据格式与生成维护流程见 [docs/dictionary.md](docs/dictionary.md)。

系统符号（emoji 与单独的标点、符号字符）在构建期就从词典中剥离，不参与拼音或五笔候选，由随包符号表统一提供，输入 `\` 按分类浏览与提交。

## 兼容性

- **Windows 10 / 11**：已完成日常使用与回归验证
- **Windows 7 及更早版本**：未验证，也不在支持范围内

## 从源码构建

```cmd
build.bat                                             # 开发构建（产物在 build\，启用工具与测试）
ctest --test-dir build -C Release --output-on-failure # 运行单元测试
python scripts\package.py                             # 生成可发布的安装包
```

环境要求：Windows 10/11、Visual Studio 2017 或更新版本（C++ 工作负载）、CMake 3.15+；打包另需 Python 3.10+ 与 [NSIS 3.x](https://nsis.sourceforge.io/)。安装包输出到 `..\output\cxxime-v<版本>-setup.exe`。

## 许可证

项目代码按 Apache License 2.0 发布。第三方组件与词典数据保留各自许可证，详见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。
