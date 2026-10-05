# 构建与数据脚本

`scripts/` 保存构建依赖下载、词典 bundle 生成与诊断收集脚本。词典源数据及 `data/tools/` 内部转换工具见
[`data/README.md`](../data/README.md)。打包、发布与测试工具不在公开仓库中。

## 脚本

| 脚本 | 职责 |
|------|------|
| `fetch_onnxruntime.py` | 下载 ONNX Runtime 到 `third_party/onnxruntime/` |
| `fetch_model.py` | 从 GitHub Release 下载 Laya 模型到 `models/laya/` |
| `dictionary_bundle_layout.py` | 运行时词典文件与 manifest 角色的共享清单 |
| `prepare_dictionary_bundle.py` | 并行准备拼音、五笔运行时词典并生成 manifest |
| `build_pinyin_topn.py` | 生成供 `topn_builder` 使用的拼音 Top-N 中间索引 |
| `verify_dictionary_bundle.py` | 校验运行时词典、索引及 manifest 的一致性 |
| `collect_diagnostics.ps1` | 收集已安装版本的诊断信息（随安装包分发） |

## 生成词典

词典 bundle 的生成顺序：

```text
独立符号清单 + 源词典
  -> prepare_dictionary_bundle.py
  -> 独立生成 symbols.json，并过滤临时词典中的系统符号
  -> 拼音 Top-N 构建中间文件
  -> topn_builder 绑定 pinyin.dict.bin 并生成共享候选索引
  -> 为拼音和五笔 dict.bin 生成反向索引
  -> dictionary_manifest.json
  -> verify_dictionary_bundle.py
```

`topn_builder` 由正常构建生成（`tools/topn_index`）。单独运行词典准备脚本：

```bat
python scripts\prepare_dictionary_bundle.py ^
    --data-dir data --output-dir dist\data ^
    --topn-builder build\tools\topn_index\topn_builder.exe
```
