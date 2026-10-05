# 路径解析

知意输入法的数据文件分布在两个位置：安装目录（只读共享数据）和用户目录（可写用户数据）。

## 目录布局

### 安装基目录 `%ProgramFiles%\ZhiyiIME\`

当前分支采用**多版本安装**布局：安装基目录下每个版本独占一个子目录，升级/降级不再覆盖旧版本目录，而是安装到新版本目录并把新版本注册为活动版本；旧版本保留到系统重启清理。

```
C:\Program Files\ZhiyiIME\             安装基目录（首次安装时选择，默认 Program Files）
├── <版本号>.<8位十六进制>\         活动版本目录（如 0.7.6.77048fc8\），每个版本自包含完整程序与 data\
│   ├── zhiyi_tsf_x64.dll / zhiyi_tsf_x86.dll
│   ├── zhiyi_ime_x64.ime / zhiyi_ime_x86.ime
│   ├── zhiyi-server.exe / zhiyi-settings.exe
│   ├── zhiyi-resources.dll
│   ├── onnxruntime.dll / onnxruntime_providers_shared.dll   Laya 推理
│   ├── vcruntime140*.dll / msvcp140*.dll                    VC++ 运行时（随程序放置）
│   ├── collect_diagnostics.ps1
│   ├── uninstall.exe
│   ├── laya\                       Laya 模型（laya.int8g.onnx、tokenizer、配置）
│   └── data\
│       ├── default.json            默认配置
│       ├── themes.json             颜色主题（12 套）
│       ├── settings_presets.json / punctuation.json / symbols.json
│       ├── dictionary_manifest.json 词典清单
│       ├── pinyin.dict.bin / pinyin.dict.idx / pinyin.spellings.bin
│       ├── english.words.tsv       英文词表
│       ├── ui.zh-CN.json / ui.en-US.json  设置程序的界面文字
│       ├── pinyin.topn.bin         拼音 Top-N 短码索引（CXTOPN v4）
│       ├── wubi86.dict.bin / wubi86.dict.idx
│       └── pinyin.reverse.idx / wubi86.reverse.idx
├── maintenance\install-state.json  生命周期状态（活动 / 待清理版本）
├── update\                         安装暂存目录（stage）
├── .zhiyi-backup\                 安装备份/回滚目录
└── .zhiyi-install-* / .zhiyi-runtime / .zhiyi-ime-*.pending  安装事务与系统 IME 更新标记
```

同一个版本升级时使用 `<version>.next` 临时目录完成替换，避免与已注册的活动版本目录冲突。安装状态通过注册表记录：

| 注册表值（`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyiIME`） | 含义 |
|------|------|
| `InstallLocation` | 活动版本目录（当前生效的程序 + data） |
| `InstallBaseLocation` | 安装基目录 |
| `PreviousInstallLocation` | 待清理的上一版本目录（存在时阻塞新的安装） |

