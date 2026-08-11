# Gotchas

跨任务坑清单。改动 Winsock / 线程 / 协议相关代码前必读。

## Winsock / Windows

- **SO_RCVTIMEO 是 DWORD 毫秒**，不是 POSIX 的 timeval 结构（operation.h:112 注释）。
  阻塞 socket 超时表现为 `WSAETIMEDOUT`（`WSAEWOULDBLOCK` 是非阻塞语义，别混用）。
- **shutdown 可解阻塞**：主线程对同 fd 调 `shutdown(fd, SD_BOTH)` 会让收线程阻塞中的 `recv` 立即返回，
  典型错误 `WSAESHUTDOWN`(10058)。清理顺序必须是 stop 置位 → shutdown → join，且泛化错误分支要先查
  `stop`，否则打印噪音。
- **`recv` 返回 0 = EOF**（对端正常 close）；`WSAECONNRESET` = 对端带未读数据关闭触发 RST。
  两者都要在泛化错误分支之前处理（operation.h:249-252, :261-265）。
- MSVC 下 `#pragma comment(lib,"ws2_32.lib")` 自动链接；MinGW 需手动 `-lws2_32`（operation.h:30-32）。
- `windows.h` 必须在 `WinSock2.h` 之后 include（否则拉入 winsock.h 冲突）；`NOMINMAX` 防 min/max 宏污染
  （operation.h:26-29）。

## 线程 / 控制台

- **std::thread 未 join 析构 → std::terminate**。所有 break/return 路径必须汇入统一清理块
  （stop→shutdown→join→closesocket→WSACleanup，main.cpp:165-175）。
- **控制台 stdin 不可轮询**：`std::cin >>` 阻塞且无法在等待期间插入输出。UI 刷新点只能在每次输入
  提示前（先 `drain_inbound` 再打印 "to: "，main.cpp:120-122）——输入等待期间到达的消息要等下次
  输入才显示，这是设计取舍，不是 bug。
- **make_client_msg_id 的 static seq 非原子**：只允许主线程调用（无锁安全的前提）。未来任何线程
  碰它都必须加锁或改 atomic。
- 入站队列满（256）丢**最旧**帧并计数 `dropped_`；计数由主线程 pop 完后统一读取清零
  （operation.h:405-412），避免打印与帧打印交错。

## 协议 / 分帧

- **take_frame 校验次序不可调换**（ocp#1）：body_len 上限检查必须先于帧体完整性检查——否则
  incomplete 会携带未校验的 body_len，acc 可能重新变得无界（operation.h:121-123, :132-138）。
- **PendingMap 依赖定宽 id 序**：`erase(pending.begin())` 的"删最早"成立的前提是字典序 == 时间序，
  即 id 必须定宽 16 位小写 hex（ocp#6）。改动 id 格式会悄悄破坏淘汰语义。
- **自发自收顺序**：先收 (self) 副本（opcode=request）后收 response——drain_inbound 按此顺序先打
  "(self) ..." 再打 "delivered"。
- `client_msg_id_` 整体回显、无配对序号：响应匹配只能靠 PendingMap 的 id 查表；未匹配响应只打诊断行。
- 服务端**零心跳义务**；`kWriteHighWater=1MiB` 背压杀连接；畸形帧/超大 body_len 杀连接零响应；
  未登录连接发非登录帧静默丢弃。客户端不加应用层心跳是对的，不要"好心"补上。
- **登录残留帧处理**：pre_login 解码 login 响应成功后，同批残留完整帧入队（收线程启动后消费），
  残帧直接丢弃——登录态边界外无解码上下文（operation.h:334-349）。
- **登录重试最坏等 5s**：pre_login 内阻塞 recv 超时 kRecvTimeoutSec=5s（operation.h:291）——仅在
  服务端无响应时触发；正常 reject 响应即时到达。

## 工程 / 仓库

- **.idea/vcs.xml 误 staged 教训**：CLion 自动生成的 vcs.xml 不在 baseline，IDE 一碰就出现在 git
  status；提交前检查 `.idea/`。`.idea/` 从 baseline 起整体被 track（历史遗留），是否改 ignore 需
  sensei 定夺，不要顺手改。
- **common/ 是共享协议层**：并发改造未动 common/ 任何文件；改协议（编解码/帧头）前先确认服务端
  仓库与本地 common/ 的同步关系。
