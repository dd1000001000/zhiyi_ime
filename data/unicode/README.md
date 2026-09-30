# Unicode 分类数据原始输入

`17.0/` 内的三份官方文件纳入版本管理，供 `generate_emoji_classification.py` 离线再生成。
总计约 2.84 MiB，仅作为维护和测试输入；不会加载到输入法进程或复制进安装包。
普通词典构建仍直接使用冻结的 `tools/dict_builder/emoji_classification.json`。

## 官方下载地址

| 本地文件 | 官方下载地址 | 用途 |
|---|---|---|
| `17.0/emoji-test.txt` | [emoji-test.txt](https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt) | 完整 emoji 序列及分组 |
| `17.0/emoji-data.txt` | [emoji-data.txt](https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt) | Emoji 等码点属性 |
| `17.0/UnicodeData.txt` | [UnicodeData.txt](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt) | Unicode 通用类别，提取 P/S |

保留官方文件原始字节，不手工修订内容或转换换行。
`.gitattributes` 禁止这三类输入的 Git 文本换行转换，避免 Windows 检出改变哈希。
三个预期 SHA-256 固定在生成器的 `SOURCES` 中；生成的 JSON 也记录来源及哈希。

## 生成与验证

从项目根目录运行：

```bat
python data/tools/generate_emoji_classification.py
python data/tools/generate_symbol_ranges.py --source data/tools/dict_builder/emoji_classification.json --output shared/src/symbol_ranges.inc
python test/data/unicode_classification_generator_test.py
python test/data/symbol_catalog_test.py
```

生成器默认读取本目录 `17.0/`，无需准备仓库外文件，也不会访问网络。
测试会从三份原始输入重新生成临时 JSON，并与仓库中的分类表逐字节比较；
范围测试同样验证 `symbol_ranges.inc` 可再生。
缺失输入或哈希不符会在打开输出文件前报错。需要重新取得原始文件时，
从上表地址下载到对应路径，再运行生成器校验；不能修改预期哈希来绕过损坏检测。