`data_dir()` 与活动版本无关——每个版本目录内的 exe 都按自身目录解析 `data\`，登录自启动的 `ZhiyiIMEServer` 指向当前活动版本的 `zhiyi-server.exe`。

### 用户目录 `%USERPROFILE%\zhiyi\`

```
C:\Users\<username>\zhiyi\
├── default.json              用户配置覆盖（可选）
├── themes.json               用户主题覆盖（可选）
├── punctuation.json          标点映射覆盖（可选）
├── user_pinyin.tsv           用户词库（拼音）
├── user_wubi.tsv             用户词库（五笔）
├── learning_pinyin.tsv       选词偏好（拼音）
├── learning_wubi.tsv         选词偏好（五笔）
├── candidate_order_pinyin.tsv 手动候选顺序（拼音）
├── candidate_order_wubi.tsv   手动候选顺序（五笔）
├── disabled_pinyin.tsv       系统词隐藏列表（拼音）
├── disabled_wubi.tsv         系统词隐藏列表（五笔）
├── learning_composition.tsv  整句学习
├── learning_english.json     英文学习（选过的纠正、常打的新词）
├── logs\                     诊断日志与用户体验改进计划记录（见 docs/privacy.md）
├── updates\                  下载的更新安装包
└── update-state.json         跳过的版本与正在安装的版本
```

以上文件都在用到时才创建。

用户目录跨版本共享，由 `user_data_dir()` 首次调用时自动创建；多版本并存时用户数据不随版本切换而改变。

## 路径解析函数

声明在 `shared/include/cxxime/data_path.h`，实现在 `shared/src/data_path.cc`。

### data_dir()

共享数据目录，只读。回退链：

1. **运行时覆盖** — `set_data_dir()` 设置的路径（server 的 `--data` 参数）
2. **编译时常量** — `CXXIME_DATA_DIR` 宏（开发构建由 CMake 定义）
3. **生产默认** — `<exe_dir>\data\`（GetModuleFileNameW 推导）

```
server --data "D:\custom\data"  →  优先级 1
cmake -DCXXIME_PRODUCTION_BUILD=OFF  →  优先级 2
NSIS 安装到 Program Files  →  优先级 3
```

### user_data_dir()

用户可写目录。固定解析为 `%USERPROFILE%\zhiyi\`（CSIDL_PROFILE + `\zhiyi\`）。

首次调用时通过 `CreateDirectoryW` 自动创建。

### data_path() / user_data_path()

便捷拼接：

```cpp
cxxime::data_path("pinyin.dict.bin")       // → data_dir() + "pinyin.dict.bin"
cxxime::user_data_path("user_pinyin.tsv")  // → user_data_dir() + "user_pinyin.tsv"
```

### set_data_dir()

运行时覆盖 `data_dir()` 的返回值。自动补尾 `\`。

```cpp
cxxime::set_data_dir("D:\\mydata");   // data_dir() → "D:\\mydata\\"
cxxime::set_data_dir("");             // 清除覆盖，恢复默认回退链
```

### 拼音方案的拼写表路径

知意输入法只安装全拼拼写表（`pinyin.spellings.bin`），下面的双拼规则沿用自 CxxIME，仅在恢复双拼时有效。

引擎按主词典路径推导拼写表：默认取同目录下的 `pinyin.spellings.bin`（全拼）；当配置的拼音方案是内置双拼方案
（`microsoft_shuangpin`、`xiaohe_shuangpin`、`ziranma_shuangpin`、`sogou_shuangpin`）时，改取同目录下的
`pinyin.<方案名>-shuangpin.spellings.bin`（即 `PinyinSchemeDescriptor.spelling_filename`）。五份拼写表共用同一个
`pinyin.dict.bin`，切换方案只更换拼写表与该方案的音节切分，不重新加载主词典。

## 配置加载顺序

server 和 settings 按以下顺序加载配置，后者覆盖前者：

```cpp
config.load(data_path("default.json"));         // 安装目录默认配置
config.load(user_data_path("default.json"));    // 用户目录覆盖
config.load_themes(data_path("themes.json"));   // 主题（仅从安装目录）
```

## CMake 宏

| 宏 | 定义位置 | 用途 |
|----|----------|------|
| `CXXIME_DATA_DIR` | 顶层 CMakeLists.txt（非 production）<br>test/CMakeLists.txt（测试） | `data_dir()` 编译时回退值 |
| `CXXIME_PROJECT_DIR` | test/CMakeLists.txt | 测试中拼接项目根路径 |

### 生产构建

```cmd
cmake -DCXXIME_PRODUCTION_BUILD=ON ...
```

不定义 `CXXIME_DATA_DIR`。`data_dir()` 走 `<exe_dir>\data\`。

### 开发构建

```cmd
cmake -DCXXIME_PRODUCTION_BUILD=OFF ...
```

定义 `CXXIME_DATA_DIR="${CMAKE_SOURCE_DIR}/data/"`。`data_dir()` 直接返回源码 data/ 目录。

### 测试

test/CMakeLists.txt 为每个测试目标定义：

```cmake
target_compile_definitions(${name} PRIVATE
    CXXIME_PROJECT_DIR="${CMAKE_SOURCE_DIR}/"
    CXXIME_DATA_DIR="${CMAKE_SOURCE_DIR}/data/"
)
```

测试代码用 `project_path()` 拼接项目根路径：

```cpp
static std::string project_path(const char* rel) {
    return std::string(CXXIME_PROJECT_DIR) + rel;
}
// project_path("data/pinyin.dict.bin")  → D:/gitee/cxx-ime/data/pinyin.dict.bin
// project_path("data/default.json")     → D:/gitee/cxx-ime/data/default.json
```

## Python 脚本路径

Python 脚本分布在两个目录，职责不同：

| 目录 | 定位 | 脚本 |
|------|------|------|
| `scripts/` | **主入口脚本**：打包、词典准备、校验、基准回归、诊断 | `package.py`、`dictionary_bundle_layout.py`、`prepare_dictionary_bundle.py`、`build_pinyin_topn.py`、`verify_dictionary_bundle.py`、`verify_package.py`、`check_query_bench.py`、`collect_diagnostics.ps1`、`benchmark.bat`、`benchmark_topn.ps1`、`run_sync_regression.bat/ps1`、`gen_theme_previews.py` |
| `data/tools/` | **词典数据处理工具**：由 `scripts/` 入口调用，也可独立运行 | `fetch_pinyin_dictionary.py`、`fetch_wubi_dictionary.py`、`convert_rime_dictionary.py`、`build_runtime_dictionary.py`、`generate_pinyin_spellings.py`、`generate_pinyin_syllable_ids.py`、`filter_dictionary_symbols.py`、`generate_symbols.py`、`generate_symbol_ranges.py`、`generate_emoji_classification.py`，以及 `dict_builder/` 实现包 |

脚本通过 `--input`/`--output` 参数接收路径，不依赖环境变量。`scripts/package.py` 经 `scripts/prepare_dictionary_bundle.py` 调用 `data/tools/` 下的词典工具时传入绝对路径：

```python
# prepare_dictionary_bundle.py 内部
cmd = [sys.executable, os.path.join(DATA_TOOLS, "build_runtime_dictionary.py"),
       "--input", db_path, "--output", output_prefix]
```
