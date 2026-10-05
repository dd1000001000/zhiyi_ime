# 可观测性与 Benchmark

## 目标

先建立稳定的性能和行为基线。所有后续优化必须回答：

- 某个输入为什么慢
- 候选来自哪个 producer
- 扫描了多少索引项
- 是否命中缓存
- 是否被 deadline 截断
- 是否因 revision 过期被丢弃

## QueryTrace 数据模型

轻量 trace 结构，定义在 `shared/include/cxxime/query_trace.h`，包含状态字段（`deadline_exceeded`、`truncated`、`cache_hit`、`cancelled`）、计数字段和耗时字段。

### should_log() 采样策略

分级采样，避免普通查询全量写日志。策略由 `default.json` 的 `diagnostics` 段控制：

| 条件 | 采样率 | 说明 |
|------|--------|------|
| `deadline_exceeded` 或 `cancelled` | 100% | 异常查询强制记录 |
| `total_us >= 30ms` | 100% | 慢查询强制记录 |
| `!cache_hit && total_us >= 10ms` | 100% | 缓存未命中且较慢 |
| `truncated` | 默认 1% | 被截断的查询（采样） |
| 其他正常查询 | 默认关闭 | 普通快速输入默认不采样 |

采样函数 `should_sample(session_id, revision, query_id, rate)` 使用 mix64 哈希，同输入同结果（确定性）。


### diagnostics 配置

```json
"diagnostics": {
    "trace_mode": "normal",
    "log_max_size": 8388608,
    "log_max_files": 4,
    "normal_sample_rate": 0,
    "truncated_sample_rate": 100,
    "slow_query_us": 30000,
    "cache_miss_slow_us": 10000,
    "slow_ipc_us": 2000,
    "slow_window_us": 5000,
    "slow_total_us": 10000
}
```

`trace_mode` 取值:

- `off`：完全关闭JSONL trace写入。
- `error`：只记录失败、超时、取消等错误路径。
- `normal`：记录错误、慢路径和少量截断采样。
- `verbose`：短期复现问题时使用,记录所有 trace。

### log() 与 log_unchecked() 拆分

`QueryTrace` 提供两个写入入口：

- **`log()`**：先调用 `should_log()` 进行采样判断，只有通过采样条件才写入。Engine、TSF 等上游组件使用此入口，避免在热路径中做不必要的 I/O。
- **`log_unchecked()`**：跳过 `should_log()` 采样判断，直接写入。适用于调用方已经判定 `should_log()` 通过的场景（例如服务端在 IPC 请求完成后已确认需要记录），避免重复计算采样条件。

### 单一日志出口

Engine 只填充 `last_trace()` 字段，**不自动调用** `trace_.log()`。ServerApp 在 IPC 请求完成后作为唯一出口调用 `trace_.log()`（已判定 should_log 后直接 `log_unchecked()`）。Tools/tests 使用 Engine 自包含模式时可显式调用。

详见 [查询预算与候选收集](query-control.md)。

要求：

- Release 默认低噪声记录，用户可以通过 `trace_mode=off` 关闭记录。
- 字符串字段限制长度，例如 `raw_input` 最多 128 字节。
- 采样率和慢路径阈值由 `diagnostics` 配置配置。

### CXXIME_LOG 热路径日志

release 构建下 `CXXIME_LOG` 宏展开为空操作（`do {} while (0)`），避免热路径 `OutputDebugStringW` 开销：

```cpp
// shared/include/cxxime/logging.h
#if defined(_DEBUG) || defined(CXXIME_ENABLE_TRACE_LOG)
#define CXXIME_LOG(fmt, ...) cxxime::debug_log(fmt, __VA_ARGS__)
#else
#define CXXIME_LOG(fmt, ...) do {} while (0)
#endif
```

## 插桩点

### TSF

文件：`tsf/src/text_service.cpp`

记录：

- key down/up 收到时间
- `_client.process_key()` 调用耗时
- candidate window `update/move/show/paint` 耗时
- 最新 accepted revision

### IPC Client/Server

文件：

- `ipc/src/ipc_client.cc`
- `server/src/server_app.cc`

记录：

- request/response 往返耗时
- IPC 命令类型
- session id、revision
- response 中 preedit/candidate 数量

### Engine

文件：

- `engine/src/engine.cc`
- `engine/src/pinyin_translator.cc`
- `engine/src/dict.cc`

记录：

- processor 耗时
- syllabifier 路径数量
- `has_prefix()` 命中数量
- `lookup_by_ids()` 每条路径扫描量
- 最终候选数量

## 日志格式

JSONL trace 输出，便于脚本分析：

```json
{"q":42,"sid":1,"rev":18,"input":"sdf","page":0,"page_size":9,"paths":12,"live":4,"candidates":7,"exact_scan":0,"prefix_scan":256,"user_scan":0,"cache":false,"deadline":false,"cancelled":false,"truncated":false,"proc_us":50,"trans_us":9200,"lookup_us":8800,"merge_us":100,"total_us":9300}
```

## 异步日志队列

trace 写入通过 lock-free MPSC 队列异步化，不阻塞按键路径：

```
Engine::process_key() → trace_.log()
  → TraceQueue::try_push()   // 非阻塞，队列满时丢弃
  → writer_thread_func()     // 后台批量写入 JSONL
```

### MPSCQueue

lock-free 多生产者单消费者队列，定义在 `shared/include/cxxime/mpscq.h`，实现 `shared/src/mpscq.cc`。基于 atomic CAS 操作，源自 gRPC MPSCQ。

