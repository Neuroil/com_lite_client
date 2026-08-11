# com-lite-client 系统档案

## 概览

Winsock TCP 聊天客户端，已从单线程同步收发原型改造为**并发收发**（收线程 + 主线程 UI）。
Windows-only，C++（无 C++17 特有依赖），CMake 构建（MSVC/CLion；MinGW 需自行加 -lws2_32）。
服务端为独立仓库（不在本仓库内），协议事实见「协议行为事实」。

- 改造 baseline：`c38115f5`（单线程同步收发原型快照）
- 改造范围：仅 `operation.h`、`main.cpp`；`common/` 与 `CMakeLists.txt` 未动
- 当前状态：实现完成、code-review passed、本机冒烟通过

## 架构

```
main.cpp              operation.h                       common/
┌───────────┐         ┌───────────────────────────┐    ├─ frame_header.h  帧头布局/大小
│ 主线程 UI │───────▶ │ send_all / encode_frame    │    ├─ message.h       消息结构/body 上限
│ 输入+排空 │         │ take_frame / InboundQueue  │    ├─ encoder/decoder JSON 编解码
│ PendingMap│         │ recv_loop（收线程主体）     │    ├─ message_pack.h
└─────┬─────┘         └───────────────────────────┘    └─ binary_code.h / io_status.h
      │ fd (send / shutdown)
      ▼
 Winsock TCP ──▶ 服务端（独立仓库，含 kWriteHighWater 背压）
```

要点：**两线程共享一个 socket**——收线程独占 `recv()`，主线程独占 `send()` 与 `shutdown()`；
Winsock 允许跨线程 fd 操作，内核负责串行化。

## 线程模型

- **收线程**（`recv_loop`，operation.h:205-273）：阻塞 `recv` 循环，超时片 `kRecvTickMs=500ms`（operation.h:47），
  分帧（`take_frame`）→ 解码（`Decoder::decode`）→ 入队（`InboundQueue::push`）。线程私有缓冲
  `acc`/`frame`/`chunk`。退出：`stop` 置位、EOF（:249-252）、RST（:261-265）、缓冲/帧超限、解码/recv 错误。
- **主线程**（main.cpp:120-163）：`drain_inbound` 排空队列分类打印 → `std::cin` 读 to/content →
  发送前复查连接状态（main.cpp:139-142）→ 生成 id、写 PendingMap、发送。**UI 刷新点只在每次输入
  提示前**（stdin 阻塞不可中断，见 gotchas）。
- **登录阶段**（main.cpp:94-112）：收线程**启动前**由主线程阻塞收 login 响应（`pre_login`），成功返回后
  启动收线程（main.cpp:117-118，避免双线程抢收）。登录失败可无限重试（决策 6），q 退出。

### 线程所有权表

| 数据 | 收线程 | 主线程 | 同步 |
| --- | --- | --- | --- |
| `InboundQueue::frames_` / `dropped_` | `push`（operation.h:156） | `pop`/`size`（:166/:173）；dropped 读取清零（:405-412） | `mutex_` |
| `stop`（atomic\<bool\>） | 读（:218, :255） | 写（main.cpp:170） | atomic |
| `state`（atomic\<ConnState\>） | 写（:224/:235/:241/:250/:262/:269） | 读（main.cpp:120, :139） | atomic |
| `PendingMap pending` | **严禁触碰**（operation.h:203-204 注释） | 读写（main.cpp:145-148；drain_inbound :371-413） | 无锁（主线程独占） |
| `acc`/`frame`/`chunk` | 独占 | — | 线程私有 |
| socket fd | `recv` | `send` / `shutdown` | 内核串行化 |

## 关键接口

| 接口 | 位置 | 职责 |
| --- | --- | --- |
| `FrameResult take_frame(acc, out)` | operation.h:125 | 纯分帧器；校验次序钉死（ocp#1）：body_len 上限先于完整性；incomplete 不改 acc |
| `InboundQueue` | operation.h:149 | 收→主唯一共享通道；满 256 丢最旧 + `dropped_` 计数 |
| `enum ConnState` | operation.h:182 | connected / peer_closed / peer_reset / recv_error / buf_overflow |
| `recv_loop(fd, q, stop, state)` | operation.h:205 | 收线程主体 |
| `make_client_msg_id()` | operation.h:186 | `(GetTickCount64()<<20) \| (seq++ & 0xFFFFF)`，定宽 16 位小写 hex（ocp#6） |
| `PendingMap`（map\<string, PendingEntry\>） | operation.h:201 | 待确认响应表，有界 256，仅主线程 |
| `pre_login(fd, uid, credential, q, out_rmp)` | operation.h:278 | 登录：阻塞收恰好一帧 + 残留完整帧入队 / 残帧丢弃 |
| `login_as(fd, q, uid, credential)` | operation.h:354 | 登录封装；status!=ok → rejected |
| `drain_inbound(q, pending, self_uid)` | operation.h:371 | 主线程分类打印；response 匹配 pending 后 erase |

## 协议行为事实

以下为隔壁服务端仓库查证结果 [外部查证]，客户端打印分支据此设计：

- request → **一帧 response（opcode=2）**，无中间 ack。
- ack 是**独立双向通道**：发 ack → 服务端原样回显 body=0x01（客户端当前不主动发 ack）。
- **无主动推送**；转发帧以 request opcode 原样到达目标。
- **自发自收顺序**：先收 (self) 副本（opcode=request，from_uid==自己），后收 response。
- `client_msg_id_` **整体回显**，是唯一关联键（无配对序号）——PendingMap 以其为 key。
- 服务端**零心跳义务**；`kWriteHighWater=1MiB` 背压杀连接（客户端 `kMaxAccBytes` 对齐此语义）。
- 畸形帧 / 超大 body_len → 杀连接零响应。
- 登录失败**可无限重试不杀连接**；未登录连接发非登录帧**静默丢弃**。

## 决策记录

| 编号 | 决策 | 依据位置 |
| --- | --- | --- |
| 决策 2 | 无应用层心跳，仅 TCP keepalive | main.cpp:67 注释 |
| 决策 6 | 登录无限重试直到成功（q 退出） | main.cpp:94 注释（服务端不杀连接） |
| ocp#1 | take_frame 校验次序：body_len 上限先于完整性 | operation.h:121-123 |
| ocp#2 | 统一清理块：stop→shutdown→join→closesocket→WSACleanup | main.cpp:165-175 |
| ocp#3 | pre_login 消费恰好一帧 + 残留入队/残帧丢弃 | operation.h:276-277 |
| ocp#6 | 定宽 16 位 hex id → 字典序即时间序（PendingMap erase(begin()) 依赖） | operation.h:184-185 |
| ocp#7 | 线程所有权边界：收线程只碰 InboundQueue；PendingMap 仅主线程 | operation.h:203-204 |

注：代码注释引用 ocp#1/2/3/6/7；ocp#4/5 未见于代码注释（完整清单需查对应 blueprint 归档）。

## 未决事项

1. PendingMap 条目无超时回收：响应永不回来时条目只靠 256 上限溢出驱逐清理。
2. `.idea/` 从 baseline 起整体被 track（历史遗留），是否改 ignore 需 sensei 定夺，不要顺手改。

## 文件地图（改造后）

- `main.cpp`：入口、连接、登录重试、主循环、统一清理块
- `operation.h`：全部协议/线程工具（header-only）
- `common/`：共享协议层（编解码/帧头/消息），改造未动
