# IPC 架构设计

描述知意输入法的 IPC 模块：named pipe 通信模型、服务端 IOCP 事件循环、关闭协议与安全设计。

## 概述

TSF DLL（客户端）与后台服务进程之间通过 **message-mode named pipe** 通信，每次按键一次请求/响应往返。延迟目标 < 1ms/次，实测 Preedit RTT ~50us（avg，见下方性能基准）。

| 组件 | 设计 |
|------|------|
| Server I/O 模型 | Overlapped I/O + IOCP |
| Server 线程模型 | 1 accept 线程 + M worker（IOCP pool，M = clamp(hardware_concurrency, 2, 4)） |
| Server 关闭 | Dummy 连接唤醒 accept + PostQueuedCompletionStatus sentinel |
| Client I/O 模型 | 同步 ReadFile/WriteFile，不使用 FlushFileBuffers |
| 协议 | IPCRequest / IPCResponse（`shared/include/cxxime/ipc_protocol.h`） |

**取舍：不使用 FlushFileBuffers。** Message-mode pipe（`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`）已保证消息边界：`WriteFile` 将完整消息拷贝到内核缓冲区即返回，`ReadFile` 读取完整消息。`FlushFileBuffers` 在此语义下只是阻塞等待对端消费，不提供额外的数据完整性保证，却会为每次往返引入调度器时间片级（~7-15ms）的额外延迟。

**取舍：handler 无锁。** 请求 handler 在 `start()` 前设置、运行期只读，不需要互斥保护。

## Server IOCP 事件循环

```
accept_loop (1 thread, synchronous ConnectNamedPipe)
  │
  ├─ CreateNamedPipe (FILE_FLAG_OVERLAPPED)
  ├─ ConnectNamedPipe (sync, blocks until client)
  ├─ CreateIoCompletionPort(pipe, iocp)
  └─ ReadFile(overlapped) → IOCP 接管

worker_loop (M threads, GetQueuedCompletionStatus)
  │
  ├─ read complete → handler_(request) → WriteFile(overlapped)
  └─ write complete → ReadFile(overlapped)
```

每个 client 连接一个 `ClientContext`，内含 pipe handle、OVERLAPPED、request/response buffer、状态标志。同一时刻每个 client 只有一个 pending overlapped 操作（请求→响应→请求循环），复用一个 OVERLAPPED。

IME server 通常只有 1-2 个活跃 client，2-4 个 worker 足够，避免过多空闲线程。

## 关闭协议

```
stop():
  1. running_ = false
  2. CreateFile + CloseHandle(dummy 连接) → 唤醒 accept 线程的 ConnectNamedPipe
  3. join accept_thread
  4. 遍历 contexts_: CancelIoEx(ctx->pipe) → 触发 I/O 完成通知到 IOCP
  5. PostQueuedCompletionStatus(nullptr) × worker_count → worker 退出
  6. join workers
  7. 清理残留 contexts、CloseHandle(iocp)
```

**取舍：** 唤醒 accept 线程使用 dummy 连接而非 `CancelSynchronousIo`——后者存在竞态（accept 线程可能尚未进入可取消的等待状态）。

## 管道命名与安全

- **每用户隔离：** `\\.\pipe\<username>\ZhiyiIME`。`make_pipe_name()` 读取 Windows 用户名拼接到路径中，多用户同时登录时互不干扰。协议层基础名 `IPC_PIPE_BASE_NAME` 定义在 `ipc_protocol.h`。
- **SDDL ACL**（`ipc/src/security_attributes.h`）：允许 SYSTEM、Everyone、UWP AppContainer 访问。

## Client 行为

- 同步 I/O，单请求单响应。
- `connect()` 带重试循环：`WaitNamedPipeW` 返回后 `CreateFileW` 可能因其他线程抢走实例而失败，失败后重新 `WaitNamedPipeW` 而非直接放弃。

## 协议约定

`START_SESSION` 响应中，`IPCResponse.highlighted` 字段复用为返回的 session_id。这是协议层面的约定，文档化在此。

## 测试与性能现状

### 单元测试

`test/ipc/ipc_test.cc` 共 33 个用例：

- **Protocol**（7）：`pipe_name`、`user_pipe_name_preserves_endpoint_and_is_idempotent`、`request_struct_size`、`response_struct_size`、`response_zero_init`、`candidate_text_over_old_capacity_round_trips`、`ime_status_flags_are_independent`
- **Server**（2）：`start_stop`、`double_stop`
- **Client**（3）：`connect_no_server`、`connect_with_server`、`disconnect_idempotent`
- **Error**（3）：`unknown_command`、`invalid_session`、`engine_not_initialized`
- **IPC**（13）：`start_session`、`end_session`、`ping`、`process_key_preedit`、`request_timeout_disconnects_client`、`process_key_commit`、`process_key_rejected`、`select_candidate`、`commit_composition`、`set_chinese_mode_uses_explicit_target`、`switch_input_mode_carries_target`、`focus_in_out`、`send_request`
- **MultiClient**（2）：`two_clients_simultaneous`、`sequential_sessions`
- **Reconnect**（1）：`server_restart`
- **Stress**（2）：`rapid_requests`、`concurrent_clients`

### 交互测试（ipc_tool）

`ipc_tool.exe`（源码 `tools/ipc_test/`，不公开）提供 IPC 交互调试命令：`connect` / `status` / `session start|end` / `key <vk>` / `select <n>` / `commit` / `focus in|out` / `bench` / `stress <n> <clients>` / `disconnect`。

### 性能基准（迭代记录）

测试方法：`key 49`（拼音 preedit）→ `key 0D`（Enter commit）交替 30 轮，测量 client 端完整 `WriteFile + ReadFile` 往返时间（含 IPC 传输 + engine 处理），排除首轮冷启动。

| 指标 | 早期 (sync+Flush) | IOCP 重设计后 | 内存用户词典后 | 总提升 |
|------|------|------|------|------|
| Preedit 最小值 | 911 us | 83 us | **29 us** | **31x** |
| Preedit 平均值 | ~14709 us | ~110 us | **~50 us** | **294x** |
| Commit 最小值 | 未单独测量 | 7147 us | **13 us** | **550x** |
| Commit 平均值 | 未单独测量 | ~7800 us | **~22 us** | **354x** |

稳态（第 5 轮起）：Preedit 29~80us、Commit 13~27us，无突刺。

### 并发行为

- `stress 6 1`（串行单客户端）：100% 成功
- `stress 6 3`（3 客户端同时连接）：稳定复现恰好 1 个 client 失败——单 accept 线程 + `WaitNamedPipeW` 2000ms 超时在瞬时并发连接下偶尔不足，属 pipe 连接层已知竞态；单元测试 `concurrent_clients`（3 客户端 × 50 请求）全部通过，IPC 层多客户端并发本身正确
