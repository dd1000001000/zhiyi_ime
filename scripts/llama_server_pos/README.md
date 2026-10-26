# llama-server 词性补丁

离线翻译模型从第 4 版 1026 起带一个**词性头**：读翻译模型第 6、8、10、12、14 层在候选最后一个 token 上的隐藏状态，
判断候选在上文里的词性（Universal Dependencies 的 17 个词性，加 CLF 量词、ONOM 拟声词、NONE 短语或句子）。
官方 llama-server 拿不到中间层，这里的补丁让它在翻译的同时算出词性，输入法显示在译文前（如 `adv./adj. fast`，
见 [docs/learning-mode.md](../../docs/learning-mode.md)）。

补丁基于 llama.cpp **b11480**，许可同 llama.cpp（MIT）。

## 做了什么

- 词性头作为元数据存在模型 GGUF 里（`zhiyi.pos_head.layers`、`tags`、`mean`、`std`、`fc1.weight`、`fc1.bias`、
  `fc2.weight`、`fc2.bias`），约 10 MB；不认识这些键的 llama.cpp 照常加载、照常翻译。
- 加载到词性头时，服务端用 eval callback 读 `l_out-5/7/9/11/13` 里各条序列最后一个提示词 token 的那一行
  （`server-pos.h`、`server-pos.cpp`）。为了让回调里的行号就是批次里的位置，改用统一 KV 缓存
  （`kv_unified`，按原顺序切分）并让 `n_ubatch = n_batch`。
- `/completion` 请求带 `"pos": true` 时，结果多出 `pos`、`pos_prob`（最可能的词性和概率）和 `pos_top`
  （概率至少 1% 的最多 5 个，`[{"tag": "ADV", "prob": 0.58}, ...]`）。不带时和原来一样。
- 代价：每次解码要从显卡读回五层，RTX 5070 Ti 上一页（10 条）多约 20 ms。

## 重现

```bat
git clone https://github.com/ggml-org/llama.cpp && cd llama.cpp && git checkout b11480
python <知意>\scripts\llama_server_pos\apply_patch.py .
<知意>\scripts\llama_server_pos\build.bat .
```

- `apply_patch.py`：把 `server-pos.h/.cpp` 复制进 `tools/server`，改 `CMakeLists.txt`、`server-context.cpp`、
  `server-task.h/.cpp`、`server-schema.cpp` 五个文件（每处锚点必须正好出现一次，在干净的 b11480 上运行一次）。
- `build.bat`：Visual Studio + Ninja 编译 `llama-server`，只用其中的 `llama-server.exe`、`llama-server-impl.dll`、
  `llama-common.dll`、`mtmd.dll`；`llama.dll`、`ggml*.dll`（Vulkan 与各 CPU 版本）仍用官方 b11480 发布的。
- `make_runtime.py`：把这四个文件和 VC 运行库换进官方运行库包，得到 `llama-b11480-zhiyi-pos-vulkan-x64.zip`。
- `add_pos_head.py`：把模型发布里的 `pos_head/`（`pos_head.json`、`pos_head.safetensors`）写进 GGUF 的副本。

翻译模型、词性头的训练和数据不在仓库里；模型和词性头见 GitHub 的 `translator` 发布。