### TraceQueue

MPSCQueue 的使用层包装，增加长度限制：

- `kQueueCapacity = 256`：队列容量上限
- `size_` 原子计数器：每 push +1，每 pop -1（实时准确）
- 超过容量时 `try_push()` 返回 false 并递增 `dropped_` 计数
- `kBatchSize = 32`：writer 线程批量消费
- `kFlushInterval = 100ms`：刷新间隔
- mutex+cv 仅用于 writer 线程休眠唤醒（不保护队列数据）

### 日志轮转

- 单文件上限由 `diagnostics.log_max_size` 控制，默认8 MiB。
- 保留代数由 `diagnostics.log_max_files` 控制，默认 4代。
- 日志目录总量超过配置派生上限时，按最后写入时间清理旧的 `.log` / `.jsonl` 文件。
- `tsf-*` 临时日志保留 7 天

## TSF 事件级追踪

TSF 层（`tsf/src/text_service.cpp`）拥有独立于 QueryTrace 的追踪系统，写入独立日志文件，不经过服务端 QueryTrace 采样路径。

### 日志路径与格式

文件写入 `%USERPROFILE%\zhiyi\logs\tsf-<pid>-trace.jsonl`，每行一个 JSON 事件：

```json
{"event":"ipc_session","detail":"ready","session":1,"focused":true,"chinese":true,"caps":false,"fg":"Notepad"}
```

### 独立队列与写线程

- **环形缓冲区**（非 MPSCQ）：容量 128 条，单生产者单消费者原子头尾索引
- **批量消费**：每批 16 条，200ms 刷新间隔
- **独立写线程**：TsfTrace 和事件追踪共享同一队列，不经过 QueryTrace 的锁自由队列和采样逻辑
- **日志轮转**：与 QueryTrace 共用 `diagnostics.log_max_size` / `log_max_files` 配置

### 事件类型

| 事件 | detail 示例 | 触发时机 |
|------|------------|----------|
| `ipc_session` | `connect_failed`, `start_failed`, `ready`, `recreated`, `heartbeat_reconnect_failed`, `heartbeat_invalid_session`, `heartbeat_failed` | IPC 连接/重连/心跳 |
| `ui_presentation` | `show:key_edit_target`, `hide:edit_target_clear` 等原因字符串 | 输入目标变化后刷新界面呈现时传递的 reason 参数 |
| `candidate_window` | 显隐原因字符串 | 候选窗口显示或隐藏时传递的 reason 参数 |
| `input_context` | `allowed` 或拒绝原因（如 `context_disabled`, `no_focus` 等） | `_trace_input_decision()` 在上下文状态变化时调用，重复原因自动去重 |

### 过滤控制

受 `diagnostics.trace_mode`（`DiagnosticTraceMode`）控制：

| 模式 | 行为 |
|------|------|
| `off` | 不记录任何事件，`tsf_should_log_event()` 返回 false |
| `error` | 仅记录 `important = true` 的事件（连接失败、会话重建、心跳异常等） |
| `normal` / `verbose` | 记录所有事件 |

### 关键代码

```
text_service.h:    void _enqueue_event_trace(const char* event, const char* detail, bool important = false);
text_service.cpp:  tsf_should_log_event() → tsf_queue_try_push() → tsf_writer_thread_func()
```

## query_bench 工具

命令行工具 `tools/query_bench/`，用于离线性能测试（测试与调试工具不在公开仓库中）。

命令：

```cmd
query_bench.exe --data cxx-ime\data --input s,sd,sdf,sddf,bj,srf,shrf,zguo,nihaoshijie --repeat 1000
query_bench.exe --data cxx-ime\data --file cases.txt --json trace.jsonl --repeat 1000
query_bench.exe --data cxx-ime\data --input s,sd,sdf,sddf,bj,srf,shrf,zguo,nihaoshijie --repeat 1000 --trace-log
```

`--trace-log` 启用时输出 `log%` 列，显示 `should_log()` 触发率。

输出：

```text
Input                e2e_p50  e2e_p95  e2e_p99  max_us   qry_p50  qry_p95  cands  exact_p95  prefix_p95  user_p95  paths  trunc%  deadline%
s                       1200     1800     2200    25000      800     1200      7          0         128         0      4    0.0%      0.0%
sd                      2100     3200     4100    30000     1500     2500      7          0         250         0      4    0.0%      0.0%
```

## 回归输入集

基础集：

```text
s
sd
sdf
sddf
bj
srf
shrf
zguo
nihao
nihaoshijie
woxiangshuruyiduanhenchangdepinyin
```

行为集：

- 快速连续输入
- 连续 Backspace
- Shift 中英文切换后输入
- 无效尾部：`nihaoxxxq`
- 多 session 并发

## 验收

- 每个输入可输出 P50/P95/P99
- trace 能定位 `s` 和 `sddf` 的路径数、扫描量和候选来源
- 插桩开启后性能开销 P95 < 3%

## 性能基线

新功能开发或重构后，用下面的命令重跑，与改动前的结果对比确认无回归（测试与调试工具不在公开仓库中）。

### 重跑基准

```cmd
# 离线查询 benchmark（无需 server）
build\tools\query_bench\query_bench.exe --data data --input s,sd,sdf,sddf,bj,srf,shrf,zguo,nihao,nihaoshijie --repeat 500

# IPC 端到端 benchmark（需先启动 server）
scripts\benchmark.bat

# 单元 benchmark
build\test\benchmark_test.exe
```
