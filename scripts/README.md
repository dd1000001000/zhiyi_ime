# 构建与发布脚本

`scripts/` 保存跨模块的构建、打包、数据校验、基准测试和诊断收集入口。词典源数据及
`data/tools/` 内部转换工具见 [`data/README.md`](../data/README.md)。

## 发布流水线

正式打包统一使用：

```bat
python scripts\package.py
```

`package.py` 负责构建程序、准备词典 bundle、复制项目与第三方许可证、验证数据并生成安装包。
常用参数：

```bat
python scripts\package.py --fast
python scripts\package.py --fast --host-diag
python scripts\package.py --skip-build
python scripts\package.py --skip-dict
python scripts\package.py --output-dir <path>
```

修改词典源数据、符号清单、候选排序或索引格式后，正式打包不得使用 `--skip-dict`。

## 发布更新（设置 > 更新）

设置程序从 GitHub 最新 Release 读取 `latest.json` 和 `latest.json.sig`，签名正确、安装包
SHA-256 一致才会安装（`update/include/cxxime/update.h`）。

1. 修改 `VERSION`，在 `docs/release-notes/<版本>.zh-CN.md` 和 `<版本>.en-US.md` 写更新说明（可选）。
2. `python scripts\package.py`：签名密钥存在时，在输出目录生成 `latest.json` 和 `latest.json.sig`。
3. 在 GitHub 新建 Release，标签为 `v<版本>`（例如 `v0.6.3`），上传安装包、`latest.json`、
   `latest.json.sig` 三个文件，发布为正式版本（不是 Pre-release），它就成为 latest。

签名私钥默认在 `%USERPROFILE%\.zhiyi-release\update-signing.pem`（或环境变量
`ZHIYI_UPDATE_KEY`），**不能提交到仓库，并且要备份**：丢失后只能发布一个需要用户手动安装、
内置新公钥（`update_signing.py pubkey`）的版本。只在新机器上第一次使用时运行
`python scripts\update_signing.py genkey`。

## 脚本职责

| 脚本 | 职责 |
|------|------|
| `package.py` | 完整构建和发布打包入口 |
| `update_signing.py` | 更新签名密钥，生成并签名 `latest.json` |
| `dictionary_bundle_layout.py` | 运行时词典文件、manifest 角色及双拼变体的共享清单 |
| `prepare_dictionary_bundle.py` | 并行准备拼音、五笔运行时词典并生成 manifest |
| `build_pinyin_topn.py` | 生成供 `topn_builder` 使用的拼音 Top-N 中间索引 |
| `verify_dictionary_bundle.py` | 校验运行时词典、索引及 manifest 的一致性 |
| `verify_package.py` | 发布包静态校验入口，仅负责组织各类检查 |
| `package_checks/` | 发布文件、词典、诊断和安装器的分模块检查 |
| `benchmark.bat` | 运行主要性能基准 |
| `benchmark_topn.ps1` | 运行拼音 Top-N 专项基准 |
| `check_query_bench.py` | 检查查询基准结果是否满足阈值 |
| `run_sync_regression.bat` | 在 cmd 中运行同步和数据回归检查 |
| `run_sync_regression.ps1` | 在 PowerShell 中运行同步和数据回归检查 |
| `collect_diagnostics.ps1` | 收集已安装版本的诊断信息 |
| `zhiyi-setup.nsi` | NSIS 安装与卸载流程入口 |
| `nsis/` | NSIS 初始化、事务、锁检查、TSF 注册和文件处理模块 |

## 词典打包阶段

词典 bundle 的正式生成顺序为：

```text
独立符号清单 + 源词典
  -> prepare_dictionary_bundle.py
  -> 独立生成 symbols.json，并过滤临时词典中的系统符号
  -> 拼音 Top-N 构建中间文件
  -> topn_builder 绑定 pinyin.dict.bin 并生成共享候选索引
  -> 为拼音和五笔 dict.bin 生成 Settings 反向索引
  -> dictionary_manifest.json
  -> verify_dictionary_bundle.py
```

单独运行词典准备脚本时必须提供 `topn_builder`：

```bat
python scripts\prepare_dictionary_bundle.py ^
    --data-dir data --output-dir dist\data ^
    --topn-builder build\tools\topn_index\Release\topn_builder.exe
```
